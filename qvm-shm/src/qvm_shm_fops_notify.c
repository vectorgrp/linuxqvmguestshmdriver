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

/*
 * Read the status register and merge it into the region's latched notification state.
 *
 * Only used in the IRQ handler.
 * Returns true if the status carried an event for this region.
 */
static bool ves_qvm_shm_notify_latch_status(struct ves_qvm_shm_region *region, u32 *status_out)
{
	unsigned long iflags;
	u16 active_now;
	u16 active_prev;
	u16 new_connections;
	u16 notify;
	u32 status;

	/*
	 * The handler is registered before ves_qvm_shm_request() attaches the region, so it can
	 * run while the control page is not mapped yet, because there is no active client in the
	 * backend bound to it. The handler is deregistered after ves_qvm_shm_detach() detaches from
	 * the region, so it can run while the control page cannot be accessed anymore again because
	 * there is no active client in the backend bound to it. In both cases the IRQ is not
	 * related to this region. This handler must ignore it, because status register cannot be
	 * accessed.
	 */
	if (!smp_load_acquire(&region->ctrl_page)) {
		if (status_out)
			*status_out = 0;
		return false;
	}

	/* irqsave, because the read path calls this from process context while the handler is
	 * already registered and can run on another CPU. */
	spin_lock_irqsave(&region->notify_lock, iflags);

	status = ves_qvm_shm_read_status_register(region);
	/* lower 16 bits: pending notifications to us; upper 16 bits: active
	 * clients */
	notify	   = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	active_now = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);

	active_prev	= (u16)region->notifications.active_ids;
	new_connections = (u16)(~active_prev & active_now);

	/*
	 * Reconnect clears stale user notifications from the previous occupant
	 * of that slot, but must not suppress a notification that arrives in
	 * the same IRQ from the new client. Clear first, then OR in current.
	 */
	region->notifications.user_notify_ids &= (__u16)~new_connections;
	region->notifications.user_notify_ids |= (__u16)notify;

	region->notifications.active_ids = active_now;
	region->notifications.disconnected_ids |= (__u16)(active_prev & ~active_now);
	spin_unlock_irqrestore(&region->notify_lock, iflags);

	if (status_out)
		*status_out = status;

	return notify || (active_now != active_prev);
}

static irqreturn_t ves_qvm_shm_notify_irq_handler(int irq, void *region_id)
{
	struct ves_qvm_shm_region *const region = region_id;
	bool has_event;
	u32 status = 0;

	has_event = ves_qvm_shm_notify_latch_status(region, &status);

	if (!has_event) {
		pr_debug_ratelimited("%s - shm_notify_irq_handler - IRQ_NONE irq=%d status=NA\n",
				     region->name, irq);
		return IRQ_NONE;
	}

	pr_debug_ratelimited("%s - shm_notify_irq_handler - IRQ_HANDLED irq=%d status=%#x\n",
			     region->name, irq, status);
	atomic_set(&region->irq_pending, 1);
	wake_up_interruptible(&region->wq);
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
	region->irq		= -1;
	atomic_set(&region->use_count, 1);
	atomic_set(&region->irq_pending, 0);
	init_waitqueue_head(&region->wq);
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
	ret = request_irq(region->irq, ves_qvm_shm_notify_irq_handler, IRQF_SHARED, region->name,
			  region);
	if (ret) {
		pr_err("%s request_irq failed: %d\n", region->name, ret);
		goto free_region;
	}

	ret = ves_qvm_shm_request(region);
	if (ret)
		goto free_handler;

	*out = region;
	return 0;

free_handler:
	free_irq(region->irq, region);
free_region:
	kfree(region);
	return ret;
}

void ves_qvm_shm_notify_region_put(struct ves_qvm_shm_region *region)
{
	if (region && atomic_dec_and_test(&region->use_count)) {
		struct guest_shm_control __iomem *ctrl_page;

		/*
		 * Detach before free_irq(): this is what makes the backend stop asserting the
		 * shared line for us, and it does so without leaving any window where the
		 * status register can still be read - see ves_qvm_shm_detach(). Doing it first
		 * means free_irq()'s own synchronize_irq() has nothing left of ours to wait
		 * out, instead of potentially being starved for as long as the shared line
		 * keeps re-triggering on our own leftover active state.
		 */
		ctrl_page = ves_qvm_shm_detach(region);

		free_irq(region->irq, region);

		ves_qvm_shm_unmap(ctrl_page);

		kfree(region);
	}
}

int ves_qvm_shm_notify_attach(struct ves_qvm_shm_file_ctx *fctx)
{
	struct ves_qvm_shm_region *region;
	int ret;

	/*
	 * region_get() also seeds the notification state and makes the region's
	 * IRQ handler live, so there is nothing left to fail here.
	 */
	ret = ves_qvm_shm_notify_region_get(fctx, &region);
	if (ret)
		return ret;

	fctx->region = region;
	return 0;
}

static int ves_qvm_shm_notify_release(struct inode *inode, struct file *file)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;

	pr_debug("%s - ves_qvm_shm_notify_release\n", fctx->shm_region_name);

	/*
	 * Nothing to unhook from the region: the handler only ever touches the
	 * region, which keeps its own notification state and stays serviceable
	 * until the last put frees its IRQ. If an mmap outlives this file,
	 * interrupts keep being acknowledged with no consumer to read the result.
	 */
	ves_qvm_shm_notify_region_put(fctx->region);
	kfree(fctx);
	return 0;
}

static ssize_t ves_qvm_shm_notify_read(struct file *file, char __user *buf, size_t count,
				       loff_t *offset)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;
	struct ves_qvm_shm_region *const region = fctx->region;
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
			if (wait_event_interruptible(region->wq,
						     atomic_read(&region->irq_pending) != 0)) {
				return -ERESTARTSYS;
			}
		} else if (atomic_read(&region->irq_pending) == 0) {
			return -EAGAIN;
		}
	} while (atomic_xchg(&region->irq_pending, 0) == 0);

	/*
	 * Consume the accumulated events. active_ids is deliberately left alone:
	 * it is current state rather than an accumulated event, and the IRQ handler
	 * uses it as its connect/disconnect edge detection baseline.
	 */
	spin_lock_irqsave(&region->notify_lock, iflags);
	event				       = region->notifications;
	region->notifications.disconnected_ids = 0;
	region->notifications.user_notify_ids  = 0;
	spin_unlock_irqrestore(&region->notify_lock, iflags);

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

	poll_wait(file, &fctx->region->wq, wait);
	if (atomic_read(&fctx->region->irq_pending) != 0)
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
