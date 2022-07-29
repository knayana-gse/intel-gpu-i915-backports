/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2012-2014 Intel Corporation
 *
  * Based on amdgpu_mn, which bears the following notice:
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Christian König <christian.koenig@amd.com>
 */

#include <linux/mmu_context.h>
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/sched/mm.h>

#include "i915_drv.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_object.h"
#include "i915_gem_userptr.h"
#include "i915_scatterlist.h"

#ifdef CONFIG_MMU_NOTIFIER

/*
 * Utility function to sync all vmas of an object, while still on list.
 */
static int i915_sync_all_vmas(struct drm_i915_gem_object *obj)
{
	struct list_head still_in_list;
	struct i915_vma *vma, *next;
	struct i915_address_space *vm = NULL;
	int ret;
	bool faultable_vm = false;
	bool loop_done;

	INIT_LIST_HEAD(&still_in_list);

restart:
	spin_lock(&obj->vma.lock);
	list_for_each_entry_safe(vma, next, &obj->vma.list, obj_link) {
		if (!i915_vma_is_active(vma) ||
			i915_vm_page_fault_enabled(vma->vm)) {
			list_move_tail(&vma->obj_link, &still_in_list);
			if (i915_vm_page_fault_enabled(vma->vm))
				faultable_vm = true;
		} else {
			if (i915_vma_is_persistent(vma)) {
				vm = i915_vm_get(vma->vm);
				break;
			}
			if (__i915_vma_get(vma))
				break;

			list_move_tail(&vma->obj_link, &still_in_list);
		}
	}
	loop_done = (&vma->obj_link == &obj->vma.list);
	/* Don't break vma list ordering! */
	list_splice_init(&still_in_list, &obj->vma.list);
	spin_unlock(&obj->vma.lock);

	if (loop_done) {
		/*
		 * If object is mapped to any faultable vm, unbind it in the
		 * invalidate_work. GPU access of the object later will trigger
		 * page fault handler which will rebind the object.
		 * Note the userptr invalidate/revalidate process under faultable
		 * vm is different from non-faultable vm in which case we suspend
		 * context, unbind and rebind userptr in a worker. For faultable
		 * vm, we don't need to suspend context and wait for suspend to
		 * complete. Also if we do the same thing for faultable vm, there
		 * will be a deadlock: GPU page fault handler need to wait for
		 * any ongoing userptr invaliation to finish before it revalidate
		 * userptr; while the userptr invalidation depends on context
		 * suspend to finish, whereas a context with pending page fault
		 * can't be suspended.
		 */
               if (faultable_vm)
                       queue_work(system_unbound_wq, &obj->userptr.invalidate_work);

		return 0;
	}

	if (vm) {
		ret = i915_vm_sync(vm);
		i915_vm_put(vm);
		vm = NULL;
	} else {
		ret = i915_vma_sync(vma);
		__i915_vma_put(vma);
	}

	if (!ret)
		goto restart;

	if (faultable_vm)
		queue_work(system_unbound_wq, &obj->userptr.invalidate_work);

	return ret;
}

/**
 * i915_gem_userptr_invalidate - callback to notify about mm change
 *
 * @mni: the range (mm) is about to update
 * @range: details on the invalidation
 * @cur_seq: Value to pass to mmu_interval_set_seq()
 *
 * Block for operations on BOs to finish and mark pages as accessed and
 * potentially dirty.
 */
