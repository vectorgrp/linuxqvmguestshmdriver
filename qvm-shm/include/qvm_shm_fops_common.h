// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_FOPS_COMMON_H
#define QVM_SHM_FOPS_COMMON_H

struct file;
struct vm_area_struct;

struct ves_qvm_shm_region;

long ves_qvm_shm_common_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int ves_qvm_shm_common_mmap(struct file *file, struct vm_area_struct *vma);
void ves_qvm_shm_common_region_put(struct ves_qvm_shm_region *region);

#endif /* QVM_SHM_FOPS_COMMON_H */
