# SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
#
# SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

CONFIG_MODULE_SIG=n
obj-m += qvm_shm.o

ccflags-y += -I$(src)/include/

qvm_shm-y := \
		src/qvm_shm_core.o \
		src/qvm_shm_file.o \
		src/qvm_shm_fops_common.o \
		src/qvm_shm_fops_nonotify.o \
		src/qvm_shm_fops_notify.o \
		src/qvm_shm_hw_common.o \
		src/qvm_shm_hw_mmio.o \
		src/qvm_shm_hw_pci.o \
		src/qvm_shm_mgmt.o \
		src/qvm_shm_module.o