static bool i915_gem_userptr_invalidate(struct mmu_interval_notifier *mni,
					const struct mmu_notifier_range *range,
					unsigned long cur_seq)
{
	struct drm_i915_gem_object *obj = container_of(mni, struct drm_i915_gem_object, userptr.notifier);
	struct drm_i915_private *i915;
	struct i915_address_space *vm;
	struct i915_vma *vma;
	int ret;

	if (!mmu_notifier_range_blockable(range))
		return false;

	/*
	 * We don't wait when the process is exiting. This is valid
	 * because the object will be cleaned up anyway.
	 *
	 * This is also temporarily required as a hack, because we
	 * cannot currently force non-consistent batch buffers to preempt
	 * and reschedule by waiting on it, hanging processes on exit.
	 */
	if (!kref_get_unless_zero(&obj->base.refcount))
		return true;

	i915 = to_i915(obj->base.dev);
	write_lock(&i915->mm.notifier_lock);

	/*
	 * Notify execbuf that a rerun is needed. For non-persistent vmas
	 * the mmu_interval_set_seq() is sufficient, but submissions on
	 * long running contexts with pre-bound persistent non-faultable
	 * vmas rely on the vm_rebind_list being non-empty to restart,
	 * so we need to populate that list under the notifier lock.
	 * For persistent faultable vmas, object rebind is done in page
	 * fault handler and it doesn't depend on vm_rebind_list, so no
	 * need to populate vm_rebind_list for this case.
	 */
	mmu_interval_set_seq(mni, cur_seq);
	if (current->flags & PF_EXITING) {
		write_unlock(&i915->mm.notifier_lock);
		i915_gem_object_put(obj);
		return true;
	}

	spin_lock(&obj->vma.lock);
	list_for_each_entry(vma, &obj->vma.list, obj_link) {
		GEM_BUG_ON(vma->obj != obj);
		if (!i915_vma_is_persistent(vma))
			continue;

		vm = vma->vm;
		if (i915_vm_page_fault_enabled(vm))
			continue;

		spin_lock(&vm->vm_rebind_lock);
		if (!i915_vma_is_purged(vma))
			list_move_tail(&vma->vm_rebind_link,
				       &vm->vm_rebind_list);
		spin_unlock(&vm->vm_rebind_lock);
	}
	spin_unlock(&obj->vma.lock);
	write_unlock(&i915->mm.notifier_lock);

	/*
	 * mmu_interval_read_begin() should be blocking new bindings of
	 * these vmas until the full invalidation is complete.
	 */
	ret = i915_sync_all_vmas(obj);
	if (ret)
		drm_err(&i915->drm,
			"(%d) failed to wait for idle\n", ret);

	i915_gem_object_put(obj);
	return true;
}

static const struct mmu_interval_notifier_ops i915_gem_userptr_notifier_ops = {
	.invalidate = i915_gem_userptr_invalidate,
};

static int
i915_gem_userptr_init__mmu_notifier(struct drm_i915_gem_object *obj)
{
	return mmu_interval_notifier_insert(&obj->userptr.notifier, current->mm,
					    obj->userptr.ptr, obj->base.size,
					    &i915_gem_userptr_notifier_ops);
}

static void i915_gem_object_userptr_drop_ref(struct drm_i915_gem_object *obj)
{
	struct page **pvec = NULL;

	assert_object_held_shared(obj);

	if (!--obj->userptr.page_ref) {
		pvec = obj->userptr.pvec;
		obj->userptr.pvec = NULL;
	}
	GEM_BUG_ON(obj->userptr.page_ref < 0);

	if (pvec) {
		const unsigned long num_pages = obj->base.size >> PAGE_SHIFT;

		release_pages(pvec, num_pages);
		kvfree(pvec);
	}
}

static int i915_gem_userptr_get_pages(struct drm_i915_gem_object *obj)
{
	pgoff_t num_pages; /* limited by __sg_alloc_table_from_pages */
	unsigned int max_segment = i915_sg_segment_size();
	struct sg_table *st;
	unsigned int sg_page_sizes;
	struct page **pvec;
	int ret;

	if (!safe_conversion(&num_pages, obj->base.size >> PAGE_SHIFT))
		return -E2BIG;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	if (!obj->userptr.page_ref) {
		ret = -EAGAIN;
		goto err_free;
	}

	obj->userptr.page_ref++;
	pvec = obj->userptr.pvec;

alloc_table:
	ret = __sg_alloc_table_from_pages(st, pvec, num_pages, 0,
					num_pages << PAGE_SHIFT,
					max_segment,
					GFP_KERNEL);
	if (ret) {
		goto err_free;
	}

	ret = i915_gem_gtt_prepare_pages(obj, st);
	if (ret) {
		sg_free_table(st);

		if (max_segment > PAGE_SIZE) {
			max_segment = PAGE_SIZE;
			goto alloc_table;
		}

		goto err;
	}

	sg_page_sizes = i915_sg_page_sizes(st->sgl);

	__i915_gem_object_set_pages(obj, st, sg_page_sizes);

	return 0;

err:
	i915_gem_object_userptr_drop_ref(obj);
err_free:
	kfree(st);
	return ret;
}

static int
i915_gem_userptr_put_pages(struct drm_i915_gem_object *obj,
			   struct sg_table *pages)
{
	struct sgt_iter sgt_iter;
	struct page *page;

	if (!pages)
		return 0;

	__i915_gem_object_release_shmem(obj, pages, true);
	i915_gem_gtt_finish_pages(obj, pages);

