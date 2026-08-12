// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * File operations for notify SHM files.
 */

#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "guest_shm.h"
#include "qvm_shm_fops_common.h"
#include "qvm_shm_fops_notify.h"
#include "qvm_shm_hw.h"
#include "qvm_shm_types.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

static irqreturn_t ves_qvm_shm_notify_irq_handler(int irq, void *fctx_id)
{
	struct ves_qvm_shm_file_ctx *const fctx = fctx_id;
	bool has_event;
	u16 active_now;
	u16 active_prev;
	u16 new_connections;
	u16 notify;
	u32 status;

	status = ves_qvm_shm_read_status_register(fctx);
	/* lower 16 bits: pending notifications to us; upper 16 bits: active
	 * clients */
	notify	   = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	active_now = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);

	spin_lock(&fctx->notify_lock);
	active_prev	= (u16)fctx->notifications.active_ids;
	new_connections = (u16)(~active_prev & active_now);

	/* Reconnect clears stale user notifications from the previous occupant
	 * of that slot, but must not suppress a notification that arrives in
	 * the same IRQ from the new client. Clear first, then OR in current. */
	fctx->notifications.user_notify_ids &= (__u16)~new_connections;
	fctx->notifications.user_notify_ids |= (__u16)notify;

	fctx->notifications.active_ids = active_now;
	fctx->notifications.disconnected_ids |= (__u16)(active_prev & ~active_now);
	spin_unlock(&fctx->notify_lock);

	has_event = notify || (active_now != active_prev);

	if (!has_event) {
		pr_debug_ratelimited(
			"%s - shm_notify_irq_handler - IRQ_NONE irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
			fctx->shm_region_name, irq, status, notify, active_now, active_prev);
		return IRQ_NONE;
	}

	pr_debug_ratelimited(
		"%s - shm_notify_irq_handler - IRQ_HANDLED irq=%d status=%#x notify=%#x active_now=%#x active_prev=%#x\n",
		fctx->shm_region_name, irq, status, notify, active_now, active_prev);
	atomic_set(&fctx->irq_pending, 1);
	wake_up_interruptible(&fctx->wq);
	return IRQ_HANDLED;
}

static int ves_qvm_shm_notify_region_get(struct ves_qvm_shm_file_ctx *fctx,
					 struct ves_qvm_shm_region **out)
{
	struct ves_qvm_shm_region *region;
	int ret;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	strscpy(region->name, fctx->shm_region_name, VES_QVM_SHM_NAME_MAX);
	region->type		= fctx->type;
	region->requested_pages = fctx->requested_pages;
	atomic_set(&region->use_count, 1);
	fctx->irq = -1;

	ret = ves_qvm_shm_request(region, &fctx->irq);
	if (ret)
		goto free_region;

	if (fctx->type == VES_QVM_SHM_NOTIFY_TWO && region->client_id > 1) {
		pr_err("%s invalid client_id %d for TWO_PEERS\n", fctx->shm_region_name,
		       region->client_id);
		ret = -EINVAL;
		goto release_region;
	}

	*out = region;
	return 0;

release_region:
	ves_qvm_shm_release(region);
free_region:
	kfree(region);
	return ret;
}

void ves_qvm_shm_notify_region_put(struct ves_qvm_shm_region *region)
{
	if (region && atomic_dec_and_test(&region->use_count)) {
		ves_qvm_shm_release(region);
		kfree(region);
	}
}

int ves_qvm_shm_notify_attach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *region;
	u32 status;
	int ret;

	ret = ves_qvm_shm_notify_region_get(fctx, &region);
	if (ret)
		return ret;
	fctx->region = region;

	/*
	 * Init active_ids based on current status so that the reconnect logic in the IRQ handler
	 * works correctly on the first notification. To prevent hiding notifications, we set
	 * irq_pending if there are any user notifications.
	 *
	 * Potential race condition: If a notification arrives after reading the status but before
	 * registering the IRQ handler and this doesn't trigger the IRQ, we might miss it (It's not
	 * lost and will be handled in the next IRQ). However, the protocol of the upper layer
	 * (hypAb) prevents this scenario.
	 *
	 * Lock is not yet needed here, because no other thread can access the file context yet.
	 */
	status				    = ves_qvm_shm_read_status_register(fctx);
	fctx->notifications.active_ids	    = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);
	fctx->notifications.user_notify_ids = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	if (fctx->notifications.user_notify_ids)
		atomic_set(&fctx->irq_pending, 1);

	if (fctx->irq < 0) {
		ret = -ENXIO;
		goto put_region;
	}
	ret = request_irq(fctx->irq, ves_qvm_shm_notify_irq_handler, IRQF_SHARED,
			  fctx->shm_region_name, fctx);
	if (ret) {
		pr_err("%s request_irq failed: %d\n", fctx->shm_region_name, ret);
		goto put_region;
	}

	return 0;

