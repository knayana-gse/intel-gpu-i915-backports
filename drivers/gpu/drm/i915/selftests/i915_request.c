/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <linux/prime_numbers.h>
#include <linux/pm_qos.h>
#include <linux/sort.h>

#include "gem/i915_gem_internal.h"
#include "gem/i915_gem_pm.h"
#include "gem/i915_gem_region.h"
#include "gem/selftests/mock_context.h"

#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_gt_requests.h"
#include "gt/selftest_engine_heartbeat.h"

#include "i915_random.h"
#include "i915_selftest.h"
#include "igt_flush_test.h"
#include "igt_live_test.h"
#include "igt_spinner.h"
#include "lib_sw_fence.h"

#include "mock_drm.h"

static unsigned int num_uabi_engines(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	unsigned int count;

	count = 0;
	for_each_uabi_engine(engine, i915)
		count++;

	return count;
}

static int live_nop_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	int err = -ENODEV;

	/*
	 * Submit various sized batches of empty requests, to each engine
	 * (individually), and wait for the batch to complete. We can check
	 * the overhead of submitting requests to the hardware.
	 */

	for_each_uabi_engine(engine, i915) {
		unsigned long n, prime;
		IGT_TIMEOUT(end_time);
		ktime_t times[2] = {};

		err = igt_live_test_begin(&t, i915, __func__, engine->name);
		if (err)
			return err;

		intel_engine_pm_get(engine);
		for_each_prime_number_from(prime, 1, 8192) {
			struct i915_request *request = NULL;

			times[1] = ktime_get_raw();

			for (n = 0; n < prime; n++) {
				i915_request_put(request);
				request = i915_request_create(engine->kernel_context);
				if (IS_ERR(request))
					return PTR_ERR(request);

				/*
				 * This space is left intentionally blank.
				 *
				 * We do not actually want to perform any
				 * action with this request, we just want
				 * to measure the latency in allocation
				 * and submission of our breadcrumbs -
				 * ensuring that the bare request is sufficient
				 * for the system to work (i.e. proper HEAD
				 * tracking of the rings, interrupt handling,
				 * etc). It also gives us the lowest bounds
				 * for latency.
				 */

				i915_request_get(request);
				i915_request_add(request);
			}
			i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);
			i915_request_put(request);

			times[1] = ktime_sub(ktime_get_raw(), times[1]);
			if (prime == 1)
				times[0] = times[1];

			if (__igt_timeout(end_time, NULL))
				break;
		}
		intel_engine_pm_put(engine);

		err = igt_live_test_end(&t);
		if (err)
			return err;

		pr_info("Request latencies on %s: 1 = %lluns, %lu = %lluns\n",
			engine->name,
			ktime_to_ns(times[0]),
			prime, div64_u64(ktime_to_ns(times[1]), prime));
	}

	return err;
}

static int __cancel_inactive(struct intel_engine_cs *engine)
{
	struct intel_context *ce;
	struct igt_spinner spin;
	struct i915_request *rq;
	int err = 0;

	if (igt_spinner_init(&spin, engine->gt))
		return -ENOMEM;

	ce = intel_context_create(engine);
	if (IS_ERR(ce)) {
		err = PTR_ERR(ce);
		goto out_spin;
	}

	rq = igt_spinner_create_request(&spin, ce, MI_ARB_CHECK);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_ce;
	}

	pr_debug("%s: Cancelling inactive request\n", engine->name);
	i915_request_cancel(rq, -EINTR);
	i915_request_get(rq);
	i915_request_add(rq);

	if (i915_request_wait(rq, 0, HZ / 5) < 0) {
		struct drm_printer p = drm_info_printer(engine->i915->drm.dev);

		pr_err("%s: Failed to cancel inactive request\n", engine->name);
		intel_engine_dump(engine, &p, 0);
		err = -ETIME;
		goto out_rq;
	}

	if (rq->fence.error != -EINTR) {
		pr_err("%s: fence not cancelled (%u)\n",
		       engine->name, rq->fence.error);
		err = -EINVAL;
	}

out_rq:
	i915_request_put(rq);
out_ce:
	intel_context_put(ce);
out_spin:
	igt_spinner_fini(&spin);
	if (err)
		pr_err("%s: %s error %d\n", __func__, engine->name, err);
	return err;
}

static int __cancel_active(struct intel_engine_cs *engine)
{
	struct intel_context *ce;
	struct igt_spinner spin;
	struct i915_request *rq;
	int err = 0;

	if (igt_spinner_init(&spin, engine->gt))
		return -ENOMEM;

	ce = intel_context_create(engine);
	if (IS_ERR(ce)) {
		err = PTR_ERR(ce);
		goto out_spin;
	}

	rq = igt_spinner_create_request(&spin, ce, MI_ARB_CHECK);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_ce;
	}

	pr_debug("%s: Cancelling active request\n", engine->name);
	i915_request_get(rq);
	i915_request_add(rq);
	if (!igt_wait_for_spinner(&spin, rq)) {
		struct drm_printer p = drm_info_printer(engine->i915->drm.dev);

		pr_err("Failed to start spinner on %s\n", engine->name);
		intel_engine_dump(engine, &p, 0);
		err = -ETIME;
		goto out_rq;
	}
	i915_request_cancel(rq, -EINTR);

	if (i915_request_wait(rq, 0, HZ / 5) < 0) {
		struct drm_printer p = drm_info_printer(engine->i915->drm.dev);

		pr_err("%s: Failed to cancel active request\n", engine->name);
		intel_engine_dump(engine, &p, 0);
		err = -ETIME;
		goto out_rq;
	}

	if (rq->fence.error != -EINTR) {
		pr_err("%s: fence not cancelled (%u)\n",
		       engine->name, rq->fence.error);
		err = -EINVAL;
	}

out_rq:
	i915_request_put(rq);
out_ce:
	intel_context_put(ce);
out_spin:
	igt_spinner_fini(&spin);
	if (err)
		pr_err("%s: %s error %d\n", __func__, engine->name, err);
	return err;
}

