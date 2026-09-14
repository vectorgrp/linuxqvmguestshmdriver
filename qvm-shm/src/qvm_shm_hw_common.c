// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#include <linux/interrupt.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io.h>
#include <linux/module.h>

#include "guest_shm.h"
#include "qvm_shm_hw.h"
#include "qvm_shm_hw_internal.h"
#include "qvm_shm_types.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

static struct ves_qvm_shm_hw_state ves_qvm_shm_hw_state;

static int ves_qvm_shm_status_to_errno(u32 status)
{
	switch (status) {
	case GSS_OK:
		return 0;
	case GSS_NOMEM:
		return -ENOMEM;
	case GSS_CLIENT_MAX:
		return -EAGAIN;
	case GSS_ILLEGAL_NAME:
		return -EINVAL;
	case GSS_NO_PERMISSION:
		return -EACCES;
	case GSS_DOES_NOT_EXIST:
		return -ENOENT;
	case GSS_UNKNOWN_FAILURE:
	default:
		return -EIO;
	}
}

static void ves_qvm_shm_hw_write_region_name(const char *name)
{
	size_t len = strnlen(name, VES_QVM_SHM_NAME_MAX - 1);

	memset_io(&ves_qvm_shm_hw_state.factory->name[0], 0, VES_QVM_SHM_NAME_MAX);
	memcpy_toio(&ves_qvm_shm_hw_state.factory->name[0], name, len);
}

/*
 * The size write is what makes our client slot live in the backend: the caller must have the IRQ
 * implications handled (line masked or handler able to answer) before calling this.
 *
 * guest_shm_create() comes from the QNX public header and only wraps a
 * plain store with compiler barriers. For Linux MMIO we need the
 * accessors so the size write uses the architecture-specific MMIO
 * ordering rules.
 */
static void ves_qvm_shm_hw_trigger_region_creation(u32 requested_pages)
{
	iowrite32(requested_pages, &ves_qvm_shm_hw_state.factory->size);
}

int ves_qvm_shm_hw_bind(struct device *device, struct guest_shm_factory __iomem *factory, int irq)
{
	int ret = 0;

	/* Only one backend instance may be active at a time. */
	mutex_lock(&ves_qvm_shm_hw_state.mutex);
	if (ves_qvm_shm_hw_state.device) {
		ret = -EBUSY;
		goto unlock;
	}

	ves_qvm_shm_hw_state.device  = device;
	ves_qvm_shm_hw_state.factory = factory;
	ves_qvm_shm_hw_state.irq     = irq;

unlock:
	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return ret;
}

void ves_qvm_shm_hw_unbind(struct device *device)
{
	mutex_lock(&ves_qvm_shm_hw_state.mutex);
	if (ves_qvm_shm_hw_state.device == device) {
		ves_qvm_shm_hw_state.device  = NULL;
		ves_qvm_shm_hw_state.factory = NULL;
		ves_qvm_shm_hw_state.irq     = -1;
	}
	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
}

u64 ves_qvm_shm_hw_read_factory_signature(struct guest_shm_factory __iomem *factory)
{
	return lo_hi_readq((const volatile void __iomem *)&factory->signature);
}

u64 ves_qvm_shm_hw_read_shmem_addr(struct guest_shm_factory __iomem *factory)
{
	return lo_hi_readq((const volatile void __iomem *)&factory->shmem);
}

int ves_qvm_shm_hw_init(void)
{
	mutex_init(&ves_qvm_shm_hw_state.mutex);
	ves_qvm_shm_hw_state.device  = NULL;
	ves_qvm_shm_hw_state.factory = NULL;
	ves_qvm_shm_hw_state.irq     = -1;

	if (ves_qvm_shm_mmio_addr_filter_enabled()) {
		pr_info("MMIO address filter active, selecting MMIO backend\n");
		return ves_qvm_shm_mmio_init();
	}

	return ves_qvm_shm_pci_init();
}

void ves_qvm_shm_hw_exit(void)
{
	if (ves_qvm_shm_mmio_addr_filter_enabled()) {
		ves_qvm_shm_mmio_exit();
		return;
	}

	ves_qvm_shm_pci_exit();
}

