#ifndef PAGING_H
#define PAGING_H

#include <stddef.h>
#include <stdint.h>

/* Linker-provided bounds of the ring3 arena (.user_text/.user_data/.user_bss),
 * 2MB-aligned on both ends — see linker.ld. */
extern char __user_start[];
extern char __user_end[];

/* OR's the U bit into every 2MB PDE (in the existing identity-mapped
 * PD@0x3000, see boot/multiboot_entry.asm) that covers [__user_start,
 * __user_end). Must run once, after paging is active (always true in long
 * mode) and before the first ring3 task is submitted. Kernel-only memory
 * outside that range is untouched and stays supervisor-only (U=0). */
void paging_mark_user_region(void);

/* Bounds check for pointers a ring3 task passes into a syscall: true iff
 * [ptr, ptr+len) lies entirely within the user arena. Reject (don't deref)
 * anything that fails this — the whole point of the syscall boundary is
 * that ring3 can't hand the kernel a kernel-space pointer to write through. */
int is_user_range(const void *ptr, size_t len);

#endif
