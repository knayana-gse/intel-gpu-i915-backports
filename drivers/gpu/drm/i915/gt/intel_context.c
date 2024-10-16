// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_pm.h"

#include "i915_drv.h"
#include "i915_trace.h"
#include "i915_suspend_fence.h"

#include "intel_context.h"
#include "intel_engine.h"
#include "intel_engine_pm.h"
#include "intel_lrc_reg.h"
#include "intel_ring.h"

static struct kmem_cache *slab_ce;

static struct intel_context *intel_context_alloc(void)
{
	return kmem_cache_zalloc(slab_ce, GFP_KERNEL);
}

static void rcu_context_free(struct rcu_head *rcu)
{
	struct intel_context *ce = container_of(rcu, typeof(*ce), rcu);

	trace_intel_context_free(ce);
	kmem_cache_free(slab_ce, ce);
}

void intel_context_free(struct intel_context *ce)
{
	call_rcu(&ce->rcu, rcu_context_free);
}

struct intel_context *
intel_context_create(struct intel_engine_cs *engine)
{
	struct intel_context *ce;

	ce = intel_context_alloc();
	if (!ce)
		return ERR_PTR(-ENOMEM);

	intel_context_init(ce, engine);
	trace_intel_context_create(ce);
	return ce;
}

int intel_context_alloc_state(struct intel_context *ce)
{
	int err = 0;

	if (mutex_lock_interruptible(&ce->pin_mutex))
		return -ERESTARTSYS;

	if (!test_bit(CONTEXT_ALLOC_BIT, &ce->flags)) {
		if (intel_context_is_banned(ce)) {
			err = -EIO;
			goto unlock;
		}

		err = ce->ops->alloc(ce);
		if (unlikely(err))
			goto unlock;

		set_bit(CONTEXT_ALLOC_BIT, &ce->flags);
	}

unlock:
	mutex_unlock(&ce->pin_mutex);
	return err;
}

static int intel_context_active_acquire(struct intel_context *ce)
{
	int err;

	__i915_active_acquire(&ce->active);

	if (intel_context_is_barrier(ce) || intel_engine_uses_guc(ce->engine) ||
	    intel_context_is_parallel(ce))
		return 0;

	/* Preallocate tracking nodes */
	err = i915_active_acquire_preallocate_barrier(&ce->active,
						      ce->engine);
	if (err)
		i915_active_release(&ce->active);

	return err;
}

static void intel_context_active_release(struct intel_context *ce)
{
	/* Nodes preallocated in intel_context_active() */
	i915_active_acquire_barrier(&ce->active);
	i915_active_release(&ce->active);
}

static int __context_pin_state(struct i915_vma *vma, struct i915_gem_ww_ctx *ww)
{
	int err;

	err = i915_ggtt_pin_for_gt(vma, ww);
	if (err)
		return err;

	err = i915_active_acquire(&vma->active);
	if (err)
		goto err_unpin;

	/*
	 * And mark it as a globally pinned object to let the shrinker know
	 * it cannot reclaim the object until we release it.
	 */
	i915_vma_make_unshrinkable(vma);
	return 0;

err_unpin:
	i915_vma_unpin(vma);
	return err;
}

static void __context_unpin_state(struct i915_vma *vma)
{
	i915_vma_make_shrinkable(vma);
	i915_active_release(&vma->active);
	__i915_vma_unpin(vma);
}

static int __ring_active(struct intel_ring *ring,
			 struct i915_gem_ww_ctx *ww)
{
	int err;

	err = intel_ring_pin(ring, ww);
	if (err)
		return err;

	err = i915_active_acquire(&ring->vma->active);
	if (err)
		goto err_pin;

	return 0;

err_pin:
	intel_ring_unpin(ring);
	return err;
}

static void __ring_retire(struct intel_ring *ring)
{
	i915_active_release(&ring->vma->active);
	intel_ring_unpin(ring);
}

static int intel_context_pre_pin(struct intel_context *ce,
				 struct i915_gem_ww_ctx *ww)
{
	int err;

	CE_TRACE(ce, "active\n");

	err = __ring_active(ce->ring, ww);
	if (err)
		return err;

	err = intel_timeline_pin(ce->timeline, ww);
	if (err)
		goto err_ring;

	if (!ce->state)
		return 0;

	err = __context_pin_state(ce->state, ww);
	if (err)
		goto err_timeline;


	return 0;

err_timeline:
	intel_timeline_unpin(ce->timeline);
err_ring:
	__ring_retire(ce->ring);
	return err;
}