static int __cancel_completed(struct intel_engine_cs *engine)
{
	struct intel_context *ce;
	struct igt_spinner spin;
	struct i915_request *rq;
	int err = 0;

	if (igt_spinner_init(&spin, engine->gt))
		return -ENOMEM;

	ce = intel_context_create(engine);
	if (IS_ERR(ce)) {
		err = PTR_ERR(ce);
		goto out_spin;
	}

	rq = igt_spinner_create_request(&spin, ce, MI_ARB_CHECK);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto out_ce;
	}
	igt_spinner_end(&spin);
	i915_request_get(rq);
	i915_request_add(rq);

	if (i915_request_wait(rq, 0, HZ / 5) < 0) {
		err = -ETIME;
		goto out_rq;
	}

	pr_debug("%s: Cancelling completed request\n", engine->name);
	i915_request_cancel(rq, -EINTR);
	if (rq->fence.error) {
		pr_err("%s: fence not cancelled (%u)\n",
		       engine->name, rq->fence.error);
		err = -EINVAL;
	}

out_rq:
	i915_request_put(rq);
out_ce:
	intel_context_put(ce);
out_spin:
	igt_spinner_fini(&spin);
	if (err)
		pr_err("%s: %s error %d\n", __func__, engine->name, err);
	return err;
}

static int live_cancel_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;

	/*
	 * Check cancellation of requests. We expect to be able to immediately
	 * cancel active requests, even if they are currently on the GPU.
	 */

	for_each_uabi_engine(engine, i915) {
		struct igt_live_test t;
		int err, err2;

		if (!intel_engine_has_preemption(engine))
			continue;

		err = igt_live_test_begin(&t, i915, __func__, engine->name);
		if (err)
			return err;

		err = __cancel_inactive(engine);
		if (err == 0)
			err = __cancel_active(engine);
		if (err == 0)
			err = __cancel_completed(engine);

		err2 = igt_live_test_end(&t);
		if (err)
			return err;
		if (err2)
			return err2;
	}

	return 0;
}

static struct i915_vma *empty_batch(struct intel_gt *gt)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	u32 *cmd;
	int err;

	if (HAS_LMEM(gt->i915))
		obj = intel_gt_object_create_lmem(gt, PAGE_SIZE,
						  I915_BO_ALLOC_VOLATILE);
	else
		obj = i915_gem_object_create_internal(gt->i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	*cmd = MI_BATCH_BUFFER_END;

	__i915_gem_object_flush_map(obj, 0, 64);
	i915_gem_object_unpin_map(obj);

	intel_gt_chipset_flush(gt);

	vma = i915_vma_instance(obj, gt->vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER | PIN_ZONE_48);
	if (err)
		goto err;

	/* Force the wait now to avoid including it in the benchmark */
	err = i915_vma_sync(vma);
	if (err)
		goto err_pin;

	return vma;

err_pin:
	i915_vma_unpin(vma);
err:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static int emit_bb_start(struct i915_request *rq, struct i915_vma *batch)
{
	return rq->engine->emit_bb_start(rq,
					 i915_vma_offset(batch),
					 i915_vma_size(batch),
					 0);
}

static struct i915_request *
empty_request(struct intel_engine_cs *engine,
	      struct i915_vma *batch)
{
	struct i915_request *request;
	int err;

	request = i915_request_create(engine->kernel_context);
	if (IS_ERR(request))
		return request;

	err = emit_bb_start(request, batch);
	if (err)
		goto out_request;

	i915_request_get(request);
out_request:
	i915_request_add(request);
	return err ? ERR_PTR(err) : request;
}

static int live_empty_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	int err;

	/*
	 * Submit various sized batches of empty requests, to each engine
	 * (individually), and wait for the batch to complete. We can check
	 * the overhead of submitting requests to the hardware.
	 */

	for_each_uabi_engine(engine, i915) {
		IGT_TIMEOUT(end_time);
		struct i915_request *request;
		struct i915_vma *batch;
		unsigned long n, prime;
		ktime_t times[2] = {};

		batch = empty_batch(engine->gt);
		if (IS_ERR(batch))
			return PTR_ERR(batch);

		err = igt_live_test_begin(&t, i915, __func__, engine->name);
		if (err)
			goto out_batch;

		intel_engine_pm_get(engine);

		/* Warmup / preload */
		request = empty_request(engine, batch);
		if (IS_ERR(request)) {
			err = PTR_ERR(request);
			intel_engine_pm_put(engine);
			goto out_batch;
		}
		i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);

		for_each_prime_number_from(prime, 1, 8192) {
			times[1] = ktime_get_raw();

			for (n = 0; n < prime; n++) {
				i915_request_put(request);
				request = empty_request(engine, batch);
				if (IS_ERR(request)) {
					err = PTR_ERR(request);
					intel_engine_pm_put(engine);
					goto out_batch;
				}
			}
			i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);

			times[1] = ktime_sub(ktime_get_raw(), times[1]);
			if (prime == 1)
				times[0] = times[1];

			if (__igt_timeout(end_time, NULL))
				break;
		}
		i915_request_put(request);
		intel_engine_pm_put(engine);

		err = igt_live_test_end(&t);
		if (err)
			goto out_batch;

		pr_info("Batch latencies on %s: 1 = %lluns, %lu = %lluns\n",
			engine->name,
			ktime_to_ns(times[0]),
			prime, div64_u64(ktime_to_ns(times[1]), prime));

out_batch:
		i915_vma_unpin(batch);
		i915_vma_put(batch);
		if (err)
			break;
	}

	return err;
}

static struct i915_vma *recursive_batch(struct intel_gt *gt)
{
	struct drm_i915_private *i915 = gt->i915;
	struct drm_i915_gem_object *obj;
	const int ver = GRAPHICS_VER(i915);
	struct i915_vma *vma;
	u32 *cmd;
	int err;

