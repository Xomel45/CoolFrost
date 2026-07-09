/* Companion linker script for userland/*.c — pins the load address inside
 * the process window kernel/elf.c and cpu/vmm.h agree on (VMM_USER_BASE +
 * one page, leaving the first page of the window free in case a future
 * loader wants it for something like an auxv/argv block). _start is called
 * directly as void _start(void *arg) by user_task_trampoline (cpu/sched.c)
 * — no crt0 stub needed, arg arrives in rdi per the normal SysV ABI. */
ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x40001000;

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