static void intel_context_post_unpin(struct intel_context *ce)
{
	if (ce->state)
		__context_unpin_state(ce->state);

	intel_timeline_unpin(ce->timeline);
	__ring_retire(ce->ring);
}

int __intel_context_do_pin_ww(struct intel_context *ce,
			      struct i915_gem_ww_ctx *ww)
{
	bool handoff = false;
	void *vaddr;
	int err = 0;

	if (unlikely(!test_bit(CONTEXT_ALLOC_BIT, &ce->flags))) {
		err = intel_context_alloc_state(ce);
		if (err)
			return err;
	}

	/*
	 * We always pin the context/ring/timeline here, to ensure a pin
	 * refcount for __intel_context_active(), which prevent a lock
	 * inversion of ce->pin_mutex vs dma_resv_lock().
	 */

	err = i915_gem_object_lock(ce->timeline->hwsp_ggtt->obj, ww);
	if (!err && ce->ring->vma->obj)
		err = i915_gem_object_lock(ce->ring->vma->obj, ww);
	if (!err && ce->state)
		err = i915_gem_object_lock(ce->state->obj, ww);
	if (!err)
		err = intel_context_pre_pin(ce, ww);
	if (err)
		return err;

	err = ce->ops->pre_pin(ce, ww, &vaddr);
	if (err)
		goto err_ctx_unpin;

	err = i915_active_acquire(&ce->active);
	if (err)
		goto err_post_unpin;

	err = mutex_lock_interruptible(&ce->pin_mutex);
	if (err)
		goto err_release;

	intel_engine_pm_might_get(ce->engine);

	if (unlikely(intel_context_is_closed(ce))) {
		err = -ENOENT;
		goto err_unlock;
	}

	if (likely(!atomic_add_unless(&ce->pin_count, 1, 0))) {
		err = intel_context_active_acquire(ce);
		if (unlikely(err))
			goto err_unlock;

		err = ce->ops->pin(ce, vaddr);
		if (err) {
			intel_context_active_release(ce);
			goto err_unlock;
		}

		CE_TRACE(ce, "pin ring:{start:%08x, head:%04x, tail:%04x}\n",
			 i915_ggtt_offset(ce->ring->vma),
			 ce->ring->head, ce->ring->tail);

		handoff = true;
		smp_mb__before_atomic(); /* flush pin before it is visible */
		atomic_inc(&ce->pin_count);
	}

	GEM_BUG_ON(!intel_context_is_pinned(ce)); /* no overflow! */

	trace_intel_context_do_pin(ce);

err_unlock:
	mutex_unlock(&ce->pin_mutex);
err_release:
	i915_active_release(&ce->active);
err_post_unpin:
	if (!handoff)
		ce->ops->post_unpin(ce);
err_ctx_unpin:
	intel_context_post_unpin(ce);

	/*
	 * Unlock the hwsp_ggtt object since it's shared.
	 * In principle we can unlock all the global state locked above
	 * since it's pinned and doesn't need fencing, and will
	 * thus remain resident until it is explicitly unpinned.
	 */
	i915_gem_ww_unlock_single(ce->timeline->hwsp_ggtt->obj);

	return err;
}

int __intel_context_do_pin(struct intel_context *ce)
{
	struct i915_gem_ww_ctx ww;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = __intel_context_do_pin_ww(ce, &ww);
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	return err;
}

void __intel_context_do_unpin(struct intel_context *ce, int sub)
{
	GEM_BUG_ON(atomic_read(&ce->pin_count) < sub);
	if (!atomic_sub_and_test(sub, &ce->pin_count))
		return;

	CE_TRACE(ce, "unpin\n");
	ce->ops->unpin(ce);
	ce->ops->post_unpin(ce);

	/*
	 * Once released, we may asynchronously drop the active reference.
	 * As that may be the only reference keeping the context alive,
	 * take an extra now so that it is not freed before we finish
	 * dereferencing it.
	 */
	intel_context_get(ce);
	intel_context_active_release(ce);
	trace_intel_context_do_unpin(ce);
	intel_context_put(ce);
}

static void __intel_context_retire(struct i915_active *active)
{
	struct intel_context *ce = container_of(active, typeof(*ce), active);

	CE_TRACE(ce, "retire runtime: { total:%lluns, avg:%lluns }\n",
		 intel_context_get_total_runtime_ns(ce),
		 intel_context_get_avg_runtime_ns(ce));

	set_bit(CONTEXT_VALID_BIT, &ce->flags);

	atomic_dec(&ce->vm->active_contexts[ce->engine->gt->info.id]);

	intel_context_post_unpin(ce);
	intel_context_put(ce);
}

