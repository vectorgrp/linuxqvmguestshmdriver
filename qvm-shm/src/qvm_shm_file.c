// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/* Per-SHM-file context allocation helpers. */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "qvm_shm_file.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

const struct file_operations *ves_qvm_shm_file_fops(enum ves_qvm_shm_type type)
{
	switch (type) {
	case VES_QVM_SHM_NOTIFY_MANY:
		fallthrough;
	case VES_QVM_SHM_NOTIFY_TWO:
		return &ves_qvm_shm_notify_fops;
	case VES_QVM_SHM_NO_NOTIFY_MANY:
		return &ves_qvm_shm_no_notify_fops;
	default:
		WARN_ON_ONCE(1);
		return NULL;
	}
}

struct ves_qvm_shm_file_ctx *ves_qvm_shm_file_alloc(const char *name, enum ves_qvm_shm_type type,
						    int pages)
{
	struct ves_qvm_shm_file_ctx *fctx;
	int ret;

	fctx = kzalloc(sizeof(*fctx), GFP_KERNEL);
	if (!fctx)
		return NULL;

	ret = strscpy(fctx->shm_region_name, name, VES_QVM_SHM_NAME_MAX);
	if (ret < 0 || ret >= VES_QVM_SHM_NAME_MAX) {
		pr_err("strscpy failed for shm_region_name\n");
		kfree(fctx);
		return NULL;
	}

	fctx->type	      = type;
	fctx->requested_pages = pages;

	return fctx;
}
