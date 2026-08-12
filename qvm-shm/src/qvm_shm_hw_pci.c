// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * Southbound PCI backend:
 * guest_shm factory page on BAR0 and control page per region.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/types.h>

#include "guest_shm.h"
#include "qvm_shm_compat.h"
#include "qvm_shm_hw_internal.h"
#include "qvm_shm_types.h"

/*
 * Use managed PCI resource allocation to simplify error handling. All resources allocated via
 * pcim_* are automatically released on probe failure or device removal. Also, don't call
 * pci_free_irq_vectors() as IRQ vectors are automatically managed when using pcim_enable_device().
 */

static const struct pci_device_id ves_qvm_shm_pci_ids[] = { { PCI_DEVICE(PCI_VID_BlackBerry_QNX,
									 PCI_DID_QNX_GUEST_SHM) },
							    {} };
MODULE_DEVICE_TABLE(pci, ves_qvm_shm_pci_ids);

static int ves_qvm_shm_pci_probe(struct pci_dev *device, const struct pci_device_id *id)
{
	struct guest_shm_factory __iomem *factory;
	int irq;
	int ret;

	ret = pcim_enable_device(device);
	if (ret) {
		dev_err(&device->dev, "pcim_enable_device failed: %d\n", ret);
		return ret;
	}

	factory = pcim_iomap_region(device, 0, VES_QVM_SHM_DRIVER_NAME);
	if (IS_ERR(factory)) {
		dev_err(&device->dev, "pcim_iomap_region(BAR0) failed\n");
		return -ENODEV;
	}

	if (ves_qvm_shm_hw_read_factory_signature(factory) != GUEST_SHM_SIGNATURE) {
		dev_err(&device->dev, "SHM signature incorrect. %llx != %llx",
			(unsigned long long)GUEST_SHM_SIGNATURE,
			(unsigned long long)ves_qvm_shm_hw_read_factory_signature(factory));
		return -ENXIO;
	}

	ret = pci_alloc_irq_vectors(device, 1, 1, PCI_IRQ_INTX);
	if (ret < 0) {
		dev_err(&device->dev, "pci_alloc_irq_vectors(INTx) failed: %d\n", ret);
		return ret;
	}

	/* nr must be 0 for INTx IRQs */
	irq = pci_irq_vector(device, 0);
	if (irq < 0) {
		dev_err(&device->dev, "pci_irq_vector(INTx) failed: %d\n", irq);
		ret = irq;
		return ret;
	}

	ret = ves_qvm_shm_hw_bind(&device->dev, factory, irq);
	if (ret)
		return ret;

	dev_info(&device->dev, "%s: PCI backend bound\n", VES_QVM_SHM_DRIVER_NAME);
	return 0;
}

static void ves_qvm_shm_pci_remove(struct pci_dev *device)
{
	ves_qvm_shm_hw_unbind(&device->dev);
	dev_info(&device->dev, "%s: PCI backend removed\n", VES_QVM_SHM_DRIVER_NAME);
}

static struct pci_driver ves_qvm_shm_pci_driver = {
	.name	  = VES_QVM_SHM_DRIVER_NAME,
	.id_table = ves_qvm_shm_pci_ids,
	.probe	  = ves_qvm_shm_pci_probe,
	.remove	  = ves_qvm_shm_pci_remove,
};

int ves_qvm_shm_pci_init(void)
{
	return pci_register_driver(&ves_qvm_shm_pci_driver);
}

void ves_qvm_shm_pci_exit(void)
{
	pci_unregister_driver(&ves_qvm_shm_pci_driver);
}
