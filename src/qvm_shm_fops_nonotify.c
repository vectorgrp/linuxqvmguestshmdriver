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

static irqreturn_t ves_qvm_shm_no_notify_irq_handler(int irq, void *fctx_id)
{
	struct ves_qvm_shm_file_ctx *const fctx = fctx_id;
	bool has_event;
	u16 active_now;
	u16 active_prev;
	u16 notify;
	u32 status;

	/*
	 * NO_NOTIFY files do not surface IRQ events to userspace. We only
	 * read status to service/ack the backend condition.
	 */
	status = ves_qvm_shm_read_status_register(fctx);
	/* lower 16 bits: pending notifications to us; upper 16 bits: active
	 * clients */
	notify	   = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	active_now = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);

	spin_lock(&fctx->notify_lock);
	active_prev		       = (u16)fctx->notifications.active_ids;
	fctx->notifications.active_ids = active_now;
	spin_unlock(&fctx->notify_lock);

	has_event = notify || (active_now != active_prev);

	if (!has_event) {
		pr_debug_ratelimited(
			"%s - shm_no_notify_irq_handler - IRQ_NONE irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
			fctx->shm_region_name, irq, status, notify, active_now, active_prev);
		return IRQ_NONE;
	}

	pr_debug_ratelimited(
		"%s - shm_no_notify_irq_handler - IRQ_HANDLED irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
		fctx->shm_region_name, irq, status, notify, active_now, active_prev);
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
		return ves_qvm_shm_get_irq(&fctx->irq);
	}
	mutex_unlock(&ves_qvm_shm_regions.mutex);

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	strscpy(region->name, fctx->shm_region_name, VES_QVM_SHM_NAME_MAX);
	region->type		= VES_QVM_SHM_NO_NOTIFY_MANY;
	region->requested_pages = fctx->requested_pages;
	atomic_set(&region->use_count, 0);
	INIT_LIST_HEAD(&region->list_entry);

	ret = ves_qvm_shm_request(region, &fctx->irq);
	if (ret)
		goto release_shm;

	mutex_lock(&ves_qvm_shm_regions.mutex);
	existing = ves_qvm_shm_no_notify_region_find(fctx->shm_region_name);
	if (existing) {
		if (existing->requested_pages != fctx->requested_pages) {
			mutex_unlock(&ves_qvm_shm_regions.mutex);
			ret = -EINVAL;
			goto release_shm;
		}
		atomic_inc(&existing->use_count);
		*out = existing;
		mutex_unlock(&ves_qvm_shm_regions.mutex);

		ret = 0;
		goto release_shm;
	}

	atomic_set(&region->use_count, 1);
	list_add(&region->list_entry, &ves_qvm_shm_regions.list);
	*out = region;
	mutex_unlock(&ves_qvm_shm_regions.mutex);
	return 0;

release_shm:
	ves_qvm_shm_release(region);
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
		ves_qvm_shm_release(region);
		kfree(region);
	}
}

int ves_qvm_shm_no_notify_attach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *region;
	u32 status;
	int ret;

	ret = ves_qvm_shm_no_notify_region_get(fctx, &region);
	if (ret)
		return ret;

	fctx->region = region;

	/* Init active_ids based on current status so that the reconnect logic
	 * in the IRQ handler works correctly on the first notification.
	 */
	status			       = ves_qvm_shm_read_status_register(fctx);
	fctx->notifications.active_ids = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);

	ret = request_irq(fctx->irq, ves_qvm_shm_no_notify_irq_handler, IRQF_SHARED,
			  fctx->shm_region_name, fctx);
	if (ret) {
		pr_err("%s request_irq (%d) failed: %d\n", fctx->shm_region_name, fctx->irq, ret);
		goto put_region;
	}

	return 0;

put_region:
	ves_qvm_shm_no_notify_detach(fctx);
	return ret;
}

void ves_qvm_shm_no_notify_detach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *const region = fctx->region;

	if (fctx->irq >= 0) {
		free_irq(fctx->irq, fctx);
		fctx->irq = -1;
	}

	ves_qvm_shm_no_notify_region_put(region);
	fctx->region = NULL;
}

static int ves_qvm_shm_no_notify_release(struct inode *inode, struct file *file)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;

	pr_debug("%s - ves_qvm_shm_no_notify_release\n", fctx->shm_region_name);

	ves_qvm_shm_no_notify_detach(fctx);
	kfree(fctx);
	return 0;
}

const struct file_operations ves_qvm_shm_no_notify_fops = {
	.owner		= THIS_MODULE,
	.release	= ves_qvm_shm_no_notify_release,
	.unlocked_ioctl = ves_qvm_shm_common_ioctl,
	.mmap		= ves_qvm_shm_common_mmap,
};