	if (HAS_LMEM(gt->i915))
		obj = intel_gt_object_create_lmem(gt, PAGE_SIZE,
						  I915_BO_ALLOC_VOLATILE);
	else
		obj = i915_gem_object_create_internal(gt->i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	vma = i915_vma_instance(obj, gt->vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER | PIN_ZONE_48);
	if (err)
		goto err;

	cmd = i915_gem_object_pin_map_unlocked(obj, I915_MAP_WC);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	if (ver >= 8) {
		*cmd++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
		*cmd++ = lower_32_bits(i915_vma_offset(vma));
		*cmd++ = upper_32_bits(i915_vma_offset(vma));
	} else if (ver >= 6) {
		*cmd++ = MI_BATCH_BUFFER_START | 1 << 8;
		*cmd++ = lower_32_bits(i915_vma_offset(vma));
	} else {
		*cmd++ = MI_BATCH_BUFFER_START | MI_BATCH_GTT;
		*cmd++ = lower_32_bits(i915_vma_offset(vma));
	}
	*cmd++ = MI_BATCH_BUFFER_END; /* terminate early in case of error */

	__i915_gem_object_flush_map(obj, 0, 64);
	i915_gem_object_unpin_map(obj);

	intel_gt_chipset_flush(gt);

	return vma;

err:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static int recursive_batch_resolve(struct i915_vma *batch)
{
	u32 *cmd;

	cmd = i915_gem_object_pin_map_unlocked(batch->obj, I915_MAP_WC);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	*cmd = MI_BATCH_BUFFER_END;

	__i915_gem_object_flush_map(batch->obj, 0, sizeof(*cmd));
	i915_gem_object_unpin_map(batch->obj);

	intel_gt_chipset_flush(batch->vm->gt);

	return 0;
}

static int live_all_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	struct i915_request **request;
	struct igt_live_test t;
	unsigned int idx;
	int err;

	/*
	 * Check we can submit requests to all engines simultaneously. We
	 * send a recursive batch to each engine - checking that we don't
	 * block doing so, and that they don't complete too soon.
	 */

	request = kcalloc(nengines, sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	err = igt_live_test_begin(&t, i915, __func__, "");
	if (err)
		goto out_free;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct i915_vma *batch;

		batch = recursive_batch(engine->gt);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			pr_err("%s: Unable to create batch, err=%d\n",
			       __func__, err);
			goto out_free;
		}

		i915_vma_lock(batch);
		request[idx] = intel_engine_create_kernel_request(engine);
		if (IS_ERR(request[idx])) {
			err = PTR_ERR(request[idx]);
			pr_err("%s: Request allocation failed with err=%d\n",
			       __func__, err);
			goto out_unlock;
		}
		GEM_BUG_ON(request[idx]->context->vm != batch->vm);

		err = i915_request_await_object(request[idx], batch->obj, 0);
		if (err == 0)
			err = i915_vma_move_to_active(batch, request[idx], 0);
		GEM_BUG_ON(err);

		err = emit_bb_start(request[idx], batch);
		GEM_BUG_ON(err);
		request[idx]->batch = batch;

		i915_request_get(request[idx]);
		i915_request_add(request[idx]);
		idx++;
out_unlock:
		i915_vma_unlock(batch);
		if (err)
			goto out_request;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		if (i915_request_completed(request[idx])) {
			pr_err("%s(%s): request completed too early!\n",
			       __func__, engine->name);
			err = -EINVAL;
			goto out_request;
		}
		idx++;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		err = recursive_batch_resolve(request[idx]->batch);
		if (err) {
			pr_err("%s: failed to resolve batch, err=%d\n",
			       __func__, err);
			goto out_request;
		}
		idx++;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct i915_request *rq = request[idx];
		long timeout;

		timeout = i915_request_wait(rq, 0, MAX_SCHEDULE_TIMEOUT);
		if (timeout < 0) {
			err = timeout;
			pr_err("%s: error waiting for request on %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_request;
		}

		GEM_BUG_ON(!i915_request_completed(rq));
		i915_vma_unpin(rq->batch);
		i915_vma_put(rq->batch);
		i915_request_put(rq);
		request[idx] = NULL;
		idx++;
	}

	err = igt_live_test_end(&t);

out_request:
	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct i915_request *rq = request[idx];

		if (!rq)
			continue;

		if (rq->batch) {
			i915_vma_unpin(rq->batch);
			i915_vma_put(rq->batch);
		}
		i915_request_put(rq);
		idx++;
	}
out_free:
	kfree(request);
	return err;
}

static int live_sequential_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const unsigned int nengines = num_uabi_engines(i915);
	struct i915_request **request;
	struct i915_request *prev = NULL;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	unsigned int idx;
	int err;

	/*
	 * Check we can submit requests to all engines sequentially, such
	 * that each successive request waits for the earlier ones. This
	 * tests that we don't execute requests out of order, even though
	 * they are running on independent engines.
	 */

	request = kcalloc(nengines, sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	err = igt_live_test_begin(&t, i915, __func__, "");
	if (err)
		goto out_free;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct i915_vma *batch;

		batch = recursive_batch(engine->gt);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			pr_err("%s: Unable to create batch for %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_free;
		}

		i915_vma_lock(batch);
		request[idx] = intel_engine_create_kernel_request(engine);
		if (IS_ERR(request[idx])) {
			err = PTR_ERR(request[idx]);
			pr_err("%s: Request allocation failed for %s with err=%d\n",
			       __func__, engine->name, err);
			goto out_unlock;
		}
		GEM_BUG_ON(request[idx]->context->vm != batch->vm);

		if (prev) {
			err = i915_request_await_dma_fence(request[idx],
							   &prev->fence);
			if (err) {
				i915_request_add(request[idx]);
				pr_err("%s: Request await failed for %s with err=%d\n",
				       __func__, engine->name, err);
				goto out_unlock;
			}
		}

		err = i915_request_await_object(request[idx],
						batch->obj, false);
		if (err == 0)
			err = i915_vma_move_to_active(batch, request[idx], 0);
		GEM_BUG_ON(err);

		err = emit_bb_start(request[idx], batch);
		GEM_BUG_ON(err);
		request[idx]->batch = batch;

		i915_request_get(request[idx]);
		i915_request_add(request[idx]);

		prev = request[idx];
		idx++;

out_unlock:
		i915_vma_unlock(batch);
		if (err)
			goto out_request;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		long timeout;

		if (i915_request_completed(request[idx])) {
			pr_err("%s(%s): request completed too early!\n",
			       __func__, engine->name);
			err = -EINVAL;
			goto out_request;
		}

		err = recursive_batch_resolve(request[idx]->batch);
		if (err) {
			pr_err("%s: failed to resolve batch, err=%d\n",
			       __func__, err);
			goto out_request;
		}

		timeout = i915_request_wait(request[idx], 0,
					    MAX_SCHEDULE_TIMEOUT);
		if (timeout < 0) {
			err = timeout;
			pr_err("%s: error waiting for request on %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_request;
		}

		GEM_BUG_ON(!i915_request_completed(request[idx]));
		idx++;
	}

	err = igt_live_test_end(&t);

out_request:
	idx = 0;
	for_each_uabi_engine(engine, i915) {
		u32 *cmd;

		if (!request[idx])
			break;

		cmd = i915_gem_object_pin_map_unlocked(request[idx]->batch->obj,
						       I915_MAP_WC);
		if (!IS_ERR(cmd)) {
			*cmd = MI_BATCH_BUFFER_END;

			__i915_gem_object_flush_map(request[idx]->batch->obj,
						    0, sizeof(*cmd));
			i915_gem_object_unpin_map(request[idx]->batch->obj);

			intel_gt_chipset_flush(engine->gt);
		}

		i915_vma_put(request[idx]->batch);
		i915_request_put(request[idx]);
		idx++;
	}
out_free:
	kfree(request);
	return err;
}

