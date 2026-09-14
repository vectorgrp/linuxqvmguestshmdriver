// SPDX-FileCopyrightText: 2026 Vector Informatik GmbH
//
// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note

#ifndef QVM_SHM_HW_H
#define QVM_SHM_HW_H

#include <linux/types.h>

struct guest_shm_control;
struct ves_qvm_shm_read_notify;
struct ves_qvm_shm_region;

int ves_qvm_shm_hw_init(void);
void ves_qvm_shm_hw_exit(void);

/*
 * Request and attach one SHM region.
 *
 * Region must be a valid, writable object with name/type/requested_pages initialized by the caller.
 * On success the backend outputs are filled in.
 *
 * From this call onwards the region is notifiable: peers can notify it and the
 * backend asserts the shared interrupt line for it.
 * The caller must already have a handler registered before this function is called to keep the
 * window where unhandled IRQs are coming as small as possible.
 */
int ves_qvm_shm_request(struct ves_qvm_shm_region *region);

/*
 * Sets the interrupt line the bound backend uses.
 * Allows request_irq() before ves_qvm_shm_request() is called.
 */
int ves_qvm_shm_get_irq(int *irq_out);

/*
 * Detach an attached region from the backend, without unmapping ctrl_page yet.
 *
 * Must be called before free_irq(). Order matters:
 *   1. region->ctrl_page is published as NULL first (release store), so any
 *      handler invocation that has not yet loaded it bails out immediately
 *      instead of touching hardware. A handler already mid-flight keeps
 *      using the pointer it already captured, which is still safe because
 *      synchronize_irq() is called before writing detach register.
 *   2. Only then is the detach register written (through the pointer this
 *      call returns, not region->ctrl_page, which is already NULL). This is
 *      what makes the backend stop asserting the shared interrupt line on
 *      this region's behalf.
 *
 * If detach were written first, a handler invocation racing the gap between
 * the two steps could still read the status register after the backend has
 * treated the slot as freed - undefined at best, and touching a slot already
 * recycled by another client at worst. Publishing NULL first closes that
 * gap: nothing reads the status register once detach has been issued.
 *
 * Returns the region's control page mapping (may be NULL if the region was
 * never attached), to be passed to ves_qvm_shm_unmap() after free_irq()
 * returns and no handler invocation can still be using it. Callers keep
 * ownership of the region object. NULL-tolerant for region->ctrl_page.
 */
struct guest_shm_control __iomem *ves_qvm_shm_detach(struct ves_qvm_shm_region *region);

/*
 * Unmap a control page previously returned by ves_qvm_shm_detach().
 *
 * Must be called after free_irq() returns, once no handler invocation can
 * still be using the pointer. NULL-tolerant.
 */
void ves_qvm_shm_unmap(struct guest_shm_control __iomem *ctrl_page);

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
 * Reading the status register is destructive: the backend clears the region's
 * pending notification, connect and disconnect flags and drops the matching
 * interrupt assertions. Each region's status must therefore be read exactly
 * once per interrupt, by whoever owns that region's IRQ handler.
 *
 * The attach/release contract should guarantee region->ctrl_page validity. The
 * implementation WARN_ON_ONCEs and returns 0 if violated.
 */
u32 ves_qvm_shm_read_status_register(struct ves_qvm_shm_region *region);

#endif /* QVM_SHM_HW_H */
