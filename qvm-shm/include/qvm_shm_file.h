// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_FILE_H
#define QVM_SHM_FILE_H

#include "qvm_shm_types.h"

struct file_operations;

extern const struct file_operations ves_qvm_shm_notify_fops;
extern const struct file_operations ves_qvm_shm_no_notify_fops;

struct ves_qvm_shm_file_ctx *ves_qvm_shm_file_alloc(const char *name, enum ves_qvm_shm_type type,
						    int pages);

const struct file_operations *ves_qvm_shm_file_fops(enum ves_qvm_shm_type type);

#endif /* QVM_SHM_FILE_H */
