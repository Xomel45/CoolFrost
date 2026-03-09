; ============================================================
; Multiboot2 header
; Must be 8-byte aligned and within the first 32KB of the ELF
; ============================================================
section .multiboot2
align 8
mb2_start:
    dd 0xE85250D6                               ; Multiboot2 magic
    dd 0                                         ; Architecture: 0 = 32-bit i386 protected mode
    dd (mb2_end - mb2_start)                    ; Header length
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start)) ; Checksum

    ; End tag (required by Multiboot2 spec)
    align 8
    dw 0    ; tag type = end
    dw 0    ; flags
    dd 8    ; size
mb2_end:

; ============================================================
; Include 64-bit GDT
; ============================================================
section .rodata
%include "boot/gdt.asm"

; ============================================================
; 32-bit bootstrap: set up paging and enter long mode
; GRUB2 multiboot2 drops us here in 32-bit protected mode.
; ============================================================
section .text32
[BITS 32]
DEFAULT ABS         ; 32-bit code uses absolute addressing
global _start
extern kernel_main

_start:
    ; Disable interrupts
    cli

    ; Save multiboot2 registers before we clobber them
    mov [mb_magic_tmp], eax     ; multiboot2 magic (0x36D76289)
    mov [mb_info_tmp],  ebx     ; multiboot2 info structure pointer

    ; ── 1. Identity-map first 1GB using 2MB huge pages ──────────────────
    ;    PML4  at 0x1000 (4KB)
    ;    PDPT  at 0x2000 (4KB)
    ;    PD    at 0x3000 (4KB, 512 entries × 8B each)
    ;    Total zeroed: 0x1000..0x3FFF (3 pages = 12 KB)

    mov edi, 0x1000
    mov cr3, edi                ; set PML4 address now (we'll fill it next)
    xor eax, eax
    mov ecx, 3 * 4096 / 4      ; 3 pages as dwords
    rep stosd                   ; zero all three pages

    ; PML4[0] → PDPT (present + writable)
    mov dword [0x1000], 0x2003

    ; PDPT[0] → PD (present + writable)
    mov dword [0x2000], 0x3003

    ; PD: 512 entries, each maps a 2MB huge page
    mov edi, 0x3000
    mov eax, 0x00000083         ; PS=1 (huge page), W=1, P=1
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    add eax, 0x200000           ; advance by 2MB
    add edi, 8
    loop .fill_pd

    ; ── 2. Enable PAE + SSE (required for long mode / 64-bit GCC) ───────
    mov eax, cr4
    or  eax, (1 << 5)           ; CR4.PAE
    or  eax, (1 << 9)           ; CR4.OSFXSR  — enable SSE
    or  eax, (1 << 10)          ; CR4.OSXMMEXCPT — enable SSE exceptions
    mov cr4, eax

    ; Clear CR0.EM (bit 2), set CR0.MP (bit 1) — required for SSE
    mov eax, cr0
    and eax, ~(1 << 2)          ; clear EM
    or  eax, (1 << 1)           ; set MP
    mov cr0, eax

    ; ── 3. Set EFER.LME (long mode enable) ─────────────────────────────
    mov ecx, 0xC0000080         ; EFER MSR address
    rdmsr
    or  eax, (1 << 8)           ; EFER.LME
    wrmsr

    ; ── 4. Load 64-bit GDT ──────────────────────────────────────────────
    lgdt [gdt64_descriptor]

    ; ── 5. Enable paging → activates long mode (CR0.PG + CR0.PE) ───────
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ; ── 6. Far jump to 64-bit code segment ──────────────────────────────
    jmp CODE_SEG:long_mode_start

.hang:
    hlt
    jmp .hang

; ============================================================
; 64-bit kernel entry
; ============================================================
section .text
[BITS 64]
DEFAULT REL         ; 64-bit code: use RIP-relative addressing by default

long_mode_start:
    ; Reload all data segment registers
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up the stack
    mov rsp, stack_top
    xor rbp, rbp

    ; Pass multiboot2 args to kernel_main (SysV AMD64 ABI: rdi, rsi)
    movzx rdi, dword [mb_magic_tmp]     ; arg1: magic
    movzx rsi, dword [mb_info_tmp]      ; arg2: info pointer

    call kernel_main

.hang:
    hlt
    jmp .hang

; ============================================================
; Stack and temporary multiboot storage
; ============================================================
section .bss
align 16
stack_bottom:
    resb 16384      ; 16 KB kernel stack
stack_top:

section .data
mb_magic_tmp: dd 0
mb_info_tmp:  dd 0
