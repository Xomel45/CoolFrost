#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

/* Per-process address spaces: each exec'd ELF (kernel/elf.c) gets its own
 * PML4 + PDPT, sharing the kernel/MMIO mappings (PDPT[0]/[3], copied by
 * reference from the master tables at 0x1000/0x2000, see
 * boot/multiboot_entry.asm) but with a fully private PD/PT chain for
 * PDPT[1] — the 1GB window [VMM_USER_BASE, VMM_USER_BASE+VMM_USER_SIZE).
 * PML4 entries span 512GB, so *any* address below that shares PML4[0]; a
 * process must not get a private mapping anywhere inside the kernel's own
 * 0-1GB identity map (PDPT[0]) or it would corrupt everyone else's 2MB
 * huge-page entries there. Hence the dedicated, currently-unused PDPT[1]
 * slot (1GB-2GB) instead. */

#define VMM_USER_BASE  0x40000000ULL
#define VMM_USER_SIZE  0x40000000ULL   /* 1GB, exactly PDPT index 1's span */

#define VMM_PAGE_P  0x1ULL
#define VMM_PAGE_W  0x2ULL
#define VMM_PAGE_U  0x4ULL
#define VMM_PAGE_RW_U (VMM_PAGE_P | VMM_PAGE_W | VMM_PAGE_U)

#define VMM_MAX_PAGES 512  /* PML4 + PDPT + PD(s) + PT(s) + data pages, per process — 2MB
                            * ceiling. Was 64 (256KB) until SYS_SBRK (cpu/syscall.c) needed
                            * real headroom for a userland heap on top of code/stack/argv;
                            * the kernel heap backing these (libc/mem.c: KHEAP_SIZE) is 16MiB,
                            * so even several processes at the new cap is a non-issue. */

typedef struct {
    uint64_t pml4_phys;                    /* == CR3 value for this process */
    uint64_t owned_pages[VMM_MAX_PAGES];    /* every page this module allocated, for teardown */
    int      owned_count;
} vmm_as_t;

/* Allocates + zeroes a 4KB page via kmalloc (identity-mapped, so the
 * returned value is both a valid kernel pointer and the physical address
 * to put in a page-table entry) and records it in `as` for later freeing. */
uint64_t vmm_alloc_page(vmm_as_t *as);

/* Builds a fresh PML4+PDPT, copies the master's kernel/MMIO PDPT slots by
 * reference (PDPT[0], PDPT[3]) so kernel code/data/interrupts keep working
 * regardless of which process's CR3 is loaded. PDPT[1] (process-private)
 * starts empty — vmm_map_page() fills it in on demand. Returns NULL on
 * allocation failure. */
vmm_as_t *vmm_create_address_space(void);

/* Maps physical page `pa` at virtual address `va` (must be 4KB-aligned and
 * fall within [VMM_USER_BASE, VMM_USER_BASE+VMM_USER_SIZE)) in `as`,
 * allocating any missing PD/PT levels. `flags` are OR'd onto the leaf PTE
 * (use VMM_PAGE_RW_U for ordinary process memory). Returns 0 on success,
 * -1 if `va` is outside the supported window or allocation failed. */
int vmm_map_page(vmm_as_t *as, uint64_t va, uint64_t pa, uint64_t flags);

/* Frees every page this address space owns (page tables and data pages
 * allocated via vmm_alloc_page), then the vmm_as_t itself. Does NOT touch
 * the shared kernel/MMIO tables (those were only ever referenced, not
 * allocated by this module). Caller must ensure this CR3 isn't loaded. */
void vmm_destroy_address_space(vmm_as_t *as);

/* Read-only walk (no allocation, unlike vmm_map_page): true iff every 4KB
 * page touched by [va, va+len) is actually present (and user-accessible)
 * under the address space whose PML4 physical address is `pml4_phys` —
 * NOT just that `va` falls inside the [VMM_USER_BASE, +VMM_USER_SIZE)
 * window. A syscall pointer can pass a plain bounds check (cpu/paging.c:
 * is_user_range) yet land in that window's unmapped holes (only loaded
 * segments + stack + argv page are ever backed by real pages, see
 * kernel/elf.c) — dereferencing it would #PF while the CPU is in ring0
 * (inside the syscall handler), which cpu/isr.c's fault handler does NOT
 * recover from (it only kills the current task for CS RPL==3 faults; a
 * ring0 fault just re-faults the same instruction forever). This is what
 * lets is_user_range() reject such a pointer before anything touches it,
 * instead of every syscall needing its own fault-recovery path. `len==0`
 * is trivially true (nothing to touch). `va` need not be page-aligned. */
int vmm_range_mapped(uint64_t pml4_phys, uint64_t va, size_t len);

#endif
