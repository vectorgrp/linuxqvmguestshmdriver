// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_COMPAT_H
#define QVM_SHM_COMPAT_H

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/version.h>

/*
 * =============================================================================
 * PCI Compatibility - managed resources
 * =============================================================================
 *
 * pcim_iomap_region() was added in Linux 6.12.0. For older kernels, provide a
 * compatibility wrapper that replicates the behavior, especially regarding
 * error handling.
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static inline void __iomem *pcim_iomap_region_compat(struct pci_dev *pdev, int bar,
						     const char *name)
{
	void __iomem *addr;
	int ret;

	/*
	 * pdev is managed (see pcim_enable_device()), so pci_request_region() above already
	 * registered this region for automatic release via pcim_release()'s region_mask
	 * tracking. No explicit devres action needed here.
	 */
	ret = pci_request_region(pdev, bar, name);
	if (ret)
		return IOMEM_ERR_PTR(ret);

	addr = pcim_iomap(pdev, bar, 0);
	if (!addr)
		return IOMEM_ERR_PTR(-ENOMEM);

	return addr;
}
#define pcim_iomap_region pcim_iomap_region_compat

#endif

/*
 * =============================================================================
 * PCI Compatibility - IRQ
 * =============================================================================
 *
 * PCI_IRQ_LEGACY got renamed to PCI_IRQ_INTX in Linux 6.10.0.
 * See https://cdn.kernel.org/pub/linux/kernel/v6.x/ChangeLog-6.10
 */
#ifndef PCI_IRQ_INTX
#define PCI_IRQ_INTX PCI_IRQ_LEGACY
#endif

/*
 * =============================================================================
 * Class Creation Compatibility
 * =============================================================================
 *
 * class_create() changed its signature in Linux 6.4.0 and no longer takes a
 * module owner argument. Keep legacy two-argument call sites unchanged by
 * mapping class_create(owner, name) to class_create(name) on newer kernels.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define class_create(_owner, _name) class_create(_name)
#endif

#endif /* QVM_SHM_COMPAT_H */
