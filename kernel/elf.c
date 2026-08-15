#include "elf.h"
#include "../fs/vfs.h"
#include "../cpu/sched.h"
#include "../libc/mem.h"
#include "../libc/string.h"

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

/* Process-side view of the argv page elf_exec() builds — argc plus pointers
 * into the strings packed right after this header, all addresses already
 * translated to the process's own VA (argv_va below), so the process can
 * dereference them directly. Must match userland/usyscall.h's proc_args_t
 * byte-for-byte (see kernel/elf.h's elf_exec doc comment). */
typedef struct {
    uint64_t argc;
    char    *argv[ELF_MAX_ARGS];
} proc_args_t;

int elf_exec(const char *path, const char *name, int argc, char *const argv[],
            vmm_as_t **out_as) {
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

    /* Highest byte any PT_LOAD segment reaches, page-aligned up — where
     * SYS_SBRK's heap (cpu/syscall.c) starts growing from, set on the task
     * once it exists (sched_submit_user_as, below). Starts at
     * VMM_USER_BASE so a (pathological) ELF with no PT_LOAD segments still
     * gets a well-defined, if useless, heap_start rather than garbage. */
    uint64_t max_seg_end = VMM_USER_BASE;

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

        if (seg_end > max_seg_end) max_seg_end = seg_end;
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

    /* Argv page: one more page directly below the stack, holding the
     * proc_args_t header (argc + ELF_MAX_ARGS char* slots) followed by the
     * NUL-terminated argument strings themselves, all packed into a single
     * 4KB page. Built through vmm_alloc_page's identity-mapped kernel-VA
     * alias (argv_pa doubles as a plain kernel pointer we can memcpy into),
     * but every pointer actually stored in the header is the process-side
     * VA (argv_va + offset) — the process can only ever see its own
     * mapping, never the kernel alias. */
    uint64_t argv_va = stack_top - (uint64_t)(ELF_STACK_PAGES + 1) * ELF_PAGE_SIZE;
    uint64_t argv_pa = vmm_alloc_page(as);
    if (!argv_pa || vmm_map_page(as, argv_va, argv_pa, VMM_PAGE_RW_U) != 0) {
        vmm_destroy_address_space(as);
        return -1;
    }

    if (argc < 0) argc = 0;
    if (argc > ELF_MAX_ARGS) argc = ELF_MAX_ARGS;

    proc_args_t *hdr    = (proc_args_t *)(uintptr_t)argv_pa;
    char        *str_kva = (char *)(uintptr_t)argv_pa + sizeof(proc_args_t);
    uint64_t     str_room = ELF_PAGE_SIZE - sizeof(proc_args_t);
    uint64_t     str_off  = 0;

    int copied = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (str_off + len + 1 > str_room) break;   /* out of room in this page — stop here */
        memcpy(str_kva + str_off, argv[i], len + 1);
        hdr->argv[copied++] = (char *)(uintptr_t)(argv_va + sizeof(proc_args_t) + str_off);
        str_off += (uint64_t)len + 1;
    }
    hdr->argc = (uint64_t)copied;
    for (int i = copied; i < ELF_MAX_ARGS; i++) hdr->argv[i] = (char *)0;

    int slot = sched_submit_user_as(name, (void (*)(void *))(uintptr_t)ehdr.e_entry,
                                    (void *)(uintptr_t)argv_va, as->pml4_phys, stack_top);
    if (slot < 0) {
        vmm_destroy_address_space(as);
        return -1;
    }

    /* Heap: [max_seg_end, argv_va) is unmapped and free for SYS_SBRK to
     * grow into (cpu/syscall.c) — argv_va is the ceiling since the argv
     * page and stack sit at fixed addresses just above it, near the top of
     * the window. as itself is stashed on the task too: SYS_SBRK needs the
     * vmm_as_t* (not just .cr3) to call vmm_alloc_page/vmm_map_page. */
    task_t *t     = sched_get_task(slot);
    t->as         = as;
    t->heap_start = max_seg_end;
    t->heap_brk   = max_seg_end;
    t->heap_end   = argv_va;

    *out_as = as;
    return slot;
}