static int __intel_context_active(struct i915_active *active)
{
	struct intel_context *ce = container_of(active, typeof(*ce), active);

	intel_context_get(ce);

	/* everything should already be activated by intel_context_pre_pin() */
	__i915_active_acquire(&ce->ring->vma->active);
	__intel_ring_pin(ce->ring);

	__intel_timeline_pin(ce->timeline);

	if (ce->state) {
		__i915_active_acquire(&ce->state->active);
		__i915_vma_pin(ce->state);
		i915_vma_make_unshrinkable(ce->state);
	}

	atomic_inc(&ce->vm->active_contexts[ce->engine->gt->info.id]);
	return 0;
}

static int
sw_fence_dummy_notify(struct i915_sw_fence *sf,
		      enum i915_sw_fence_notify state)
{
	return NOTIFY_DONE;
}

void intel_context_update_schedule_policy(struct intel_context *ce)
{
	atomic_t *count = &ce->schedule_policy.preempt_disable_count;
	struct intel_engine_cs *engine = ce->engine;

	if (atomic_read(count))
		return;

	ce->schedule_policy.preempt_timeout_ms =
					engine->props.preempt_timeout_ms;
	ce->schedule_policy.timeslice_duration_ms =
					engine->props.timeslice_duration_ms;
}

void intel_context_init_schedule_policy(struct intel_context *ce)
{
	atomic_set(&ce->schedule_policy.preempt_disable_count, 0);
	intel_context_update_schedule_policy(ce);
}

static void __intel_context_set_preemption_timeout(struct intel_context *ce,
						   u32 preemption_timeout_ms)
{
	/* FIXME: This needs execlist support as well */
	if (!intel_uc_wants_guc_submission(&ce->engine->gt->uc))
		return;

	ce->schedule_policy.preempt_timeout_ms = preemption_timeout_ms;

	intel_guc_context_set_preemption_timeout(ce);
}

void intel_context_reset_preemption_timeout(struct intel_context *ce)
{
	atomic_t *count = &ce->schedule_policy.preempt_disable_count;
	struct intel_engine_cs *engine = ce->engine;

	GEM_WARN_ON(atomic_read(count) <= 0);

	if (atomic_dec_and_test(count))
		__intel_context_set_preemption_timeout(ce,
					engine->props.preempt_timeout_ms);
}

void intel_context_disable_preemption_timeout(struct intel_context *ce)
{
	atomic_t *count = &ce->schedule_policy.preempt_disable_count;

	if (atomic_inc_return(count) == 1)
		__intel_context_set_preemption_timeout(ce, 0);
}

void
intel_context_init(struct intel_context *ce, struct intel_engine_cs *engine)
{
	GEM_BUG_ON(!engine->cops);
	GEM_BUG_ON(!engine->gt->vm);

	kref_init(&ce->ref);

	ce->engine = engine;
	ce->ops = engine->cops;
	ce->sseu = engine->sseu;
	ce->ring = NULL;
	ce->ring_size = SZ_4K;

	ewma_runtime_init(&ce->stats.runtime.avg);

	ce->vm = i915_vm_get(engine->gt->vm);

	/* NB ce->signal_link/lock is used under RCU */
	spin_lock_init(&ce->signal_lock);
	INIT_LIST_HEAD(&ce->signals);

	mutex_init(&ce->pin_mutex);

	spin_lock_init(&ce->guc_state.lock);
	INIT_LIST_HEAD(&ce->guc_state.fences);

	ce->guc_id.id = GUC_INVALID_CONTEXT_ID;
	INIT_LIST_HEAD(&ce->guc_id.link);

	INIT_LIST_HEAD(&ce->destroyed_link);

	INIT_LIST_HEAD(&ce->parallel.child_list);

	/*
	 * Initialize fence to be complete as this is expected to be complete
	 * unless there is a pending schedule disable outstanding.
	 */
	i915_sw_fence_init(&ce->guc_state.blocked,
			   sw_fence_dummy_notify);
	i915_sw_fence_commit(&ce->guc_state.blocked);

	i915_active_init(&ce->active,
			 __intel_context_active, __intel_context_retire, 0);
}

