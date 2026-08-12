// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_HW_INTERNAL_H
#define QVM_SHM_HW_INTERNAL_H

#include <linux/device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct guest_shm_factory;

/* Global backend binding and request serialization state. */
struct ves_qvm_shm_hw_state {
	struct device *device;
	struct guest_shm_factory __iomem *factory;
	int irq;
	struct mutex mutex;
};

int ves_qvm_shm_pci_init(void);
void ves_qvm_shm_pci_exit(void);
int ves_qvm_shm_mmio_init(void);
void ves_qvm_shm_mmio_exit(void);
bool ves_qvm_shm_mmio_addr_filter_enabled(void);

int ves_qvm_shm_hw_bind(struct device *device, struct guest_shm_factory __iomem *factory, int irq);
void ves_qvm_shm_hw_unbind(struct device *device);
u64 ves_qvm_shm_hw_read_factory_signature(struct guest_shm_factory __iomem *factory);
u64 ves_qvm_shm_hw_read_shmem_addr(struct guest_shm_factory __iomem *factory);

#endif /* QVM_SHM_HW_INTERNAL_H */
