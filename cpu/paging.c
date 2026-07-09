#include "paging.h"
#include "vmm.h"

/* boot/multiboot_entry.asm's identity map for 0-1GB: PML4[0] -> PDPT@0x2000,
 * PDPT[0] -> PD@0x3000, PD = 512 x 2MB huge pages. */
#define PML4_BASE  0x1000ULL
#define PDPT_BASE  0x2000ULL
#define PD0_BASE   0x3000ULL
#define PDE_COUNT  512
#define PDE_SIZE   0x200000ULL   /* 2MB per huge-page PDE */
#define ENTRY_USER 0x4ULL        /* U/S bit */

void paging_mark_user_region(void) {
    uint64_t start = (uint64_t)(uintptr_t)__user_start;
    uint64_t end   = (uint64_t)(uintptr_t)__user_end;

    uint64_t first_idx = start / PDE_SIZE;
    uint64_t last_idx  = (end - 1) / PDE_SIZE;   /* end is exclusive, already 2MB-aligned */

    /* The CPU ANDs the U/S bit across every level of the walk — setting it
     * only on the leaf PDE isn't enough, PML4[0]/PDPT[0] (0x83/0x83, U=0)
     * would still deny ring3 outright. Both cover the entire identity-mapped
     * 0-1GB region, so marking them U=1 doesn't loosen anything by itself:
     * per-2MB access is still gated by each individual PDE below, and only
     * the ones in [__user_start, __user_end) get that bit set. */
    volatile uint64_t *pml4 = (volatile uint64_t *)PML4_BASE;
    volatile uint64_t *pdpt = (volatile uint64_t *)PDPT_BASE;
    volatile uint64_t *pd   = (volatile uint64_t *)PD0_BASE;

    pml4[0] |= ENTRY_USER;
    pdpt[0] |= ENTRY_USER;

    for (uint64_t idx = first_idx; idx <= last_idx && idx < PDE_COUNT; idx++)
        pd[idx] |= ENTRY_USER;
}

/* A pointer is "user" if it falls in either of two disjoint ranges:
 *  - the shared arena (.user_text/.user_bss, WM/smoke — see linker.ld),
 *    valid under every address space since it's mapped identically in all
 *    of them (cpu/vmm.c copies PDPT[0] by reference into every process);
 *  - a private process's VMM_USER_BASE window (cpu/vmm.h). This is safe to
 *    check unconditionally, without knowing which task is making the
 *    syscall: a trap doesn't change CR3, so whatever's mapped there right
 *    now belongs to whichever task's syscall we're currently handling. */
static int in_range(uintptr_t p, size_t len, uintptr_t start, uintptr_t end) {
    if (len == 0) return p >= start && p < end;
    if (p < start || p >= end) return 0;
    if (len > (uintptr_t)(end - p)) return 0;   /* also catches p+len overflow */
    return 1;
}

int is_user_range(const void *ptr, size_t len) {
    uintptr_t p = (uintptr_t)ptr;

    if (in_range(p, len, (uintptr_t)__user_start, (uintptr_t)__user_end))
        return 1;
    if (in_range(p, len, (uintptr_t)VMM_USER_BASE, (uintptr_t)(VMM_USER_BASE + VMM_USER_SIZE)))
        return 1;

    return 0;
}