int ves_qvm_shm_get_irq(int *irq_out)
{
	int ret = 0;

	if (!irq_out)
		return -EINVAL;

	mutex_lock(&ves_qvm_shm_hw_state.mutex);
	if (!ves_qvm_shm_hw_state.device) {
		ret = -ENODEV;
		goto unlock;
	}

	*irq_out = ves_qvm_shm_hw_state.irq;

unlock:
	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return ret;
}

int ves_qvm_shm_request(struct ves_qvm_shm_region *region)
{
	struct guest_shm_control __iomem *ctrl_page;
	u32 client_id;
	u32 mapped_pages;
	u32 status;
	phys_addr_t ctrl_phys;
	int ret;

	bool irq_masked = false;

	/* The factory page is shared across all requests and must stay serialized. */
	mutex_lock(&ves_qvm_shm_hw_state.mutex);
	if (!ves_qvm_shm_hw_state.factory || !ves_qvm_shm_hw_state.device) {
		ret = -ENODEV;
		pr_warn_ratelimited("shm region request failed: no backend bound yet\n");
		goto unlock;
	}

	/* parallel unplug could lead to invalid irq being used.*/
	if (ves_qvm_shm_hw_state.irq != region->irq) {
		ret = -ENODEV;
		pr_warn_ratelimited("shm region request failed: inconsistent backend irq found\n");
		goto unlock;
	}

	/*
	 * Written before the mask on purpose: the name writes do not make the slot live, and they
	 * are the most expensive accesses on the factory page (byte/word MMIO traps), so keeping
	 * them outside shortens the masked window. The factory mutex above is what keeps them
	 * paired with the creation trigger below.
	 */
	ves_qvm_shm_hw_write_region_name(region->name);

	/*
	 * Mask the line for the whole attach window below, before the region exists in the backend.
	 * From ves_qvm_shm_hw_trigger_region_creation() onwards our client slot is live, so any
	 * peer attaching or detaching raises a connect/disconnect event on it - but our handler
	 * NULL-guards until ctrl_page is published at the end of this function, and no other
	 * handler can consume our slot's event, because the status read is per-slot. The
	 * level-triggered line therefore stays asserted with nothing able to deassert it, which is
	 * what the kernel eventually reports as "nobody cared" before disabling the line for every
	 * peer on it.
	 *
	 * The window is not short: it spans two factory reads and an ioremap(). Masking is what
	 * makes it unobservable; nothing is lost, because a pending assert is re-delivered by
	 * enable_irq() once we can actually answer it.
	 */
	disable_irq(region->irq);
	irq_masked = true;

	ves_qvm_shm_hw_trigger_region_creation(region->requested_pages);

	status = ioread32(&ves_qvm_shm_hw_state.factory->status);
	ret    = ves_qvm_shm_status_to_errno(status);
	if (ret)
		goto unlock;

	mapped_pages = ioread32(&ves_qvm_shm_hw_state.factory->size);
	ctrl_phys    = (phys_addr_t)ves_qvm_shm_hw_read_shmem_addr(ves_qvm_shm_hw_state.factory);
	region->shm_phys     = ctrl_phys + PAGE_SIZE;
	region->mapped_pages = mapped_pages;

	/*
	 * Map and validate through a local pointer; it is published only once the region is
	 * complete - see the release store below.
	 */
	ctrl_page = (struct guest_shm_control __iomem *)ioremap(ctrl_phys, PAGE_SIZE);
	if (!ctrl_page) {
		ret = -ENOMEM;
		goto unlock;
	}

	client_id = ioread32(&ctrl_page->idx);
	/* client_id should always be [0..15] since it is set by the hypervisor */
	if (WARN_ON_ONCE(client_id >= VES_QVM_SHM_STATUS_ACTIVE_SHIFT)) {
		ret = -EIO;
		goto detach_ctrl_page;
	}
	if (region->type == VES_QVM_SHM_NOTIFY_TWO && client_id > 1) {
		pr_err("%s invalid client_id %d for TWO_PEERS\n", region->name, client_id);
		ret = -EBUSY;
		goto detach_ctrl_page;
	}
	region->client_id = (u8)client_id;

	/*
	 * Release store, and deliberately the last thing this function does: the region's
	 * interrupt handler is already registered at this point and uses ctrl_page to decide
	 * whether there is anything to read. Publishing only here means a handler that observes
	 * the pointer also observes the established mapping behind it and every region field
	 * filled in above - shm_phys, mapped_pages and client_id.
	 */
	smp_store_release(&region->ctrl_page, ctrl_page);

	/*
	 * Seed the notification state from the current status. This is the first status
	 * read operation. Reading status clears the user notifications, so they are stored
	 * and irq_pending ensures that the user can consume them.
	 * No notify mutex is needed because the interrupts are disabled.
	 */
	status				      = ves_qvm_shm_read_status_register(region);
	region->notifications.active_ids      = (u16)(status >> VES_QVM_SHM_STATUS_ACTIVE_SHIFT);
	region->notifications.user_notify_ids = (u16)(status & VES_QVM_SHM_STATUS_NOTIFY_MASK);
	if (region->notifications.user_notify_ids)
		atomic_set(&region->irq_pending, 1);

	/*
	 * From here the handler can answer for this region, so re-delivery of anything that
	 * accumulated while the line was masked is now safe to take.
	 */
	enable_irq(region->irq);

	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return 0;

detach_ctrl_page:
	/*
	 * ctrl_page was never published, so no handler can have loaded it: unlike
	 * ves_qvm_shm_detach() this needs neither a NULL store nor synchronize_irq(). Detach
	 * before dropping the mapping anyway - it releases our client slot in the backend and
	 * stops it asserting the shared line on our behalf, which the caller's free_irq() would
	 * otherwise have to wait out.
	 */
	iowrite32(1, &ctrl_page->detach);
	iounmap(ctrl_page);

unlock:
	/*
	 * Balances the mask above on every failing path. The detach write on the
	 * detach_ctrl_page path has already stopped the backend asserting for our slot, so
	 * unmasking cannot resurrect a storm. The one exception is a failed ioremap(): the slot
	 * is live but the detach register is only reachable through a mapping we do not have, so
	 * that pre-existing hole stays - it needs an -ENOMEM from ioremap() to be reached at all.
	 */
	if (irq_masked)
		enable_irq(region->irq);

	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return ret;
}