void intel_context_fini(struct intel_context *ce)
{
	struct intel_context *child, *next;

	if (ce->timeline)
		intel_timeline_put(ce->timeline);
	i915_vm_put(ce->vm);

	if (ce->client)
		i915_drm_client_put(ce->client);

	/* Need to put the creation ref for the children */
	if (intel_context_is_parent(ce))
		for_each_child_safe(ce, child, next)
			intel_context_put(child);

	mutex_destroy(&ce->pin_mutex);
	i915_active_fini(&ce->active);
	i915_sw_fence_fini(&ce->guc_state.blocked);
}

void i915_context_module_exit(void)
{
	kmem_cache_destroy(slab_ce);
}

int __init i915_context_module_init(void)
{
	slab_ce = KMEM_CACHE(intel_context, SLAB_HWCACHE_ALIGN);
	if (!slab_ce)
		return -ENOMEM;

	return 0;
}

void intel_context_enter_engine(struct intel_context *ce)
{
	intel_engine_pm_get(ce->engine);
	intel_timeline_enter(ce->timeline);
}

void intel_context_exit_engine(struct intel_context *ce)
{
	unsigned long flags;

	intel_timeline_exit(ce->timeline);

	flags = 0;
	if (!test_bit(CONTEXT_BARRIER_BIT, &ce->flags)) /* short keepalive */
		flags = INTEL_WAKEREF_PUT_ASYNC |
			FIELD_PREP(INTEL_WAKEREF_PUT_DELAY, 2);

	__intel_wakeref_put(&ce->engine->wakeref, flags);
}

int intel_context_prepare_remote_request(struct intel_context *ce,
					 struct i915_request *rq)
{
	struct intel_timeline *tl = ce->timeline;
	int err;

	/* Only suitable for use in remotely modifying this context */
	GEM_BUG_ON(rq->context == ce);

	if (rcu_access_pointer(rq->timeline) != tl) { /* timeline sharing! */
		/* Queue this switch after current activity by this context. */
		err = i915_active_fence_set(&tl->last_request, rq);
		if (err)
			return err;
	}

	/*
	 * Guarantee context image and the timeline remains pinned until the
	 * modifying request is retired by setting the ce activity tracker.
	 *
	 * But we only need to take one pin on the account of it. Or in other
	 * words transfer the pinned ce object to tracked active request.
	 */
	GEM_BUG_ON(i915_active_is_idle(&ce->active));
	return i915_active_add_request(&ce->active, rq);
}

struct i915_request *intel_context_create_request(struct intel_context *ce)
{
	struct i915_gem_ww_ctx ww;
	struct i915_request *rq;
	int err;

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = intel_context_pin_ww(ce, &ww);
	if (!err) {
		rq = i915_request_create(ce);
		intel_context_unpin(ce);
	} else if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
		rq = ERR_PTR(err);
	} else {
		rq = ERR_PTR(err);
	}

	i915_gem_ww_ctx_fini(&ww);

	if (IS_ERR(rq))
		return rq;

	/*
	 * timeline->mutex should be the inner lock, but is used as outer lock.
	 * Hack around this to shut up lockdep in selftests..
	 */
	lockdep_unpin_lock(&ce->timeline->mutex, rq->cookie);
#ifdef BPM_LOCKING_NESTED_ARG_NOT_PRESENT
	mutex_release(&ce->timeline->mutex.dep_map, 0, _RET_IP_);
#else
	mutex_release(&ce->timeline->mutex.dep_map, _RET_IP_);
#endif
	mutex_acquire(&ce->timeline->mutex.dep_map, SINGLE_DEPTH_NESTING, 0, _RET_IP_);
	rq->cookie = lockdep_pin_lock(&ce->timeline->mutex);

	return rq;
}

struct i915_request *
__intel_context_find_active_request(struct intel_context *ce,
				    bool rq_get_ref)
{
	struct i915_request *rq, *active = NULL;

	/*
	 * We search the parent list to find an active request on the submitted
	 * context. The parent list contains the requests for all the contexts
	 * in the relationship so we have to do a compare of each request's
	 * context.
	 */
	rcu_read_lock();
	list_for_each_entry_reverse(rq, &ce->timeline->requests, link) {
		if (__i915_request_is_complete(rq))
			break;

		if (!(rq->execution_mask & ce->engine->mask))
			continue;

		if (i915_request_is_active(rq))
			active = rq;
	}

	if (rq_get_ref && active)
		active = i915_request_get(active);

	rcu_read_unlock();

	return active;
}

