// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_FOPS_NOTIFY_H
#define QVM_SHM_FOPS_NOTIFY_H

struct ves_qvm_shm_file_ctx;
struct ves_qvm_shm_region;

int ves_qvm_shm_notify_attach(struct ves_qvm_shm_file_ctx *fctx);
void ves_qvm_shm_notify_region_put(struct ves_qvm_shm_region *region);

#endif /* QVM_SHM_FOPS_NOTIFY_H */