static int __live_parallel_engine1(void *arg)
{
	struct intel_engine_cs *engine = arg;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;

	count = 0;
	intel_engine_pm_get(engine);
	do {
		struct i915_request *rq;

		rq = i915_request_create(engine->kernel_context);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (i915_request_wait(rq, 0, HZ) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));
	intel_engine_pm_put(engine);

	pr_info("%s: %lu request + sync\n", engine->name, count);
	return err;
}

static int __live_parallel_engineN(void *arg)
{
	struct intel_engine_cs *engine = arg;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;

	count = 0;
	intel_engine_pm_get(engine);
	do {
		struct i915_request *rq;

		rq = i915_request_create(engine->kernel_context);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
		count++;
	} while (!__igt_timeout(end_time, NULL));
	intel_engine_pm_put(engine);

	pr_info("%s: %lu requests\n", engine->name, count);
	return err;
}

static bool wake_all(struct drm_i915_private *i915)
{
	if (atomic_dec_and_test(&i915->selftest.counter)) {
		wake_up_var(&i915->selftest.counter);
		return true;
	}

	return false;
}

static int wait_for_all(struct drm_i915_private *i915)
{
	if (wake_all(i915))
		return 0;

	if (wait_var_event_timeout(&i915->selftest.counter,
				   !atomic_read(&i915->selftest.counter),
				   i915_selftest.timeout_jiffies))
		return 0;

	return -ETIME;
}

static int __live_parallel_spin(void *arg)
{
	struct intel_engine_cs *engine = arg;
	struct igt_spinner spin;
	struct i915_request *rq;
	int err = 0;

	/*
	 * Create a spinner running for eternity on each engine. If a second
	 * spinner is incorrectly placed on the same engine, it will not be
	 * able to start in time.
	 */

	if (igt_spinner_init(&spin, engine->gt)) {
		wake_all(engine->i915);
		return -ENOMEM;
	}

	intel_engine_pm_get(engine);
	rq = igt_spinner_create_request(&spin,
					engine->kernel_context,
					MI_NOOP); /* no preemption */
	intel_engine_pm_put(engine);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		if (err == -ENODEV)
			err = 0;
		wake_all(engine->i915);
		goto out_spin;
	}

	i915_request_get(rq);
	i915_request_add(rq);
	if (igt_wait_for_spinner(&spin, rq)) {
		/* Occupy this engine for the whole test */
		err = wait_for_all(engine->i915);
	} else {
		pr_err("Failed to start spinner on %s\n", engine->name);
		err = -EINVAL;
	}
	igt_spinner_end(&spin);

	if (err == 0 && i915_request_wait(rq, 0, HZ) < 0)
		err = -EIO;
	i915_request_put(rq);

out_spin:
	igt_spinner_fini(&spin);
	return err;
}

static int live_parallel_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		__live_parallel_engine1,
		__live_parallel_engineN,
		__live_parallel_spin,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct task_struct **tsk;
	int err = 0;

	/*
	 * Check we can submit requests to all engines concurrently. This
	 * tests that we load up the system maximally.
	 */

	tsk = kcalloc(nengines, sizeof(*tsk), GFP_KERNEL);
	if (!tsk)
		return -ENOMEM;

	for (fn = func; !err && *fn; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;
		unsigned int idx;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		atomic_set(&i915->selftest.counter, 0);

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			atomic_inc(&i915->selftest.counter);
			tsk[idx] = kthread_run(*fn, engine,
					       "igt/parallel:%s",
					       engine->name);
			if (IS_ERR(tsk[idx])) {
				err = PTR_ERR(tsk[idx]);
				break;
			}
			get_task_struct(tsk[idx++]);
		}

		yield(); /* start all threads before we kthread_stop() */

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			int status;

			if (IS_ERR(tsk[idx]))
				break;

			status = kthread_stop(tsk[idx]);
			if (status && !err)
				err = status;

			put_task_struct(tsk[idx++]);
		}

		if (igt_live_test_end(&t))
			err = -EIO;
	}

	kfree(tsk);
	return err;
}

int i915_request_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(live_nop_request),
		SUBTEST(live_all_engines),
		SUBTEST(live_sequential_engines),
		SUBTEST(live_parallel_engines),
		SUBTEST(live_empty_request),
		SUBTEST(live_cancel_request),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	if (!num_uabi_engines(i915))
		return 0;

	return i915_live_subtests(tests, i915);
}

static int switch_to_kernel_sync(struct intel_context *ce, int err)
{
	struct intel_engine_cs *engine = ce->engine;
	struct i915_request *rq;
	struct dma_fence *fence;

	rq = intel_engine_create_kernel_request(engine);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	fence = i915_active_fence_get(&ce->timeline->last_request);
	if (fence) {
		i915_request_await_dma_fence(rq, fence);
		dma_fence_put(fence);
	}

	rq = i915_request_get(rq);
	i915_request_add(rq);
	if (i915_request_wait(rq, 0, HZ / 2) < 0 && !err)
		err = -ETIME;
	i915_request_put(rq);

	intel_gt_retire_requests(engine->gt);
	return err;
}

struct perf_stats {
	struct intel_engine_cs *engine;
	unsigned long count;
	ktime_t time;
	ktime_t busy;
	u64 runtime;
};

struct perf_series {
	struct drm_i915_private *i915;
	unsigned int nengines;
	struct intel_context *ce[];
};

static int cmp_u32(const void *A, const void *B)
{
	const u32 *a = A, *b = B;

	return *a - *b;
}

static u32 trifilter(u32 *a)
{
	u64 sum;

#define TF_COUNT 5
	sort(a, TF_COUNT, sizeof(*a), cmp_u32, NULL);

	sum = mul_u32_u32(a[2], 2);
	sum += a[1];
	sum += a[3];

	GEM_BUG_ON(sum > U32_MAX);
	return sum;
#define TF_BIAS 2
}

static u64 cycles_to_ns(struct intel_engine_cs *engine, u32 cycles)
{
	u64 ns = intel_gt_clock_interval_to_ns(engine->gt, cycles);

	return DIV_ROUND_CLOSEST(ns, 1 << TF_BIAS);
}