	/*
	 * We always mark objects as dirty when they are used by the GPU,
	 * just in case. However, if we set the vma as being read-only we know
	 * that the object will never have been written to.
	 */
	if (i915_gem_object_is_readonly(obj))
		obj->mm.dirty = false;

	for_each_sgt_page(page, sgt_iter, pages) {
		if (obj->mm.dirty && trylock_page(page)) {
			/*
			 * As this may not be anonymous memory (e.g. shmem)
			 * but exist on a real mapping, we have to lock
			 * the page in order to dirty it -- holding
			 * the page reference is not sufficient to
			 * prevent the inode from being truncated.
			 * Play safe and take the lock.
			 *
			 * However...!
			 *
			 * The mmu-notifier can be invalidated for a
			 * migrate_page, that is alreadying holding the lock
			 * on the page. Such a try_to_unmap() will result
			 * in us calling put_pages() and so recursively try
			 * to lock the page. We avoid that deadlock with
			 * a trylock_page() and in exchange we risk missing
			 * some page dirtying.
			 */
			set_page_dirty(page);
			unlock_page(page);
		}

		mark_page_accessed(page);
	}
	obj->mm.dirty = false;

	sg_free_table(pages);
	kfree(pages);

	i915_gem_object_userptr_drop_ref(obj);

	return 0;
}

static int i915_gem_object_userptr_unbind(struct drm_i915_gem_object *obj,
					  struct i915_gem_ww_ctx *ww)
{
	struct sg_table *pages;
	int err;

	err = i915_gem_object_unbind(obj, ww, I915_GEM_OBJECT_UNBIND_ACTIVE);
	if (err)
		return err;

	if (GEM_WARN_ON(i915_gem_object_has_pinned_pages(obj)))
		return -EBUSY;

	assert_object_held(obj);

	pages = __i915_gem_object_unset_pages(obj);
	if (!IS_ERR_OR_NULL(pages))
		i915_gem_userptr_put_pages(obj, pages);

	return err;
}

static void i915_gem_object_userptr_invalidate_work(struct work_struct *work)
{
	struct drm_i915_gem_object *obj =
		container_of(work, typeof(*obj), userptr.invalidate_work);
	struct i915_gem_ww_ctx ww;
	int ret;

	for_i915_gem_ww(&ww, ret, true) {
		ret = i915_gem_object_lock(obj, &ww);
		if (ret)
			continue;

		i915_gem_object_userptr_unbind(obj, &ww);
	}
}

int i915_gem_object_userptr_submit_init(struct drm_i915_gem_object *obj)
{
	const unsigned long num_pages = obj->base.size >> PAGE_SHIFT;
	struct page **pvec;
	unsigned int gup_flags = 0;
	unsigned long notifier_seq;
	int pinned, ret;
	bool in_kthread = !current->mm;
	struct i915_gem_ww_ctx ww;

	if (!in_kthread && obj->userptr.notifier.mm != current->mm)
		return -EFAULT;

	notifier_seq = mmu_interval_read_begin(&obj->userptr.notifier);
	/*
	 * For the faultable vm, userptr invalidation is done in a worker which can be
	 * interrupted by GPU page fault (which triggers this (re)validate function).
	 * Flush the invalidate worker here to wait for invalidation to finish.
	 */
	flush_work(&obj->userptr.invalidate_work);

	for_i915_gem_ww(&ww, ret, true) {
		ret = i915_gem_object_lock_interruptible(obj, &ww);
		if (ret)
			continue;
		/*
		 * If pages are stale, make sure userptr is unbound for
		 * next attempt.
		 */
		if (notifier_seq != obj->userptr.notifier_seq ||
		    !obj->userptr.pvec)
			ret = i915_gem_object_userptr_unbind(obj, &ww);
	}

	if (ret)
		return ret;

	pvec = kvmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pvec)
		return -ENOMEM;

	if (!i915_gem_object_is_readonly(obj))
		gup_flags |= FOLL_WRITE;

	if (in_kthread)
		kthread_use_mm(obj->userptr.notifier.mm);

	pinned = ret = 0;
	while (pinned < num_pages) {
		ret = get_user_pages_fast(obj->userptr.ptr + pinned * PAGE_SIZE,
					  num_pages - pinned,
					  gup_flags,
					  &pvec[pinned]);
		if (ret < 0) {
			if (in_kthread)
				kthread_unuse_mm(obj->userptr.notifier.mm);
			goto out;
		}

		pinned += ret;
	}
	ret = 0;

	if (in_kthread)
		kthread_unuse_mm(obj->userptr.notifier.mm);

	ret = i915_gem_object_lock_interruptible(obj, NULL);
	if (ret)
		goto out;

	if (mmu_interval_read_retry(&obj->userptr.notifier,
		!obj->userptr.page_ref ? notifier_seq :
		obj->userptr.notifier_seq)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	if (!obj->userptr.page_ref++) {
		obj->userptr.pvec = pvec;
		obj->userptr.notifier_seq = notifier_seq;
		pvec = NULL;
		ret = ____i915_gem_object_get_pages(obj);
	}

	obj->userptr.page_ref--;

out_unlock:
	i915_gem_object_unlock(obj);

out:
	if (pvec) {
		release_pages(pvec, pinned);
		kvfree(pvec);
	}

	return ret;
}

