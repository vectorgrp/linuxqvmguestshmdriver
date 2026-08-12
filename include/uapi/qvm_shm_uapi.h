// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_UAPI_H
#define QVM_SHM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* SHM region name: up to 31 chars + NUL */
#define VES_QVM_SHM_NAME_MAX 32
#define VES_QVM_SHM_MAX_PEERS 16

/*
 * IOCTL magic - unique identifier for this driver.
 * TODO: check against Documentation/userspace-api/ioctl/ioctl-number.rst
 */
#define VES_QVM_SHM_MAGIC 0xEF

/* Special targets for write() notification */
#define VES_QVM_SHM_NOTIFY_ALL ((1u << VES_QVM_SHM_MAX_PEERS) - 1)
#define VES_QVM_SHM_NOTIFY_ALL_BUT_ME (~0u)

/* Parameters for SHM creation ioctls (in/out) */
struct ves_qvm_shm_create_params {
	/* input */
	char shm_name[VES_QVM_SHM_NAME_MAX];
	__u32 requested_pages;
	/* output */
	__u32 mapped_pages;
};

/* Parameters for get client ID ioctl (out) */
struct ves_qvm_shm_get_client_id_params {
	__u32 client_id;
};

/* Passed to write() on a notify SHM FD to trigger a peer notification */
struct ves_qvm_shm_write_notify {
	__u32 target; /* Either bitmask of client ids to be notified [0...15],
			 or VES_QVM_SHM_NOTIFY_ALL, or VES_QVM_SHM_NOTIFY_ALL_BUT_ME */
};

/* Returned by read() on a notify SHM FD */
struct ves_qvm_shm_read_notify {
	__u16 active_ids; /* bitmask of active clients */
	__u16 disconnected_ids; /* bitmask of newly disconnected clients */
	__u16 user_notify_ids; /* bitmask of clients with user notifications */
};

/* Management device ioctls */
#define VES_QVM_SHM_CREATE_NOTIFY_MANY _IOWR(VES_QVM_SHM_MAGIC, 0, struct ves_qvm_shm_create_params)
#define VES_QVM_SHM_CREATE_NOTIFY_TWO _IOWR(VES_QVM_SHM_MAGIC, 1, struct ves_qvm_shm_create_params)
#define VES_QVM_SHM_CREATE_NO_NOTIFY_MANY \
	_IOWR(VES_QVM_SHM_MAGIC, 2, struct ves_qvm_shm_create_params)

/* SHM FD ioctls */
#define VES_QVM_SHM_GET_CLIENT_ID \
	_IOR(VES_QVM_SHM_MAGIC, 3, struct ves_qvm_shm_get_client_id_params)

#endif /* QVM_SHM_UAPI_H */
