// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * Shared file-operation helpers used by multiple SHM types.
 */

#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/uaccess.h>

#include "qvm_shm_fops_common.h"
#include "qvm_shm_fops_nonotify.h"
#include "qvm_shm_fops_notify.h"
#include "qvm_shm_types.h"

#undef pr_fmt
#define pr_fmt(fmt) VES_QVM_SHM_DRIVER_NAME ": " fmt

static void ves_qvm_shm_common_region_put(struct ves_qvm_shm_region *region)
{
	if (region->type == VES_QVM_SHM_NO_NOTIFY_MANY)
		ves_qvm_shm_no_notify_region_put(region);
	else
		ves_qvm_shm_notify_region_put(region);
}

/*
 * VMA open hook: Called whenever the kernel creates an additional VMA referring
 * to this region without going through mmap() again - dup_mmap() on fork(),
 * __split_vma() on partial munmap/mprotect, copy_vma() on mremap.
 *
 * Every one of those VMAs gets its own .close, so every one of them needs its
 * own reference. Without this hook use_count would be balanced per mmap() call
 * rather than per VMA and would underflow, freeing the region while a file or
 * another mapping is still using it.
 */
static void ves_qvm_shm_common_vma_open(struct vm_area_struct *vma)
{
	struct ves_qvm_shm_region *const region = vma->vm_private_data;

	pr_debug("ves_qvm_shm_common_vma_open - VMA duplicated for region %s\n", region->name);

	atomic_inc(&region->use_count);
}

/*
 * VMA close hook: Called when mmap'd memory region is destroyed (munmap,
 * process exit, etc).
 *
 * vm_private_data points to ves_qvm_shm_region (not ves_qvm_shm_file_ctx) so this
 * hook remains safe after release() has destroyed and freed the file context.
 *
 * Decrements use_count. For NO_NOTIFY_MANY this goes through
 * ves_qvm_shm_no_notify_region_put() which also handles list removal under mutex. For
 * NOTIFY types the region is freed directly when use_count reaches 0.
 */
static void ves_qvm_shm_common_vma_close(struct vm_area_struct *vma)
{
	struct ves_qvm_shm_region *const region = vma->vm_private_data;

	pr_debug("ves_qvm_shm_common_vma_close - VMA destroyed for region %s\n", region->name);

	ves_qvm_shm_common_region_put(region);
}

static const struct vm_operations_struct ves_qvm_shm_vm_ops = {
	.open  = ves_qvm_shm_common_vma_open,
	.close = ves_qvm_shm_common_vma_close,
};

long ves_qvm_shm_common_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ves_qvm_shm_file_ctx *const fctx	    = file->private_data;
	void __user *uarg			    = (void __user *)arg;
	struct ves_qvm_shm_get_client_id_params out = { 0 };

	pr_debug("%s - ves_qvm_shm_common_ioctl - cmd: %#x\n", fctx->shm_region_name, cmd);

	switch (cmd) {
	case VES_QVM_SHM_GET_CLIENT_ID:
		out.client_id = (__u32)fctx->region->client_id;
		if (copy_to_user(uarg, &out, sizeof(out))) {
			return -EFAULT;
		}
		return 0;
	default:
		return -ENOTTY;
	}
}

int ves_qvm_shm_common_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ves_qvm_shm_file_ctx *const fctx = file->private_data;
	struct ves_qvm_shm_region *const region = fctx->region;
	unsigned long mapped_size;
	unsigned long offset;
	unsigned long pfn;
	const unsigned long req_size = vma->vm_end - vma->vm_start;
	int ret;

	if (WARN_ON_ONCE(!region))
		return -ENXIO;
	if (check_shl_overflow((unsigned long)region->mapped_pages, PAGE_SHIFT, &mapped_size))
		return -EINVAL;
	if (check_shl_overflow(vma->vm_pgoff, PAGE_SHIFT, &offset))
		return -EINVAL;
	if (req_size + offset > mapped_size)
		return -EINVAL;
	if (check_add_overflow((region->shm_phys >> PAGE_SHIFT), vma->vm_pgoff, &pfn))
		return -EINVAL;

	pr_debug("%s - ves_qvm_shm_common_mmap - offset: %lu, req_size: %lu\n",
		 fctx->shm_region_name, offset, req_size);

	atomic_inc(&region->use_count);

	ret = remap_pfn_range(vma, vma->vm_start, pfn, req_size, vma->vm_page_prot);

	if (ret) {
		ves_qvm_shm_common_region_put(region);
		return ret;
	}

	vma->vm_ops	     = &ves_qvm_shm_vm_ops;
	vma->vm_private_data = region;

	return 0;
}
