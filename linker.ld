ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x100000;

    /* Multiboot2 header MUST come first (within first 32KB) */
    .multiboot2 ALIGN(8) : {
        *(.multiboot2)
    }

    /* 32-bit bootstrap code (long mode transition) */
    .text32 ALIGN(4K) : {
        *(.text32)
    }

    /* 64-bit kernel code */
    .text ALIGN(4K) : {
        *(.text)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }

    /* Ring3 arena: everything a user task can execute/touch. 2MB-aligned on
     * both ends so paging_mark_user_region() can grant access by OR-ing the
     * U bit into whole PDEs of the existing 2MB-huge-page identity map
     * (cpu/paging.c) instead of splitting it into 4KB page tables. */
    . = ALIGN(0x200000);
    __user_start = .;

    .user_text ALIGN(4K) : {
        *(.user_text)
    }

    .user_data ALIGN(4K) : {
        *(.user_data)
    }

    .user_bss ALIGN(4K) : {
        *(.user_bss)
    }

    . = ALIGN(0x200000);
    __user_end = .;
}
