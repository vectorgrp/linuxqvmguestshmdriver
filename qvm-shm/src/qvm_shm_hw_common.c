// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

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

static void ves_qvm_shm_hw_create_region(const char *name, u32 requested_pages)
{
	size_t len = strnlen(name, VES_QVM_SHM_NAME_MAX - 1);

	memset_io(&ves_qvm_shm_hw_state.factory->name[0], 0, VES_QVM_SHM_NAME_MAX);
	memcpy_toio(&ves_qvm_shm_hw_state.factory->name[0], name, len);

	/*
	 * guest_shm_create() comes from the QNX public header and only wraps a
	 * plain store with compiler barriers. For Linux MMIO we need the
	 * accessors so the size write uses the architecture-specific MMIO
	 * ordering rules.
	 */
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

int ves_qvm_shm_request(struct ves_qvm_shm_region *region, int *irq_out)
{
	u32 client_id;
	u32 mapped_pages;
	u32 status;
	phys_addr_t ctrl_phys;
	int ret;

	/* The factory page is shared across all requests and must stay serialized. */
	mutex_lock(&ves_qvm_shm_hw_state.mutex);
	if (!ves_qvm_shm_hw_state.factory || !ves_qvm_shm_hw_state.device) {
		ret = -ENODEV;
		pr_warn_ratelimited("shm region request failed: no backend bound yet\n");
		goto unlock;
	}

	ves_qvm_shm_hw_create_region(region->name, region->requested_pages);

	status = ioread32(&ves_qvm_shm_hw_state.factory->status);
	ret    = ves_qvm_shm_status_to_errno(status);
	if (ret)
		goto unlock;

	if (irq_out)
		*irq_out = ves_qvm_shm_hw_state.irq;

	mapped_pages = ioread32(&ves_qvm_shm_hw_state.factory->size);
	ctrl_phys    = (phys_addr_t)ves_qvm_shm_hw_read_shmem_addr(ves_qvm_shm_hw_state.factory);
	region->shm_phys     = ctrl_phys + PAGE_SIZE;
	region->mapped_pages = mapped_pages;

	region->ctrl_page = (struct guest_shm_control __iomem *)ioremap(ctrl_phys, PAGE_SIZE);
	if (!region->ctrl_page) {
		ret = -ENOMEM;
		goto unlock;
	}

	client_id = ioread32(&region->ctrl_page->idx);
	/* client_id should always be [0..15] since it is set by the hypervisor */
	if (WARN_ON_ONCE(client_id >= VES_QVM_SHM_STATUS_ACTIVE_SHIFT)) {
		ret = -EIO;
		goto unmap_ctrl_page;
	}
	if (region->type == VES_QVM_SHM_NOTIFY_TWO && client_id > 1) {
		ret = -EBUSY;
		goto unmap_ctrl_page;
	}
	region->client_id = (u8)client_id;

	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return 0;

unmap_ctrl_page:
	iounmap(region->ctrl_page);
	region->ctrl_page = NULL;

unlock:
	mutex_unlock(&ves_qvm_shm_hw_state.mutex);
	return ret;
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

void ves_qvm_shm_release(struct ves_qvm_shm_region *region)
{
	if (region->ctrl_page) {
		iowrite32(1, &region->ctrl_page->detach);
		iounmap(region->ctrl_page);
		region->ctrl_page = NULL;
	}
}

void ves_qvm_shm_send_notification(struct ves_qvm_shm_region *region, u32 target)
{
	if (WARN_ON_ONCE(!region || !region->ctrl_page))
		return;

	iowrite32(target, &region->ctrl_page->notify);
}

u32 ves_qvm_shm_read_status_register(struct ves_qvm_shm_file_ctx *shm)
{
	if (WARN_ON_ONCE(!shm || !shm->region || !shm->region->ctrl_page))
		return 0;

	return ioread32(&shm->region->ctrl_page->status);
}
