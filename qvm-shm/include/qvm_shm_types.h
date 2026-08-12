// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_TYPES_H
#define QVM_SHM_TYPES_H

#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "guest_shm.h"
#include "uapi/qvm_shm_uapi.h"

#define VES_QVM_SHM_DRIVER_NAME "ves_qvm_shm"
#define VES_QVM_SHM_STATUS_NOTIFY_MASK GENMASK(15, 0)
#define VES_QVM_SHM_STATUS_ACTIVE_SHIFT 16

enum ves_qvm_shm_type {
	VES_QVM_SHM_NOTIFY_MANY = 0, /* multi-peer, with IRQ notification */
	VES_QVM_SHM_NOTIFY_TWO, /* two-peer, with IRQ notification */
	VES_QVM_SHM_NO_NOTIFY_MANY, /* multi-peer, no IRQ */
};

struct ves_qvm_shm_region {
	/* Request inputs (written before ves_qvm_shm_request) */
	char name[VES_QVM_SHM_NAME_MAX];
	enum ves_qvm_shm_type type;
	u32 requested_pages;

	/* Backend outputs (populated by ves_qvm_shm_request) */
	phys_addr_t shm_phys;
	struct guest_shm_control __iomem *ctrl_page;
	u32 mapped_pages;
	u8 client_id;

	struct list_head list_entry;

	/*
	 * Reference count covering all active users of this region.
	 *
	 * The unit of counting is one VMA, not one mmap() call, as VMAs can also be create by the
	 * kernel during other systemcalls.
	 *
	 * NO_NOTIFY_MANY:
	 * - Region starts at 1 when installed in ves_qvm_shm_regions.list.
	 * - +1 per additional opener, +1 per VMA (mmap() and .open).
	 * - Decremented by release() and by VMA .close().
	 * - Last put runs under ves_qvm_shm_regions.mutex (sleeping context), then removes list
	 *   entry and frees region.
	 *
	 * NOTIFY types:
	 * - Region starts at 1 when attached to a SHM file.
	 * - +1 per VMA (mmap() and .open), put in release()/VMA .close().
	 * - Region is freed when this reaches 0.
	 */
	atomic_t use_count;
};

/* Per-SHM file context; freed on release() */
struct ves_qvm_shm_file_ctx {
	char shm_region_name[VES_QVM_SHM_NAME_MAX];
	enum ves_qvm_shm_type type;
	u32 requested_pages;

	struct ves_qvm_shm_read_notify notifications;
	spinlock_t notify_lock;

	int irq;
	atomic_t irq_pending;
	wait_queue_head_t wq;

	/* Active SHM region attachment */
	struct ves_qvm_shm_region *region;
};

#endif /* QVM_SHM_TYPES_H */