int i915_gem_object_userptr_submit_done(struct drm_i915_gem_object *obj)
{
	if (mmu_interval_read_retry(&obj->userptr.notifier,
				    obj->userptr.notifier_seq)) {
		/* We collided with the mmu notifier, need to retry */

		return -EAGAIN;
	}

	return 0;
}

int i915_gem_object_userptr_validate(struct drm_i915_gem_object *obj)
{
	int err;

	err = i915_gem_object_userptr_submit_init(obj);
	if (err)
		return err;

	err = i915_gem_object_lock_interruptible(obj, NULL);
	if (!err) {
		/*
		 * Since we only check validity, not use the pages,
		 * it doesn't matter if we collide with the mmu notifier,
		 * and -EAGAIN handling is not required.
		 */
		err = i915_gem_object_pin_pages(obj);
		if (!err)
			i915_gem_object_unpin_pages(obj);

		i915_gem_object_unlock(obj);
	}

	return err;
}

static void
i915_gem_userptr_release(struct drm_i915_gem_object *obj)
{
	GEM_WARN_ON(obj->userptr.page_ref);

	if (!obj->userptr.notifier.mm)
		return;

	mmu_interval_notifier_remove(&obj->userptr.notifier);
	obj->userptr.notifier.mm = NULL;
}

static int
i915_gem_userptr_dmabuf_export(struct drm_i915_gem_object *obj)
{
	drm_dbg(obj->base.dev, "Exporting userptr no longer allowed\n");

	return -EINVAL;
}

static int
i915_gem_userptr_pwrite(struct drm_i915_gem_object *obj,
			const struct drm_i915_gem_pwrite *args)
{
	drm_dbg(obj->base.dev, "pwrite to userptr no longer allowed\n");

	return -EINVAL;
}

static int
i915_gem_userptr_pread(struct drm_i915_gem_object *obj,
		       const struct drm_i915_gem_pread *args)
{
	drm_dbg(obj->base.dev, "pread from userptr no longer allowed\n");

	return -EINVAL;
}

static const struct drm_i915_gem_object_ops i915_gem_userptr_ops = {
	.name = "i915_gem_object_userptr",
	.flags = I915_GEM_OBJECT_IS_SHRINKABLE |
		 I915_GEM_OBJECT_NO_MMAP |
		 I915_GEM_OBJECT_IS_PROXY,
	.get_pages = i915_gem_userptr_get_pages,
	.put_pages = i915_gem_userptr_put_pages,
	.dmabuf_export = i915_gem_userptr_dmabuf_export,
	.pwrite = i915_gem_userptr_pwrite,
	.pread = i915_gem_userptr_pread,
	.release = i915_gem_userptr_release,
};

#endif

/*
 * Creates a new mm object that wraps some normal memory from the process
 * context - user memory.
 *
 * We impose several restrictions upon the memory being mapped
 * into the GPU.
 * 1. It must be page aligned (both start/end addresses, i.e ptr and size).
 * 2. It must be normal system memory, not a pointer into another map of IO
 *    space (e.g. it must not be a GTT mmapping of another object).
 * 3. We only allow a bo as large as we could in theory map into the GTT,
 *    that is we limit the size to the total size of the GTT.
 * 4. The bo is marked as being snoopable. The backing pages are left
 *    accessible directly by the CPU, but reads and writes by the GPU may
 *    incur the cost of a snoop (unless you have an LLC architecture).
 *
 * Synchronisation between multiple users and the GPU is left to userspace
 * through the normal set-domain-ioctl. The kernel will enforce that the
 * GPU relinquishes the VMA before it is returned back to the system
 * i.e. upon free(), munmap() or process termination. However, the userspace
 * malloc() library may not immediately relinquish the VMA after free() and
 * instead reuse it whilst the GPU is still reading and writing to the VMA.
 * Caveat emptor.
 *
 * Also note, that the object created here is not currently a "first class"
 * object, in that several ioctls are banned. These are the CPU access
 * ioctls: mmap(), pwrite and pread. In practice, you are expected to use
 * direct access via your pointer rather than use those ioctls. Another
 * restriction is that we do not allow userptr surfaces to be pinned to the
 * hardware and so we reject any attempt to create a framebuffer out of a
 * userptr.
 *
 * If you think this is a good interface to use to pass GPU memory between
 * drivers, please use dma-buf instead. In fact, wherever possible use
 * dma-buf instead.
 */