static u32 *emit_timestamp_store(u32 *cs, struct intel_context *ce, u32 offset)
{
	*cs++ = MI_STORE_REGISTER_MEM_GEN8 | MI_USE_GGTT;
	*cs++ = i915_mmio_reg_offset(RING_TIMESTAMP((ce->engine->mmio_base)));
	*cs++ = offset;
	*cs++ = 0;

	return cs;
}

static u32 *emit_store_dw(u32 *cs, u32 offset, u32 value)
{
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = offset;
	*cs++ = 0;
	*cs++ = value;

	return cs;
}

static u32 *emit_semaphore_poll(u32 *cs, u32 mode, u32 value, u32 offset)
{
	*cs++ = MI_SEMAPHORE_WAIT |
		MI_SEMAPHORE_GLOBAL_GTT |
		MI_SEMAPHORE_POLL |
		mode;
	*cs++ = value;
	*cs++ = offset;
	*cs++ = 0;

	return cs;
}

static u32 *emit_semaphore_poll_until(u32 *cs, u32 offset, u32 value)
{
	return emit_semaphore_poll(cs, MI_SEMAPHORE_SAD_EQ_SDD, value, offset);
}

static void semaphore_set(u32 *sema, u32 value)
{
	WRITE_ONCE(*sema, value);
	wmb(); /* flush the update to the cache, and beyond */
}

static u32 *hwsp_scratch(const struct intel_context *ce)
{
	return memset32(ce->engine->status_page.addr + 1000, 0, 21);
}

static u32 hwsp_offset(const struct intel_context *ce, u32 *dw)
{
	return (i915_ggtt_offset(ce->engine->status_page.vma) +
		offset_in_page(dw));
}

static int wait_for_idle(struct intel_engine_cs *engine)
{
	intel_gt_retire_requests(engine->gt);
	return wait_var_event_timeout(&engine->wakeref.wakeref,
				      !intel_wakeref_is_active(&engine->wakeref),
				      HZ / 2);
}

static int measure_semaphore_response(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT], cycles;
	struct i915_request *rq;
	u32 *cs;
	int err;
	int i;

	/*
	 * Measure how many cycles it takes for the HW to detect the change
	 * in a semaphore value.
	 *
	 *    A: read CS_TIMESTAMP from CPU
	 *    poke semaphore
	 *    B: read CS_TIMESTAMP on GPU
	 *
	 * Semaphore latency: B - A
	 */

	semaphore_set(sema, -1);

	rq = i915_request_create(ce);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, 4 + 12 * ARRAY_SIZE(elapsed));
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		err = PTR_ERR(cs);
		goto err;
	}

	cs = emit_store_dw(cs, offset, 0);
	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		cs = emit_semaphore_poll_until(cs, offset, i);
		cs = emit_timestamp_store(cs, ce, offset + i * sizeof(u32));
		cs = emit_store_dw(cs, offset, 0);
	}

	intel_ring_advance(rq, cs);
	i915_request_add(rq);

	if (wait_for(READ_ONCE(*sema) == 0, 50)) {
		err = -EIO;
		goto err;
	}

	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		preempt_disable();
		cycles = ENGINE_READ_FW(ce->engine, RING_TIMESTAMP);
		semaphore_set(sema, i);
		preempt_enable();

		if (wait_for(READ_ONCE(*sema) == 0, 50)) {
			err = -EIO;
			goto err;
		}

		elapsed[i - 1] = sema[i] - cycles;
	}

	cycles = trifilter(elapsed);
	pr_info("%s: semaphore response %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return 0;

err:
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static int measure_idle_dispatch(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT], cycles;
	u32 *cs;
	int err;
	int i;

	/*
	 * Measure how long it takes for us to submit a request while the
	 * engine is idle, but is resting in our context.
	 *
	 *    A: read CS_TIMESTAMP from CPU
	 *    submit request
	 *    B: read CS_TIMESTAMP on GPU
	 *
	 * Submission latency: B - A
	 */

	for (i = 0; i < ARRAY_SIZE(elapsed); i++) {
		struct i915_request *rq;

		err = wait_for_idle(ce->engine);
		if (err)
			return err;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 4);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_timestamp_store(cs, ce, offset + i * sizeof(u32));

		intel_ring_advance(rq, cs);

		preempt_disable();
		local_bh_disable();
		elapsed[i] = ENGINE_READ_FW(ce->engine, RING_TIMESTAMP);
		i915_request_add(rq);
		local_bh_enable();
		preempt_enable();
	}

	for (i = 0; i < ARRAY_SIZE(elapsed); i++)
		elapsed[i] = sema[i] - elapsed[i];

	cycles = trifilter(elapsed);
	pr_info("%s: idle dispatch latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static int measure_busy_dispatch(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT + 1], cycles;
	u32 *cs;
	int err;
	int i;

	/*
	 * Measure how long it takes for us to submit a request while the
	 * engine is busy, polling on a semaphore in our context. With
	 * direct submission, this will include the cost of a lite restore.
	 *
	 *    A: read CS_TIMESTAMP from CPU
	 *    submit request
	 *    B: read CS_TIMESTAMP on GPU
	 *
	 * Submission latency: B - A
	 */

	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 12);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_store_dw(cs, offset + i * sizeof(u32), -1);
		cs = emit_semaphore_poll_until(cs, offset, i);
		cs = emit_timestamp_store(cs, ce, offset + i * sizeof(u32));

		intel_ring_advance(rq, cs);

		if (i > 1 && wait_for(READ_ONCE(sema[i - 1]), 500)) {
			err = -EIO;
			goto err;
		}

		preempt_disable();
		local_bh_disable();
		elapsed[i - 1] = ENGINE_READ_FW(ce->engine, RING_TIMESTAMP);
		i915_request_add(rq);
		local_bh_enable();
		semaphore_set(sema, i - 1);
		preempt_enable();
	}

	wait_for(READ_ONCE(sema[i - 1]), 500);
	semaphore_set(sema, i - 1);

	for (i = 1; i <= TF_COUNT; i++) {
		GEM_BUG_ON(sema[i] == -1);
		elapsed[i - 1] = sema[i] - elapsed[i];
	}

	cycles = trifilter(elapsed);
	pr_info("%s: busy dispatch latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static int plug(struct intel_engine_cs *engine, u32 *sema, u32 mode, int value)
{
	const u32 offset =
		i915_ggtt_offset(engine->status_page.vma) +
		offset_in_page(sema);
	struct i915_request *rq;
	u32 *cs;

	rq = i915_request_create(engine->kernel_context);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		return PTR_ERR(cs);
	}

	cs = emit_semaphore_poll(cs, mode, value, offset);

	intel_ring_advance(rq, cs);
	i915_request_add(rq);

	return 0;
}

