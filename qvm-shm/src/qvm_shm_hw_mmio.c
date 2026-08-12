// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "guest_shm.h"
#include "qvm_shm_hw_internal.h"
#include "qvm_shm_types.h"

static unsigned long long ves_qvm_shm_mmio_factory_addr;
module_param_named(mmio_addr, ves_qvm_shm_mmio_factory_addr, ullong, 0444);
MODULE_PARM_DESC(mmio_addr, "Expected MMIO factory base address; disables PCI backend when set");

static const struct of_device_id ves_qvm_shm_mmio_of_match[] = { { .compatible = "qvm,guest_shm" },
								 {} };
MODULE_DEVICE_TABLE(of, ves_qvm_shm_mmio_of_match);

static const struct acpi_device_id ves_qvm_shm_mmio_acpi_match[] = { { "SHM0001" }, {} };
MODULE_DEVICE_TABLE(acpi, ves_qvm_shm_mmio_acpi_match);

static int ves_qvm_shm_mmio_probe(struct platform_device *pdev)
{
	struct device *device = &pdev->dev;
	struct resource *mem;
	struct guest_shm_factory __iomem *factory;
	resource_size_t factory_size;
	int irq;
	int ret;

	irq = platform_get_irq(pdev, 0);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (irq < 0 || !mem) {
		dev_err(device, "failed to get MMIO resources\n");
		return -EINVAL;
	}

	if (ves_qvm_shm_mmio_factory_addr != 0 && mem->start != ves_qvm_shm_mmio_factory_addr) {
		pr_debug("%s: MMIO address mismatch: resource=%pa expected=%#llx\n",
			 VES_QVM_SHM_DRIVER_NAME, &mem->start, ves_qvm_shm_mmio_factory_addr);
		return -ENODEV;
	}

	factory_size = resource_size(mem);
	if (factory_size != PAGE_SIZE) {
		dev_err(device, "MMIO resource size incorrect: %pa size=%pa expected=%#lx\n",
			&mem->start, &factory_size, PAGE_SIZE);
		return -EINVAL;
	}

	factory = devm_ioremap_resource(device, mem);
	if (IS_ERR(factory)) {
		ret = PTR_ERR(factory);
		dev_err(device, "devm_ioremap_resource failed for MMIO resource at %pa: %d\n",
			&mem->start, ret);
		return ret;
	}

	if (ves_qvm_shm_hw_read_factory_signature(factory) != GUEST_SHM_SIGNATURE) {
		dev_err(device, "SHM signature incorrect. %llx != %llx\n",
			(unsigned long long)GUEST_SHM_SIGNATURE,
			(unsigned long long)ves_qvm_shm_hw_read_factory_signature(factory));
		return -ENXIO;
	}

	ret = ves_qvm_shm_hw_bind(device, factory, irq);
	if (ret)
		return ret;

	dev_info(device, "%s: MMIO backend bound\n", VES_QVM_SHM_DRIVER_NAME);
	return 0;
}

static void ves_qvm_shm_mmio_remove(struct platform_device *pdev)
{
	ves_qvm_shm_hw_unbind(&pdev->dev);
	dev_info(&pdev->dev, "%s: MMIO backend removed\n", VES_QVM_SHM_DRIVER_NAME);
}

static struct platform_driver ves_qvm_shm_mmio_driver = {
	.probe = ves_qvm_shm_mmio_probe,
	.remove_new = ves_qvm_shm_mmio_remove,
	.driver = {
		.name = VES_QVM_SHM_DRIVER_NAME,
		.of_match_table = ves_qvm_shm_mmio_of_match,
		.acpi_match_table = ACPI_PTR(ves_qvm_shm_mmio_acpi_match),
	},
};

bool ves_qvm_shm_mmio_addr_filter_enabled(void)
{
	return ves_qvm_shm_mmio_factory_addr != 0;
}

int ves_qvm_shm_mmio_init(void)
{
	return platform_driver_register(&ves_qvm_shm_mmio_driver);
}

void ves_qvm_shm_mmio_exit(void)
{
	platform_driver_unregister(&ves_qvm_shm_mmio_driver);
}
