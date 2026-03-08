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
}