struct guest_shm_control __iomem *ves_qvm_shm_detach(struct ves_qvm_shm_region *region)
{
	struct guest_shm_control __iomem *ctrl_page = region->ctrl_page;

	if (ctrl_page) {
		/*
		 * Between the NULL store and the detach write nobody can read this region's status
		 * register, so the backend would keep the level-triggered line asserted with every
		 * handler on it answering IRQ_NONE. Masking makes that window unobservable. Nothing
		 * is lost: the line is level-triggered, so a pending assert is re-delivered by
		 * enable_irq().
		 *
		 */
		disable_irq(region->irq);

		/*
		 * Publish NULL first: every handler invocation that has not yet loaded
		 * ctrl_page will observe NULL and return without touching hardware at all,
		 * so it can no longer race the detach write below.
		 */
		smp_store_release(&region->ctrl_page, NULL);

		/*
		 * Now the true last register access for this region - through our local copy,
		 * since region->ctrl_page is already NULL. This is what makes the backend stop
		 * asserting the shared line on our behalf, so it must happen before free_irq():
		 * otherwise our own leftover active state keeps re-triggering the (now
		 * NULL-guarded, so cheap) handler for as long as free_irq()'s own
		 * synchronize_irq() takes to notice the line is quiet, which on a genuinely
		 * storming shared line can never happen.
		 */
		iowrite32(1, &ctrl_page->detach);

		/*
		 * Balanced with the disable_irq() above, and before the caller's free_irq():
		 * the backend has stopped asserting on our behalf by now, so re-enabling
		 * cannot resurrect the storm this masking avoided. The depth is per-line and
		 * refcounted, so a concurrent detach of another region nests correctly.
		 */
		enable_irq(region->irq);
	}

	return ctrl_page;
}

void ves_qvm_shm_unmap(struct guest_shm_control __iomem *ctrl_page)
{
	if (ctrl_page)
		iounmap(ctrl_page);
}
void ves_qvm_shm_send_notification(struct ves_qvm_shm_region *region, u32 target)
{
	if (WARN_ON_ONCE(!region || !region->ctrl_page))
		return;

	iowrite32(target, &region->ctrl_page->notify);
}

u32 ves_qvm_shm_read_status_register(struct ves_qvm_shm_region *region)
{
	if (WARN_ON_ONCE(!region || !region->ctrl_page))
		return 0;

	return ioread32(&region->ctrl_page->status);
}
