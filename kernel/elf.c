#include "elf.h"
#include "../fs/vfs.h"
#include "../cpu/sched.h"

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1

#define ELF_MAX_PHDR      8
#define ELF_STACK_PAGES   4
#define ELF_PAGE_SIZE     4096ULL

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

static int validate_ehdr(const elf64_ehdr_t *eh) {
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return -1;
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB)
        return -1;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return -1;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHDR)
        return -1;
    if (eh->e_phentsize != sizeof(elf64_phdr_t))
        return -1;
    return 0;
}

/* Maps [seg_start, seg_end) (page-aligned) into `as`, filling the first
 * `filesz` bytes (starting `content_off` into the first page) from `fd`'s
 * current seek position and leaving the rest zeroed (vmm_alloc_page already
 * zeroes — that's the segment's BSS tail, memsz > filesz). */
static int load_segment(int fd, vmm_as_t *as, uint64_t seg_start, uint64_t seg_end,
                        uint64_t content_off, uint64_t filesz) {
    uint64_t remaining = filesz;

    for (uint64_t va = seg_start; va < seg_end; va += ELF_PAGE_SIZE) {
        uint64_t pa = vmm_alloc_page(as);
        if (!pa) return -1;
        if (vmm_map_page(as, va, pa, VMM_PAGE_RW_U) != 0) return -1;

        uint64_t page_off  = (va == seg_start) ? content_off : 0;
        uint64_t page_room = ELF_PAGE_SIZE - page_off;
        uint64_t to_read   = remaining < page_room ? remaining : page_room;

        if (to_read > 0) {
            int n = vfs_read(fd, (void *)(uintptr_t)(pa + page_off), (size_t)to_read);
            if (n != (int)to_read) return -1;
            remaining -= to_read;
        }
    }
    return 0;
}

int elf_exec(const char *path, const char *name, vmm_as_t **out_as) {
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return -1;

    elf64_ehdr_t ehdr;
    if (vfs_read(fd, &ehdr, sizeof(ehdr)) != (int)sizeof(ehdr) || validate_ehdr(&ehdr) != 0) {
        vfs_close(fd);
        return -1;
    }

    elf64_phdr_t phdrs[ELF_MAX_PHDR];
    if (vfs_seek(fd, ehdr.e_phoff) != 0 ||
        vfs_read(fd, phdrs, (size_t)ehdr.e_phnum * sizeof(elf64_phdr_t)) !=
            (int)(ehdr.e_phnum * sizeof(elf64_phdr_t))) {
        vfs_close(fd);
        return -1;
    }

    vmm_as_t *as = vmm_create_address_space();
    if (!as) { vfs_close(fd); return -1; }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint64_t seg_start = ph->p_vaddr & ~(ELF_PAGE_SIZE - 1);
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + ELF_PAGE_SIZE - 1) & ~(ELF_PAGE_SIZE - 1);

        if (seg_start < VMM_USER_BASE || seg_end > VMM_USER_BASE + VMM_USER_SIZE ||
            ph->p_filesz > ph->p_memsz) {
            vmm_destroy_address_space(as);
            vfs_close(fd);
            return -1;
        }

        if (vfs_seek(fd, ph->p_offset) != 0 ||
            load_segment(fd, as, seg_start, seg_end, ph->p_vaddr - seg_start, ph->p_filesz) != 0) {
            vmm_destroy_address_space(as);
            vfs_close(fd);
            return -1;
        }
    }

    if (ehdr.e_entry < VMM_USER_BASE || ehdr.e_entry >= VMM_USER_BASE + VMM_USER_SIZE) {
        vmm_destroy_address_space(as);
        vfs_close(fd);
        return -1;
    }

    /* Stack: ELF_STACK_PAGES pages just below the window ceiling, with the
     * very top page left unmapped as a guard against RSP wrapping past the
     * window edge. RSP starts at stack_top, a mapped address. */
    uint64_t stack_top = VMM_USER_BASE + VMM_USER_SIZE - ELF_PAGE_SIZE;
    for (int i = 0; i < ELF_STACK_PAGES; i++) {
        uint64_t va = stack_top - (uint64_t)(i + 1) * ELF_PAGE_SIZE;
        uint64_t pa = vmm_alloc_page(as);
        if (!pa || vmm_map_page(as, va, pa, VMM_PAGE_RW_U) != 0) {
            vmm_destroy_address_space(as);
            vfs_close(fd);
            return -1;
        }
    }

    vfs_close(fd);   /* file content is fully loaded into memory now */

    int slot = sched_submit_user_as(name, (void (*)(void *))(uintptr_t)ehdr.e_entry,
                                    (void *)0, as->pml4_phys, stack_top);
    if (slot < 0) {
        vmm_destroy_address_space(as);
        return -1;
    }

    *out_as = as;
    return slot;
}