static int measure_inter_request(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT + 1], cycles;
	struct i915_sw_fence *submit;
	int i, err;

	/*
	 * Measure how long it takes to advance from one request into the
	 * next. Between each request we flush the GPU caches to memory,
	 * update the breadcrumbs, and then invalidate those caches.
	 * We queue up all the requests to be submitted in one batch so
	 * it should be one set of contiguous measurements.
	 *
	 *    A: read CS_TIMESTAMP on GPU
	 *    advance request
	 *    B: read CS_TIMESTAMP on GPU
	 *
	 * Request latency: B - A
	 */

	err = plug(ce->engine, sema, MI_SEMAPHORE_SAD_NEQ_SDD, 0);
	if (err)
		return err;

	submit = heap_fence_create(GFP_KERNEL);
	if (!submit) {
		semaphore_set(sema, 1);
		return -ENOMEM;
	}

	intel_engine_flush_submission(ce->engine);
	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		struct i915_request *rq;
		u32 *cs;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		err = i915_sw_fence_await_sw_fence_gfp(&rq->submit,
						       submit,
						       GFP_KERNEL);
		if (err < 0) {
			i915_request_add(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 4);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_timestamp_store(cs, ce, offset + i * sizeof(u32));

		intel_ring_advance(rq, cs);
		i915_request_add(rq);
	}
	i915_sw_fence_commit(submit);
	intel_engine_flush_submission(ce->engine);
	heap_fence_put(submit);

	semaphore_set(sema, 1);

	for (i = 1; i <= TF_COUNT; i++)
		elapsed[i - 1] = sema[i + 1] - sema[i];

	cycles = trifilter(elapsed);
	pr_info("%s: inter-request latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	i915_sw_fence_commit(submit);
	heap_fence_put(submit);
	semaphore_set(sema, 1);
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static int measure_context_switch(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	struct i915_request *fence = NULL;
	u32 elapsed[TF_COUNT + 1], cycles;
	int i, j, err;
	u32 *cs;

	/*
	 * Measure how long it takes to advance from one request in one
	 * context to a request in another context. This allows us to
	 * measure how long the context save/restore take, along with all
	 * the inter-context setup we require.
	 *
	 *    A: read CS_TIMESTAMP on GPU
	 *    switch context
	 *    B: read CS_TIMESTAMP on GPU
	 *
	 * Context switch latency: B - A
	 */

	err = plug(ce->engine, sema, MI_SEMAPHORE_SAD_NEQ_SDD, 0);
	if (err)
		return err;

	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		struct intel_context *arr[] = {
			ce, ce->engine->kernel_context
		};
		u32 addr = offset + ARRAY_SIZE(arr) * i * sizeof(u32);

		for (j = 0; j < ARRAY_SIZE(arr); j++) {
			struct i915_request *rq;

			rq = i915_request_create(arr[j]);
			if (IS_ERR(rq)) {
				err = PTR_ERR(rq);
				goto err;
			}

			if (fence) {
				err = i915_request_await_dma_fence(rq,
								   &fence->fence);
				if (err) {
					i915_request_add(rq);
					goto err;
				}
			}

			cs = intel_ring_begin(rq, 4);
			if (IS_ERR(cs)) {
				i915_request_add(rq);
				err = PTR_ERR(cs);
				goto err;
			}

			cs = emit_timestamp_store(cs, ce, addr);
			addr += sizeof(u32);

			intel_ring_advance(rq, cs);

			i915_request_put(fence);
			fence = i915_request_get(rq);

			i915_request_add(rq);
		}
	}
	i915_request_put(fence);
	intel_engine_flush_submission(ce->engine);

	semaphore_set(sema, 1);

	for (i = 1; i <= TF_COUNT; i++)
		elapsed[i - 1] = sema[2 * i + 2] - sema[2 * i + 1];

	cycles = trifilter(elapsed);
	pr_info("%s: context switch latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	i915_request_put(fence);
	semaphore_set(sema, 1);
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static int measure_preemption(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT], cycles;
	u32 *cs;
	int err;
	int i;

	/*
	 * We measure two latencies while triggering preemption. The first
	 * latency is how long it takes for us to submit a preempting request.
	 * The second latency is how it takes for us to return from the
	 * preemption back to the original context.
	 *
	 *    A: read CS_TIMESTAMP from CPU
	 *    submit preemption
	 *    B: read CS_TIMESTAMP on GPU (in preempting context)
	 *    context switch
	 *    C: read CS_TIMESTAMP on GPU (in original context)
	 *
	 * Preemption dispatch latency: B - A
	 * Preemption switch latency: C - B
	 */

	if (!intel_engine_has_preemption(ce->engine))
		return 0;

	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		u32 addr = offset + 2 * i * sizeof(u32);
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 12);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_store_dw(cs, addr, -1);
		cs = emit_semaphore_poll_until(cs, offset, i);
		cs = emit_timestamp_store(cs, ce, addr + sizeof(u32));

		intel_ring_advance(rq, cs);
		i915_request_add(rq);

		if (wait_for(READ_ONCE(sema[2 * i]) == -1, 500)) {
			err = -EIO;
			goto err;
		}

		rq = i915_request_create(ce->engine->kernel_context);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 8);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_timestamp_store(cs, ce, addr);
		cs = emit_store_dw(cs, offset, i);

		intel_ring_advance(rq, cs);
		rq->sched.attr.priority = I915_PRIORITY_BARRIER;

		elapsed[i - 1] = ENGINE_READ_FW(ce->engine, RING_TIMESTAMP);
		i915_request_add(rq);
	}

	if (wait_for(READ_ONCE(sema[2 * i - 2]) != -1, 500)) {
		err = -EIO;
		goto err;
	}

	for (i = 1; i <= TF_COUNT; i++)
		elapsed[i - 1] = sema[2 * i + 0] - elapsed[i - 1];

	cycles = trifilter(elapsed);
	pr_info("%s: preemption dispatch latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	for (i = 1; i <= TF_COUNT; i++)
		elapsed[i - 1] = sema[2 * i + 1] - sema[2 * i + 0];

	cycles = trifilter(elapsed);
	pr_info("%s: preemption switch latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

struct signal_cb {
	struct dma_fence_cb base;
	bool seen;
};

static void signal_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct signal_cb *s = container_of(cb, typeof(*s), base);

	smp_store_mb(s->seen, true); /* be safe, be strong */
}

static int measure_completion(struct intel_context *ce)
{
	u32 *sema = hwsp_scratch(ce);
	const u32 offset = hwsp_offset(ce, sema);
	u32 elapsed[TF_COUNT], cycles;
	u32 *cs;
	int err;
	int i;

	/*
	 * Measure how long it takes for the signal (interrupt) to be
	 * sent from the GPU to be processed by the CPU.
	 *
	 *    A: read CS_TIMESTAMP on GPU
	 *    signal
	 *    B: read CS_TIMESTAMP from CPU
	 *
	 * Completion latency: B - A
	 */

	for (i = 1; i <= ARRAY_SIZE(elapsed); i++) {
		struct signal_cb cb = { .seen = false };
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err;
		}

		cs = intel_ring_begin(rq, 12);
		if (IS_ERR(cs)) {
			i915_request_add(rq);
			err = PTR_ERR(cs);
			goto err;
		}

		cs = emit_store_dw(cs, offset + i * sizeof(u32), -1);
		cs = emit_semaphore_poll_until(cs, offset, i);
		cs = emit_timestamp_store(cs, ce, offset + i * sizeof(u32));

		intel_ring_advance(rq, cs);

		dma_fence_add_callback(&rq->fence, &cb.base, signal_cb);
		i915_request_add(rq);

		intel_engine_flush_submission(ce->engine);
		if (wait_for(READ_ONCE(sema[i]) == -1, 50)) {
			err = -EIO;
			goto err;
		}

		preempt_disable();
		semaphore_set(sema, i);
		while (!READ_ONCE(cb.seen))
			cpu_relax();

		elapsed[i - 1] = ENGINE_READ_FW(ce->engine, RING_TIMESTAMP);
		preempt_enable();
	}

	for (i = 0; i < ARRAY_SIZE(elapsed); i++) {
		GEM_BUG_ON(sema[i + 1] == -1);
		elapsed[i] = elapsed[i] - sema[i + 1];
	}

	cycles = trifilter(elapsed);
	pr_info("%s: completion latency %d cycles, %lluns\n",
		ce->engine->name, cycles >> TF_BIAS,
		cycles_to_ns(ce->engine, cycles));

	return wait_for_idle(ce->engine);

err:
	intel_gt_set_wedged(ce->engine->gt);
	return err;
}

