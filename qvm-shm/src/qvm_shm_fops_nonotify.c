// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * File operations for no-notify SHM files.
 */

#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "qvm_shm_fops_common.h"
#include "qvm_shm_fops_nonotify.h"
#include "qvm_shm_hw.h"
#include "qvm_shm_types.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

struct ves_qvm_shm_region_list {
	struct list_head list;
	struct mutex mutex;
};

static struct ves_qvm_shm_region_list ves_qvm_shm_regions = {
	.list  = LIST_HEAD_INIT(ves_qvm_shm_regions.list),
	.mutex = __MUTEX_INITIALIZER(ves_qvm_shm_regions.mutex),
};

static irqreturn_t ves_qvm_shm_no_notify_irq_handler(int irq, void *region_id)
{
	struct ves_qvm_shm_region *const region = region_id;
	bool has_event;
	u16 active_now;
	u16 active_prev;
	u16 notify;
	u32 status;

	/*
	 * The handler is registered before ves_qvm_shm_request() attaches the region, so it can
	 * run while the control page is not mapped yet.
	 * The handler is deregistered after ves_qvm_shm_detach() detaches from the region,
	 * so it can run while the control page cannot be accessed anymore.
	 * In both cases the IRQ cannot be handled, because status register cannot be accessed.
	 */
	if (!smp_load_acquire(&region->ctrl_page)) {
		pr_debug_ratelimited("%s - shm_no_notify_irq_handler - IRQ_NONE irq=%d status=NA\n",
				     region->name, irq);
		return IRQ_NONE;
	}

	/*
	 * NO_NOTIFY files do not surface IRQ events to userspace. We only
	 * read status to acknowledge the backend condition and detect which
	 * handler is responsible for the interrupt.
	 */
	status = ves_qvm_shm_read_status_register(region);
	/* lower 16 bits: pending notifications to us; upper 16 bits: active
	 * clients */
	notify	   = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	active_now = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);

	active_prev			 = (u16)region->notifications.active_ids;
	region->notifications.active_ids = active_now;

	has_event = notify || (active_now != active_prev);

	if (!has_event) {
		pr_debug_ratelimited(
			"%s - shm_no_notify_irq_handler - IRQ_NONE irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
			region->name, irq, status, notify, active_now, active_prev);
		return IRQ_NONE;
	}

	pr_debug_ratelimited(
		"%s - shm_no_notify_irq_handler - IRQ_HANDLED irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
		region->name, irq, status, notify, active_now, active_prev);
	return IRQ_HANDLED;
}

static struct ves_qvm_shm_region *ves_qvm_shm_no_notify_region_find(const char *name)
{
	struct ves_qvm_shm_region *region;

	lockdep_assert_held(&ves_qvm_shm_regions.mutex);

	list_for_each_entry(region, &ves_qvm_shm_regions.list, list_entry) {
		if (!strncmp(region->name, name, VES_QVM_SHM_NAME_MAX))
			return region;
	}

	return NULL;
}

static int ves_qvm_shm_no_notify_region_get(struct ves_qvm_shm_file_ctx *fctx,
					    struct ves_qvm_shm_region **out)
{
	struct ves_qvm_shm_region *existing;
	struct ves_qvm_shm_region *region;
	int ret;

	mutex_lock(&ves_qvm_shm_regions.mutex);
	existing = ves_qvm_shm_no_notify_region_find(fctx->shm_region_name);
	if (existing) {
		if (existing->requested_pages != fctx->requested_pages) {
			mutex_unlock(&ves_qvm_shm_regions.mutex);
			return -EINVAL;
		}
		atomic_inc(&existing->use_count);
		*out = existing;
		mutex_unlock(&ves_qvm_shm_regions.mutex);
		return 0;
	}
	mutex_unlock(&ves_qvm_shm_regions.mutex);

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	strscpy(region->name, fctx->shm_region_name, VES_QVM_SHM_NAME_MAX);
	region->type		= VES_QVM_SHM_NO_NOTIFY_MANY;
	region->requested_pages = fctx->requested_pages;
	region->irq		= -1;
	atomic_set(&region->use_count, 0);
	atomic_set(&region->irq_pending, 0);
	init_waitqueue_head(&region->wq);
	INIT_LIST_HEAD(&region->list_entry);
	spin_lock_init(&region->notify_lock);

