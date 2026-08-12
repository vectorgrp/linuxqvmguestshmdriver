// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

/*
 * Module entry/exit registration for non-VectorCAST builds.
 * VectorCAST builds exclude this file and register their own
 * module_init/module_exit in qvm_shm_vcast.c.
 */

#include <linux/module.h>

#include "qvm_shm_module_state.h"

static int __init ves_qvm_shm_init(void)
{
	return ves_qvm_shm_init_core();
}

static void __exit ves_qvm_shm_exit(void)
{
	ves_qvm_shm_exit_core();
}

module_init(ves_qvm_shm_init);
module_exit(ves_qvm_shm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QVM Guest Shared Memory IPC driver");