static void rps_pin(struct intel_gt *gt)
{
	/* Pin the frequency to max */
	atomic_inc(&gt->rps.num_waiters);
	intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);

	mutex_lock(&gt->rps.lock);
	intel_rps_set(&gt->rps, gt->rps.max_freq);
	mutex_unlock(&gt->rps.lock);
}

static void rps_unpin(struct intel_gt *gt)
{
	intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
	atomic_dec(&gt->rps.num_waiters);
}

static int perf_request_latency(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct pm_qos_request qos;
	int err = 0;

	if (GRAPHICS_VER(i915) < 8) /* per-engine CS timestamp, semaphores */
		return 0;

	cpu_latency_qos_add_request(&qos, 0); /* disable cstates */

	for_each_uabi_engine(engine, i915) {
		struct intel_context *ce;
		intel_wakeref_t wf;

		if (intel_gt_wait_for_idle(engine->gt, HZ))
			break;

		ce = intel_context_create(engine);
		if (IS_ERR(ce)) {
			err = PTR_ERR(ce);
			goto out;
		}

		err = intel_context_pin(ce);
		if (err) {
			intel_context_put(ce);
			goto out;
		}

		wf = intel_gt_pm_get(engine->gt);
		st_engine_heartbeat_disable(engine);
		rps_pin(engine->gt);

		if (err == 0)
			err = measure_semaphore_response(ce);
		if (err == 0)
			err = measure_idle_dispatch(ce);
		if (err == 0)
			err = measure_busy_dispatch(ce);
		if (err == 0)
			err = measure_inter_request(ce);
		if (err == 0)
			err = measure_context_switch(ce);
		if (err == 0)
			err = measure_preemption(ce);
		if (err == 0)
			err = measure_completion(ce);

		rps_unpin(engine->gt);
		st_engine_heartbeat_enable(engine);
		intel_gt_pm_put(engine->gt, wf);

		intel_context_unpin(ce);
		intel_context_put(ce);
		if (err)
			break;
	}

out:
	if (igt_flush_test(i915))
		err = -EIO;

	cpu_latency_qos_remove_request(&qos);
	return err;
}

static int s_sync0(void *arg)
{
	struct perf_series *ps = arg;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;
	int err = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		if (i915_request_wait(rq, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));

	return err;
}

static int s_sync1(void *arg)
{
	struct perf_series *ps = arg;
	struct i915_request *prev = NULL;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;
	int err = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		if (prev && i915_request_wait(prev, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(prev);
		prev = rq;
		if (err)
			break;

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));
	i915_request_put(prev);

	return err;
}

static int s_many(void *arg)
{
	struct perf_series *ps = arg;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq))
			return PTR_ERR(rq);

		i915_request_add(rq);

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));

	return 0;
}

static int perf_series_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		s_sync0,
		s_sync1,
		s_many,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct pm_qos_request qos;
	struct perf_stats *stats;
	struct perf_series *ps;
	unsigned int idx;
	int err = 0;

	stats = kcalloc(nengines, sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	ps = kzalloc(struct_size(ps, ce, nengines), GFP_KERNEL);
	if (!ps) {
		kfree(stats);
		return -ENOMEM;
	}

	cpu_latency_qos_add_request(&qos, 0); /* disable cstates */

	ps->i915 = i915;
	ps->nengines = nengines;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct intel_context *ce;

		ce = intel_context_create(engine);
		if (IS_ERR(ce)) {
			err = PTR_ERR(ce);
			goto out;
		}

		err = intel_context_pin(ce);
		if (err) {
			intel_context_put(ce);
			goto out;
		}

		intel_engine_pm_get(ce->engine);
		ps->ce[idx++] = ce;
	}
	GEM_BUG_ON(idx != ps->nengines);

	for (fn = func; *fn && !err; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		for (idx = 0; idx < nengines; idx++) {
			struct perf_stats *p =
				memset(&stats[idx], 0, sizeof(stats[idx]));
			struct intel_context *ce = ps->ce[idx];

			p->engine = ps->ce[idx]->engine;
			st_engine_heartbeat_disable(p->engine);

			if (intel_engine_supports_stats(p->engine))
				p->busy = intel_engine_get_busy_time(p->engine,
								     &p->time) + 1;
			else
				p->time = ktime_get();
			p->runtime = -intel_context_get_total_runtime_ns(ce);
		}

		err = (*fn)(ps);
		if (igt_live_test_end(&t))
			err = -EIO;

		for (idx = 0; idx < nengines; idx++) {
			struct perf_stats *p = &stats[idx];
			struct intel_context *ce = ps->ce[idx];
			int integer, decimal;
			u64 busy, dt, now;

			if (p->busy)
				p->busy = ktime_sub(intel_engine_get_busy_time(p->engine,
									       &now),
						    p->busy - 1);
			else
				now = ktime_get();
			p->time = ktime_sub(now, p->time);

			st_engine_heartbeat_enable(p->engine);
			err = switch_to_kernel_sync(ce, err);
			p->runtime += intel_context_get_total_runtime_ns(ce);

			busy = 100 * ktime_to_ns(p->busy);
			dt = ktime_to_ns(p->time);
			if (dt) {
				integer = div64_u64(busy, dt);
				busy -= integer * dt;
				decimal = div64_u64(100 * busy, dt);
			} else {
				integer = 0;
				decimal = 0;
			}

			pr_info("%s %5s: { seqno:%d, busy:%d.%02d%%, runtime:%lldms, walltime:%lldms }\n",
				name, p->engine->name, ce->timeline->seqno,
				integer, decimal,
				div_u64(p->runtime, 1000 * 1000),
				div_u64(ktime_to_ns(p->time), 1000 * 1000));
		}
	}