	ret = ves_qvm_shm_get_irq(&region->irq);
	if (ret)
		goto free_region;

	if (region->irq < 0) {
		ret = -ENXIO;
		goto free_region;
	}

	/*
	 * Register the handler before ves_qvm_shm_request() attaches the region.
	 * Refer to ESCAN00123878
	 */
	ret = request_irq(region->irq, ves_qvm_shm_no_notify_irq_handler, IRQF_SHARED, region->name,
			  region);
	if (ret) {
		pr_err("%s request_irq failed: %d\n", region->name, ret);
		goto free_region;
	}

	ret = ves_qvm_shm_request(region);
	if (ret)
		goto free_handler;

	/* Needed to protect ves_qvm_shm_regions.list */
	mutex_lock(&ves_qvm_shm_regions.mutex);
	existing = ves_qvm_shm_no_notify_region_find(fctx->shm_region_name);
	if (existing) {
		if (existing->requested_pages != fctx->requested_pages) {
			mutex_unlock(&ves_qvm_shm_regions.mutex);
			ret = -EINVAL;
			goto free_irq_region;
		}
		atomic_inc(&existing->use_count);
		*out = existing;
		mutex_unlock(&ves_qvm_shm_regions.mutex);

		ret = 0;
		goto free_irq_region;
	}

	atomic_set(&region->use_count, 1);
	list_add(&region->list_entry, &ves_qvm_shm_regions.list);
	*out = region;
	mutex_unlock(&ves_qvm_shm_regions.mutex);
	return 0;

free_irq_region : {
	/*
	 * Detach before free_irq(): this is what makes the backend stop asserting the
	 * shared line for us, and it does so without leaving any window where the
	 * status register can still be read - see ves_qvm_shm_detach(). Doing it first
	 * means free_irq()'s own synchronize_irq() has nothing left of ours to wait
	 * out, instead of potentially being starved for as long as the shared line
	 * keeps re-triggering on our own leftover active state.
	 */
	struct guest_shm_control __iomem *const ctrl_page = ves_qvm_shm_detach(region);

	free_irq(region->irq, region);
	ves_qvm_shm_unmap(ctrl_page);
	kfree(region);
	return ret;
}
free_handler:
	free_irq(region->irq, region);
free_region:
	kfree(region);
	return ret;
}

void ves_qvm_shm_no_notify_region_put(struct ves_qvm_shm_region *region)
{
	bool do_release = false;

	if (!region)
		return;

	mutex_lock(&ves_qvm_shm_regions.mutex);
	if (atomic_dec_and_test(&region->use_count)) {
		list_del(&region->list_entry);
		do_release = true;
	}
	mutex_unlock(&ves_qvm_shm_regions.mutex);

	if (do_release) {
		/*
		 * Detach before free_irq(): this is what makes the backend stop asserting the
		 * shared line for us, and it does so without leaving any window where the
		 * status register can still be read - see ves_qvm_shm_detach(). Doing it first
		 * means free_irq()'s own synchronize_irq() has nothing left of ours to wait
		 * out, instead of potentially being starved for as long as the shared line
		 * keeps re-triggering on our own leftover active state.
		 */
		struct guest_shm_control __iomem *const ctrl_page = ves_qvm_shm_detach(region);
		free_irq(region->irq, region);
		ves_qvm_shm_unmap(ctrl_page);
		kfree(region);
	}
}

int ves_qvm_shm_no_notify_attach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *region;
	int ret;

	ret = ves_qvm_shm_no_notify_region_get(fctx, &region);
	if (ret)
		return ret;

	fctx->region = region;
	return 0;
}

static int ves_qvm_shm_no_notify_release(struct inode *inode, struct file *file)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;

	pr_debug("%s - ves_qvm_shm_no_notify_release\n", fctx->shm_region_name);

	ves_qvm_shm_no_notify_region_put(fctx->region);
	kfree(fctx);
	return 0;
}

const struct file_operations ves_qvm_shm_no_notify_fops = {
	.owner		= THIS_MODULE,
	.release	= ves_qvm_shm_no_notify_release,
	.unlocked_ioctl = ves_qvm_shm_common_ioctl,
	.mmap		= ves_qvm_shm_common_mmap,
};