void intel_context_bind_parent_child(struct intel_context *parent,
				     struct intel_context *child)
{
	/*
	 * Callers responsibility to validate that this function is used
	 * correctly but we use GEM_BUG_ON here ensure that they do.
	 */
	GEM_BUG_ON(intel_context_is_pinned(parent));
	GEM_BUG_ON(intel_context_is_child(parent));
	GEM_BUG_ON(intel_context_is_pinned(child));
	GEM_BUG_ON(intel_context_is_child(child));
	GEM_BUG_ON(intel_context_is_parent(child));

	child->parallel.child_index = parent->parallel.number_children++;
	list_add_tail(&child->parallel.child_link,
		      &parent->parallel.child_list);
	child->parallel.parent = parent;
}

u64 intel_context_get_total_runtime_ns(const struct intel_context *ce)
{
	u64 total, active;

	total = ce->stats.runtime.total;
	if (ce->ops->flags & COPS_RUNTIME_CYCLES)
		total *= ce->engine->gt->clock_period_ns;

	active = READ_ONCE(ce->stats.active);
	if (active)
		active = intel_context_clock() - active;

	return total + active;
}

u64 intel_context_get_avg_runtime_ns(struct intel_context *ce)
{
	u64 avg = ewma_runtime_read(&ce->stats.runtime.avg);

	if (ce->ops->flags & COPS_RUNTIME_CYCLES)
		avg *= ce->engine->gt->clock_period_ns;

	return avg;
}

int intel_context_throttle(const struct intel_context *ce, long timeout)
{
	const struct intel_timeline *tl = ce->timeline;
	const struct intel_ring *ring = ce->ring;
	struct i915_request *rq;
	int err = 0;

	if (READ_ONCE(ring->space) >= SZ_1K)
		return 0;

	rcu_read_lock();
	list_for_each_entry_reverse(rq, &tl->requests, link) {
		if (i915_request_signaled(rq))
			break;

		if (rq->ring != ring)
			continue;

		/* Wait until there will be enough space following that rq */
		if (__intel_ring_space(rq->postfix,
				       ring->emit,
				       ring->size) < ring->size / 2) {
			if (!__i915_request_is_complete(rq) &&
			    i915_request_get_rcu(rq)) {
				rcu_read_unlock();

				if (timeout && intel_context_is_barrier(ce))
					i915_request_set_priority(rq, I915_PRIORITY_BARRIER);

				timeout = i915_request_wait(rq,
							    I915_WAIT_INTERRUPTIBLE,
							    timeout);
				if (timeout < 0)
					err = timeout;

				rcu_read_lock();
				i915_request_put(rq);
			}
			break;
		}
	}
	rcu_read_unlock();

	return err;
}

bool intel_context_ban(struct intel_context *ce, struct i915_request *rq)
{
	bool ret = intel_context_set_banned(ce);

	trace_intel_context_ban(ce);
	if (ce->ops->ban)
		ce->ops->ban(ce, rq);

	if (!ret) {
		struct i915_gem_context *ctx;

		rcu_read_lock();
		ctx = rcu_dereference(ce->gem_context);
		if (ctx)
			i915_gem_context_set_banned(ctx);
		rcu_read_unlock();
	}

	return ret;
}

static void hexdump(struct drm_printer *m, int indent, const void *buf, size_t len)
{
	const size_t rowsize = 8 * sizeof(u32);
	const void *prev = NULL;
	bool skip = false;
	size_t pos;

	for (pos = 0; pos < len; pos += rowsize) {
		char line[128];

		if (prev && !memcmp(prev, buf + pos, rowsize)) {
			if (!skip) {
				i_printf(m, indent, "*\n");
				skip = true;
			}
			continue;
		}

		WARN_ON_ONCE(hex_dump_to_buffer(buf + pos, len - pos,
						rowsize, sizeof(u32),
						line, sizeof(line),
						false) >= sizeof(line));
		i_printf(m, indent, "[%04zx] %s\n", pos, line);

		prev = buf + pos;
		skip = false;
	}
}

