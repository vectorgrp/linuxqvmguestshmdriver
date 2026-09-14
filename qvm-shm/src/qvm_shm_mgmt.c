// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * Management device file operations and ioctl handling.
 */

#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "qvm_shm_file.h"
#include "qvm_shm_fops_common.h"
#include "qvm_shm_fops_nonotify.h"
#include "qvm_shm_fops_notify.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

static int ves_qvm_shm_mgmt_open(struct inode *inode, struct file *file)
{
	pr_debug("ves_qvm_shm_mgmt_open\n");
	return 0;
}

static int ves_qvm_shm_mgmt_release(struct inode *inode, struct file *file)
{
	pr_debug("ves_qvm_shm_mgmt_release\n");
	return 0;
}

static long ves_qvm_shm_mgmt_create(enum ves_qvm_shm_type type,
				    struct ves_qvm_shm_create_params __user *uparam)
{
	struct ves_qvm_shm_create_params params;
	const struct file_operations *fops;
	struct ves_qvm_shm_file_ctx *fctx;
	int fd;
	int ret;

	if (copy_from_user(&params, uparam, sizeof(params)))
		return -EFAULT;

	params.shm_name[VES_QVM_SHM_NAME_MAX - 1] = '\0';
	if (params.shm_name[0] == '\0' || params.requested_pages == 0)
		return -EINVAL;

	pr_debug("ves_qvm_shm_mgmt_create - type: %d, name: %s, size: %u\n", type, params.shm_name,
		 params.requested_pages);

	fctx = ves_qvm_shm_file_alloc(params.shm_name, type, params.requested_pages);
	if (!fctx)
		return -ENOMEM;

	fops = ves_qvm_shm_file_fops(type);
	if (!fops) {
		ret = -EINVAL;
		goto free_shm_file;
	}

	if (type == VES_QVM_SHM_NO_NOTIFY_MANY)
		ret = ves_qvm_shm_no_notify_attach(fctx);
	else
		ret = ves_qvm_shm_notify_attach(fctx);

	if (ret)
		goto free_shm_file;

	params.mapped_pages = fctx->region->mapped_pages;

	ret = copy_to_user(uparam, &params, sizeof(params));
	if (ret) {
		ret = -EFAULT;
		goto detach_shm_file;
	}

	fd = anon_inode_getfd("[ves_shmfd]", fops, fctx, O_RDWR);
	if (fd < 0) {
		ret = fd;
		goto detach_shm_file;
	}

	return fd;

detach_shm_file:
	ves_qvm_shm_common_region_put(fctx->region);

free_shm_file:
	kfree(fctx);
	return ret;
}

static long ves_qvm_shm_mgmt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	pr_debug("ves_qvm_shm_mgmt_ioctl - cmd: %#x\n", cmd);

	switch (cmd) {
	case VES_QVM_SHM_CREATE_NOTIFY_MANY:
		return ves_qvm_shm_mgmt_create(VES_QVM_SHM_NOTIFY_MANY, uarg);
	case VES_QVM_SHM_CREATE_NOTIFY_TWO:
		return ves_qvm_shm_mgmt_create(VES_QVM_SHM_NOTIFY_TWO, uarg);
	case VES_QVM_SHM_CREATE_NO_NOTIFY_MANY:
		return ves_qvm_shm_mgmt_create(VES_QVM_SHM_NO_NOTIFY_MANY, uarg);
	default:
		return -ENOTTY;
	}
}

const struct file_operations ves_qvm_shm_mgmt_fops = {
	.owner		= THIS_MODULE,
	.open		= ves_qvm_shm_mgmt_open,
	.release	= ves_qvm_shm_mgmt_release,
	.unlocked_ioctl = ves_qvm_shm_mgmt_ioctl,
};