int
i915_gem_userptr_ioctl(struct drm_device *dev,
		       void *data,
		       struct drm_file *file)
{
	static struct lock_class_key __maybe_unused lock_class;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_userptr *args = data;
	struct drm_i915_gem_object __maybe_unused *obj;
	int __maybe_unused ret;
	u32 __maybe_unused handle;

	if (!HAS_LLC(dev_priv) && !HAS_SNOOP(dev_priv)) {
		/* We cannot support coherent userptr objects on hw without
		 * LLC and broken snooping.
		 */
		return -ENODEV;
	}

	if (args->flags & ~(I915_USERPTR_READ_ONLY |
			    I915_USERPTR_UNSYNCHRONIZED))
		return -EINVAL;

	if (i915_gem_object_size_2big(args->user_size))
		return -E2BIG;

	if (!args->user_size)
		return -EINVAL;

	if (offset_in_page(args->user_ptr | args->user_size))
		return -EINVAL;

	if (!access_ok((char __user *)(unsigned long)args->user_ptr, args->user_size))
		return -EFAULT;

	if (args->flags & I915_USERPTR_UNSYNCHRONIZED)
		return -ENODEV;

	if (args->flags & I915_USERPTR_READ_ONLY) {
		/*
		 * On almost all of the older hw, we cannot tell the GPU that
		 * a page is readonly.
		 */
		if (!to_gt(dev_priv)->vm->has_read_only)
			return -ENODEV;
	}

#ifdef CONFIG_MMU_NOTIFIER
	obj = i915_gem_object_alloc();
	if (obj == NULL)
		return -ENOMEM;

	drm_gem_private_object_init(dev, &obj->base, args->user_size);
	i915_gem_object_init(obj, &i915_gem_userptr_ops, &lock_class,
			     I915_BO_ALLOC_STRUCT_PAGE);
	obj->read_domains = I915_GEM_DOMAIN_CPU;
	obj->write_domain = I915_GEM_DOMAIN_CPU;
	i915_gem_object_set_cache_coherency(obj, I915_CACHE_LLC);

	obj->userptr.ptr = args->user_ptr;
	obj->userptr.notifier_seq = ULONG_MAX;
	if (args->flags & I915_USERPTR_READ_ONLY)
		i915_gem_object_set_readonly(obj);

	/* And keep a pointer to the current->mm for resolving the user pages
	 * at binding. This means that we need to hook into the mmu_notifier
	 * in order to detect if the mmu is destroyed.
	 */
	ret = i915_gem_userptr_init__mmu_notifier(obj);
	if (ret == 0)
		ret = drm_gem_handle_create(file, &obj->base, &handle);

	/* drop reference from allocate - handle holds it now */
	i915_gem_object_put(obj);
	if (ret)
		return ret;

	INIT_WORK(&obj->userptr.invalidate_work, i915_gem_object_userptr_invalidate_work);
	args->handle = handle;
	return 0;
#else
	return -ENODEV;
#endif
}

int i915_gem_init_userptr(struct drm_i915_private *dev_priv)
{
#ifdef CONFIG_MMU_NOTIFIER
	rwlock_init(&dev_priv->mm.notifier_lock);
#endif

	return 0;
}

void i915_gem_cleanup_userptr(struct drm_i915_private *dev_priv)
{
}

#ifdef CONFIG_MMU_NOTIFIER
void i915_gem_userptr_lock_mmu_notifier(struct drm_i915_private *i915)
{
	write_lock(&i915->mm.notifier_lock);
}

void i915_gem_userptr_unlock_mmu_notifier(struct drm_i915_private *i915)
{
	write_unlock(&i915->mm.notifier_lock);
}
#endif