void intel_context_show(struct intel_context *ce, struct drm_printer *p, int indent)
{
	bool running = ce->timeline && i915_active_fence_isset(&ce->timeline->last_request);
	u32 *regs = ce->lrc_reg_state;
	char buf[80] = "[i915]";
	int i, len;

	if (ce->client) {
		rcu_read_lock();
		sprintf(buf, READ_ONCE(ce->client->closed) ? "%s<%d>" : "%s[%d]",
			i915_drm_client_name(ce->client),
			pid_nr(i915_drm_client_pid(ce->client)));
		rcu_read_unlock();
	}

	i_printf(p, indent, "ce->name: %s\n", buf);
	if (ce->timeline)
		i_printf(p, indent, "ce->fence: %llx\n", ce->timeline->fence_context);
	i_printf(p, indent, "ce->engine: %s\n", ce->engine->name);

	len = 0;
	buf[0] = '\0';
	if (intel_context_is_banned(ce))
		len += snprintf(buf + len, sizeof(buf) - len, "banned, ");
	if (intel_context_is_closed(ce))
		len += snprintf(buf + len, sizeof(buf) - len, "closed, ");
	if (intel_context_debug(ce))
		len += snprintf(buf + len, sizeof(buf) - len, "debug, ");
	if (len)
		buf[len - 2] = '\0';
	i_printf(p, indent, "ce->flags: 0x%08lx [%s]\n", ce->flags, buf);
	i_printf(p, indent, "ce->pins: { pinned:%d, active:%d, running:%s }\n",
		 atomic_read(&ce->pin_count), ce->active_count, str_yes_no(running));
	i_printf(p, indent, "ce->runtime: { total: %lld ns, avg: %lld ns }\n",
		 intel_context_get_total_runtime_ns(ce),
		 intel_context_get_avg_runtime_ns(ce));

	i_printf(p, indent, "ce->lrc.lrca: 0x%08x\n", ce->lrc.lrca);
	i_printf(p, indent, "ce->lrc.ccid: 0x%08x\n", ce->lrc.ccid);

	i_printf(p, indent, "ce->policy.preempt_timeout_ms: %d\n", ce->schedule_policy.preempt_timeout_ms);
	i_printf(p, indent, "ce->policy.timeslice_duration_ms: %d\n", ce->schedule_policy.timeslice_duration_ms);

	i_printf(p, indent, "ce->guc.id: 0x%08x\n", ce->guc_id.id);
	i_printf(p, indent, "ce->guc.ref: 0x%08x\n", atomic_read(&ce->guc_id.ref));
	i_printf(p, indent, "ce->guc.state: 0x%08x\n", ce->guc_state.sched_state);

	if (running) {
		len = 0;
		buf[0] = '\0';
		for (i = GUC_CLIENT_PRIORITY_KMD_HIGH; i < GUC_CLIENT_PRIORITY_NUM; ++i)
			len += snprintf(buf + len, sizeof(buf) - len,
					"%d, ", ce->guc_state.prio_count[i]);
		buf[len - 2] = '\0';
		i_printf(p, indent, "ce->guc.priority: %d [%s]\n", ce->guc_state.prio, buf);
	}

	if (has_null_page(ce->vm))
		i_printf(p, indent, "vm->poison:   NULL PTE\n");
	else
		i_printf(p, indent, "vm->poison:   0x%08x\n",
			 ce->vm->poison);

	if (ce->ring && running) {
		i_printf(p, indent, "ring->start:  0x%08x [%08x]\n",
			 i915_ggtt_offset(ce->ring->vma), regs ? regs[CTX_RING_START] : -1);
		i_printf(p, indent, "ring->head:   0x%08x [%08x]\n",
			 ce->ring->head, regs ? regs[CTX_RING_HEAD] : -1);
		i_printf(p, indent, "ring->tail:   0x%08x [%08x]\n",
			 ce->ring->tail, regs ? regs[CTX_RING_TAIL] : -1);
		i_printf(p, indent, "ring->emit:   0x%08x\n",
			 ce->ring->emit);
		i_printf(p, indent, "ring->space:  0x%08x\n",
			 ce->ring->space);
		i_printf(p, indent, "ring->hwsp:   0x%08x\n",
			 ce->timeline->hwsp_offset);
	}

	if (ce->lrc_reg_state && running) {
		void *va = ce->lrc_reg_state;

		i_printf(p, indent, "ppHWSP [0x%08lx,0x%08x):\n",
			 i915_ggtt_offset(ce->state) - PAGE_SIZE,
			 i915_ggtt_offset(ce->state));
		hexdump(p, indent + 2, va - PAGE_SIZE, PAGE_SIZE);

		i_printf(p, indent, "Logical Ring Context [0x%08x,0x%08llx):\n",
			 i915_ggtt_offset(ce->state),
			 i915_ggtt_offset(ce->state) + ce->state->node.size);
		hexdump(p, indent + 2, va, PAGE_SIZE);
	}
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_context.c"
#endif