out:
	for (idx = 0; idx < nengines; idx++) {
		if (IS_ERR_OR_NULL(ps->ce[idx]))
			break;

		intel_engine_pm_put(ps->ce[idx]->engine);

		intel_context_unpin(ps->ce[idx]);
		intel_context_put(ps->ce[idx]);
	}
	kfree(ps);

	cpu_latency_qos_remove_request(&qos);
	kfree(stats);
	return err;
}

static int p_sync0(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	bool busy;
	int err = 0;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	st_engine_heartbeat_disable(engine);
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine, &p->time);
		busy = true;
	} else {
		p->time = ktime_get();
		busy = false;
	}

	count = 0;
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (i915_request_wait(rq, 0, HZ) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));

	if (busy) {
		ktime_t now;

		p->busy = ktime_sub(intel_engine_get_busy_time(engine, &now),
				    p->busy);
		p->time = ktime_sub(now, p->time);
	} else {
		p->time = ktime_sub(ktime_get(), p->time);
	}

	st_engine_heartbeat_enable(engine);
	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int p_sync1(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct i915_request *prev = NULL;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	bool busy;
	int err = 0;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	st_engine_heartbeat_disable(engine);
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine, &p->time);
		busy = true;
	} else {
		p->time = ktime_get();
		busy = false;
	}

	count = 0;
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (prev && i915_request_wait(prev, 0, HZ) < 0)
			err = -ETIME;
		i915_request_put(prev);
		prev = rq;
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));
	i915_request_put(prev);

	if (busy) {
		ktime_t now;

		p->busy = ktime_sub(intel_engine_get_busy_time(engine, &now),
				    p->busy);
		p->time = ktime_sub(now, p->time);
	} else {
		p->time = ktime_sub(ktime_get(), p->time);
	}

	st_engine_heartbeat_enable(engine);
	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int p_many(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;
	bool busy;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	st_engine_heartbeat_disable(engine);
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine, &p->time);
		busy = true;
	} else {
		p->time = ktime_get();
		busy = false;
	}

	count = 0;
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
		count++;
	} while (!__igt_timeout(end_time, NULL));

	if (busy) {
		ktime_t now;

		p->busy = ktime_sub(intel_engine_get_busy_time(engine, &now),
				    p->busy);
		p->time = ktime_sub(now, p->time);
	} else {
		p->time = ktime_sub(ktime_get(), p->time);
	}

	st_engine_heartbeat_enable(engine);
	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int perf_parallel_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		p_sync0,
		p_sync1,
		p_many,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct pm_qos_request qos;
	struct {
		struct perf_stats p;
		struct task_struct *tsk;
	} *engines;
	int err = 0;

	engines = kcalloc(nengines, sizeof(*engines), GFP_KERNEL);
	if (!engines)
		return -ENOMEM;

	cpu_latency_qos_add_request(&qos, 0);

	for (fn = func; *fn; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;
		unsigned int idx;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		atomic_set(&i915->selftest.counter, nengines);

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			memset(&engines[idx].p, 0, sizeof(engines[idx].p));
			engines[idx].p.engine = engine;
			intel_engine_pm_get(engine);

			engines[idx].tsk = kthread_run(*fn, &engines[idx].p,
						       "igt:%s", engine->name);
			if (IS_ERR(engines[idx].tsk)) {
				err = PTR_ERR(engines[idx].tsk);
				intel_engine_pm_put(engine);
				break;
			}
			get_task_struct(engines[idx++].tsk);
		}

		yield(); /* start all threads before we kthread_stop() */

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			int status;

			if (IS_ERR(engines[idx].tsk))
				break;

			status = kthread_stop(engines[idx].tsk);
			if (status && !err)
				err = status;

			put_task_struct(engines[idx++].tsk);
			intel_engine_pm_put(engine);
		}

		if (igt_live_test_end(&t))
			err = -EIO;
		if (err)
			break;

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			struct perf_stats *p = &engines[idx].p;
			u64 busy = 100 * ktime_to_ns(p->busy);
			u64 dt = ktime_to_ns(p->time);
			int integer, decimal;

			if (dt) {
				integer = div64_u64(busy, dt);
				busy -= integer * dt;
				decimal = div64_u64(100 * busy, dt);
			} else {
				integer = 0;
				decimal = 0;
			}

			GEM_BUG_ON(engine != p->engine);
			pr_info("%s %5s: { count:%lu, busy:%d.%02d%%, runtime:%lldms, walltime:%lldms }\n",
				name, engine->name, p->count, integer, decimal,
				div_u64(p->runtime, 1000 * 1000),
				div_u64(ktime_to_ns(p->time), 1000 * 1000));
			idx++;
		}
	}

	cpu_latency_qos_remove_request(&qos);
	kfree(engines);
	return err;
}

int i915_request_perf_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(perf_request_latency),
		SUBTEST(perf_series_engines),
		SUBTEST(perf_parallel_engines),
	};

	if (intel_gt_is_wedged(to_gt(i915)))
		return 0;

	return i915_subtests(tests, i915);
}