put_region:
	ves_qvm_shm_notify_detach(fctx);
	return ret;
}

void ves_qvm_shm_notify_detach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *const region = fctx->region;

	if (fctx->irq >= 0) {
		free_irq(fctx->irq, fctx);
		fctx->irq = -1;
	}

	fctx->region = NULL;
	atomic_set(&fctx->irq_pending, 0);
	ves_qvm_shm_notify_region_put(region);
}

static int ves_qvm_shm_notify_release(struct inode *inode, struct file *file)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;

	pr_debug("%s - ves_qvm_shm_notify_release\n", fctx->shm_region_name);

	ves_qvm_shm_notify_detach(fctx);
	kfree(fctx);
	return 0;
}

static ssize_t ves_qvm_shm_notify_read(struct file *file, char __user *buf, size_t count,
				       loff_t *offset)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;
	struct ves_qvm_shm_read_notify event;
	unsigned long iflags;

	pr_debug("%s - ves_qvm_shm_notify_read\n", fctx->shm_region_name);

	if (count < sizeof(event))
		return -EINVAL;

	/*
	 * Retry if another reader or a racing IRQ cleared irq_pending before our atomic_xchg().
	 * We only proceed after successfully consuming the event.
	 */
	do {
		if (!(file->f_flags & O_NONBLOCK)) {
			if (wait_event_interruptible(fctx->wq,
						     atomic_read(&fctx->irq_pending) != 0)) {
				return -ERESTARTSYS;
			}
		} else if (atomic_read(&fctx->irq_pending) == 0) {
			return -EAGAIN;
		}
	} while (atomic_xchg(&fctx->irq_pending, 0) == 0);

	spin_lock_irqsave(&fctx->notify_lock, iflags);
	event				     = fctx->notifications;
	fctx->notifications.disconnected_ids = 0;
	fctx->notifications.user_notify_ids  = 0;
	spin_unlock_irqrestore(&fctx->notify_lock, iflags);

	if (!event.active_ids && !event.disconnected_ids && !event.user_notify_ids) {
		return -EAGAIN;
	}

	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	return sizeof(event);
}

static ssize_t ves_qvm_shm_notify_write(struct file *file, const char __user *buf, size_t count,
					loff_t *offset)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;
	struct ves_qvm_shm_write_notify cmd;

	pr_debug("%s - ves_qvm_shm_notify_write\n", fctx->shm_region_name);

	if (count != sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	if (cmd.target != VES_QVM_SHM_NOTIFY_ALL && cmd.target != VES_QVM_SHM_NOTIFY_ALL_BUT_ME &&
	    (cmd.target & ~VES_QVM_SHM_NOTIFY_ALL)) {
		return -EINVAL;
	}

	ves_qvm_shm_send_notification(fctx->region, cmd.target);

	return (ssize_t)count;
}

static __poll_t ves_qvm_shm_notify_poll(struct file *file, poll_table *wait)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;
	__poll_t mask				= 0;

	pr_debug("%s - ves_qvm_shm_notify_poll\n", fctx->shm_region_name);

	poll_wait(file, &fctx->wq, wait);
	if (atomic_read(&fctx->irq_pending) != 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

const struct file_operations ves_qvm_shm_notify_fops = {
	.owner		= THIS_MODULE,
	.release	= ves_qvm_shm_notify_release,
	.read		= ves_qvm_shm_notify_read,
	.write		= ves_qvm_shm_notify_write,
	.unlocked_ioctl = ves_qvm_shm_common_ioctl,
	.mmap		= ves_qvm_shm_common_mmap,
	.poll		= ves_qvm_shm_notify_poll,
};
