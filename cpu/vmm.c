#include "vmm.h"
#include "../libc/mem.h"

/* Master identity-map tables from boot/multiboot_entry.asm — PML4@0x1000,
 * PDPT@0x2000. PDPT[0] (0-1GB, kernel) and PDPT[3] (3-4GB, MMIO) are what
 * every process shares; PDPT[1]/[2] are unused by the kernel itself. */
#define MASTER_PDPT ((volatile uint64_t *)0x2000ULL)

#define PTE_ADDR_MASK 0xFFFFFFFFFFFFF000ULL

uint64_t vmm_alloc_page(vmm_as_t *as) {
    if (as->owned_count >= VMM_MAX_PAGES) return 0;

    uintptr_t p = kmalloc(4096, 1, (uintptr_t *)0);
    if (!p) return 0;
    memset((void *)p, 0, 4096);

    as->owned_pages[as->owned_count++] = (uint64_t)p;
    return (uint64_t)p;
}

vmm_as_t *vmm_create_address_space(void) {
    vmm_as_t *as = (vmm_as_t *)kmalloc(sizeof(vmm_as_t), 0, (uintptr_t *)0);
    if (!as) return (vmm_as_t *)0;
    memset(as, 0, sizeof(*as));

    uint64_t pml4 = vmm_alloc_page(as);
    uint64_t pdpt = vmm_alloc_page(as);
    if (!pml4 || !pdpt) {
        vmm_destroy_address_space(as);
        return (vmm_as_t *)0;
    }

    ((uint64_t *)(uintptr_t)pml4)[0] = pdpt | VMM_PAGE_RW_U;

    volatile uint64_t *pdpt_tbl = (volatile uint64_t *)(uintptr_t)pdpt;
    pdpt_tbl[0] = MASTER_PDPT[0];   /* kernel 0-1GB, shared (already U=0) */
    pdpt_tbl[3] = MASTER_PDPT[3];   /* MMIO 3-4GB, shared */
    /* pdpt_tbl[1] (process-private, VMM_USER_BASE) starts absent — filled
     * in lazily by vmm_map_page. */

    as->pml4_phys = pml4;
    return as;
}

int vmm_map_page(vmm_as_t *as, uint64_t va, uint64_t pa, uint64_t flags) {
    if (va < VMM_USER_BASE || va >= VMM_USER_BASE + VMM_USER_SIZE) return -1;
    if (va & 0xFFFULL) return -1;

    uint64_t pdpt_idx = (va >> 30) & 0x1FFULL;   /* always 1 within our window */
    uint64_t pd_idx   = (va >> 21) & 0x1FFULL;
    uint64_t pt_idx   = (va >> 12) & 0x1FFULL;

    volatile uint64_t *pml4_tbl = (volatile uint64_t *)(uintptr_t)as->pml4_phys;
    volatile uint64_t *pdpt_tbl = (volatile uint64_t *)(uintptr_t)(pml4_tbl[0] & PTE_ADDR_MASK);

    if (!(pdpt_tbl[pdpt_idx] & VMM_PAGE_P)) {
        uint64_t pd = vmm_alloc_page(as);
        if (!pd) return -1;
        pdpt_tbl[pdpt_idx] = pd | VMM_PAGE_RW_U;
    }
    volatile uint64_t *pd_tbl = (volatile uint64_t *)(uintptr_t)(pdpt_tbl[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd_tbl[pd_idx] & VMM_PAGE_P)) {
        uint64_t pt = vmm_alloc_page(as);
        if (!pt) return -1;
        pd_tbl[pd_idx] = pt | VMM_PAGE_RW_U;
    }
    volatile uint64_t *pt_tbl = (volatile uint64_t *)(uintptr_t)(pd_tbl[pd_idx] & PTE_ADDR_MASK);

    pt_tbl[pt_idx] = (pa & PTE_ADDR_MASK) | (flags & 0xFFFULL) | VMM_PAGE_P;
    return 0;
}

void vmm_destroy_address_space(vmm_as_t *as) {
    if (!as) return;
    for (int i = 0; i < as->owned_count; i++)
        kfree((void *)(uintptr_t)as->owned_pages[i]);
    kfree(as);
}

int vmm_range_mapped(uint64_t pml4_phys, uint64_t va, size_t len) {
    if (len == 0) return 1;
    if (va < VMM_USER_BASE || va + len > VMM_USER_BASE + VMM_USER_SIZE) return 0;

    uint64_t page      = va & ~0xFFFULL;
    uint64_t last_page = (va + (uint64_t)len - 1) & ~0xFFFULL;

    volatile uint64_t *pml4_tbl = (volatile uint64_t *)(uintptr_t)pml4_phys;
    if (!(pml4_tbl[0] & VMM_PAGE_P)) return 0;
    volatile uint64_t *pdpt_tbl = (volatile uint64_t *)(uintptr_t)(pml4_tbl[0] & PTE_ADDR_MASK);

    for (;; page += 0x1000ULL) {
        uint64_t pdpt_idx = (page >> 30) & 0x1FFULL;   /* always 1 within our window */
        uint64_t pd_idx   = (page >> 21) & 0x1FFULL;
        uint64_t pt_idx   = (page >> 12) & 0x1FFULL;

        if (!(pdpt_tbl[pdpt_idx] & VMM_PAGE_P)) return 0;
        volatile uint64_t *pd_tbl = (volatile uint64_t *)(uintptr_t)(pdpt_tbl[pdpt_idx] & PTE_ADDR_MASK);

        if (!(pd_tbl[pd_idx] & VMM_PAGE_P)) return 0;
        volatile uint64_t *pt_tbl = (volatile uint64_t *)(uintptr_t)(pd_tbl[pd_idx] & PTE_ADDR_MASK);

        uint64_t pte = pt_tbl[pt_idx];
        if ((pte & (VMM_PAGE_P | VMM_PAGE_U)) != (VMM_PAGE_P | VMM_PAGE_U)) return 0;

        if (page == last_page) break;
    }
    return 1;
}
