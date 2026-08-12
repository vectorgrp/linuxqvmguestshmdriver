// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_HW_H
#define QVM_SHM_HW_H

#include <linux/types.h>

struct ves_qvm_shm_read_notify;
struct ves_qvm_shm_file_ctx;
struct ves_qvm_shm_region;

int ves_qvm_shm_hw_init(void);
void ves_qvm_shm_hw_exit(void);

/*
 * Request and attach one SHM region.
 *
 * region must be a valid, writable object with name/type/requested_pages
 * initialized by the caller.
 */
int ves_qvm_shm_request(struct ves_qvm_shm_region *region, int *irq_out);

/*
 * Get the backend IRQ number. Returns 0 and writes the IRQ to irq_out on
 * success.
 */
int ves_qvm_shm_get_irq(int *irq_out);

/*
 * Release backend resources of an attached region.
 *
 * Callers keep ownership of the region object. This helper only tears down
 * backend state (for example ctrl_page mapping) and is NULL-tolerant for
 * region->ctrl_page.
 */
void ves_qvm_shm_release(struct ves_qvm_shm_region *region);

/*
 * Backend boundary helper used by write() paths.
 *
 * The open/release contract should guarantee region and ctrl_page validity.
 * The implementation still WARN_ON_ONCEs and returns on violations so driver
 * bugs are visible without dereferencing invalid pointers.
 */
void ves_qvm_shm_send_notification(struct ves_qvm_shm_region *region, u32 target);

/*
 * Backend boundary helper used by attach()/IRQ paths.
 *
 * The open/release contract should guarantee shm->region->ctrl_page validity.
 * The implementation WARN_ON_ONCEs and returns 0 if violated.
 */
u32 ves_qvm_shm_read_status_register(struct ves_qvm_shm_file_ctx *shm);

#endif /* QVM_SHM_HW_H */
