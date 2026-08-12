// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * Module entry/exit and management chardev setup.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>

#include "qvm_shm_compat.h"
#include "qvm_shm_hw.h"
#include "qvm_shm_module_state.h"
#include "qvm_shm_types.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

extern const struct file_operations ves_qvm_shm_mgmt_fops;

static struct class *ves_qvm_shm_class;

static dev_t ves_qvm_shm_mgmt_devno;
static struct cdev ves_qvm_shm_mgmt_cdev;
static struct device *ves_qvm_shm_mgmt_dev;

int __init ves_qvm_shm_init_core(void)
{
	int ret;

	pr_info("waiting for PCI/MMIO backend to bind; requests will fail with ENODEV until then\n");

	ret = ves_qvm_shm_hw_init();
	if (ret) {
		pr_err("ves_qvm_shm_hw_init failed: %d\n", ret);
		goto out;
	}

	ret = alloc_chrdev_region(&ves_qvm_shm_mgmt_devno, 0, 1, VES_QVM_SHM_DRIVER_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed\n");
		goto hw_exit;
	}

	cdev_init(&ves_qvm_shm_mgmt_cdev, &ves_qvm_shm_mgmt_fops);
	ves_qvm_shm_mgmt_cdev.owner = THIS_MODULE;
	ret			    = cdev_add(&ves_qvm_shm_mgmt_cdev, ves_qvm_shm_mgmt_devno, 1);
	if (ret) {
		pr_err("cdev_add failed\n");
		goto free_chrdev;
	}
	ves_qvm_shm_class = class_create(THIS_MODULE, VES_QVM_SHM_DRIVER_NAME);
	if (IS_ERR(ves_qvm_shm_class)) {
		ret = PTR_ERR(ves_qvm_shm_class);
		pr_err("class_create failed\n");
		goto del_cdev;
	}

	ves_qvm_shm_mgmt_dev = device_create(ves_qvm_shm_class, NULL, ves_qvm_shm_mgmt_devno, NULL,
					     "%s", VES_QVM_SHM_DRIVER_NAME);
	if (IS_ERR(ves_qvm_shm_mgmt_dev)) {
		ret = PTR_ERR(ves_qvm_shm_mgmt_dev);
		pr_err("device_create failed for mgmt\n");
		goto destroy_class;
	}

	dev_info(ves_qvm_shm_mgmt_dev, "loaded, management device: /dev/%s\n",
		 VES_QVM_SHM_DRIVER_NAME);

	return 0;

destroy_class:
	class_destroy(ves_qvm_shm_class);
del_cdev:
	cdev_del(&ves_qvm_shm_mgmt_cdev);
free_chrdev:
	unregister_chrdev_region(ves_qvm_shm_mgmt_devno, 1);
hw_exit:
	ves_qvm_shm_hw_exit();
out:
	return ret;
}

void __exit ves_qvm_shm_exit_core(void)
{
	device_destroy(ves_qvm_shm_class, ves_qvm_shm_mgmt_devno);
	class_destroy(ves_qvm_shm_class);
	cdev_del(&ves_qvm_shm_mgmt_cdev);
	unregister_chrdev_region(ves_qvm_shm_mgmt_devno, 1);
	ves_qvm_shm_hw_exit();
}
