; ============================================================
; AP startup trampoline
; Compiled as flat binary: nasm -f bin -o boot/ap_trampoline.bin
;
; SIPI vector 0x08 → AP starts at CS=0x0800 IP=0x0000 → phys 0x8000
; We use [ORG 0x8000] + DS=0 so all label addresses equal physical addresses.
;
; AP mailbox written by BSP at 0x7000:
;   +0x00  uint64_t  ap_main address
;   +0x08  uint64_t  AP stack pointer (RSP)
;   +0x10  uint32_t  BSP CR3 (page table root)
;   +0x14  uint32_t  ap_ready flag  (AP writes 1 here)
;   +0x18  uint32_t  ap_index  (0-based)
; ============================================================

[ORG 0x8000]
DEFAULT ABS
[BITS 16]

ap_tramp16:
    cli
    cld

    ; DS = 0 so [label] == physical address (ORG 0x8000 gives physical offsets)
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7FFC         ; real-mode temp stack below mailbox

    ; Load minimal GDT embedded in this trampoline
    lgdt [ap_gdtr]

    ; Enter protected mode
    mov  eax, cr0
    or   al, 1
    mov  cr0, eax

    ; Far jump to 32-bit code: flush pipeline, load CS=0x08 (code32)
    ; Operand-size prefix (0x66) makes this a 32-bit far jump while in 16-bit mode
    db   0x66, 0xEA
    dd   ap_pm32            ; 32-bit EIP (= 0x8000 + offset, physical)
    dw   0x08               ; code32 selector

[BITS 32]
align 4
ap_pm32:
    mov  ax, 0x10           ; data32 selector
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  fs, ax
    mov  gs, ax

    ; Enable PAE (required for long mode)
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax

    ; Load BSP page tables from mailbox
    mov  eax, [0x7010]      ; ap_cr3
    mov  cr3, eax

    ; Set EFER.LME
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, (1 << 8)
    wrmsr

    ; Enable paging → activates long mode
    mov  eax, cr0
    or   eax, (1 << 31)
    mov  cr0, eax

    ; Far jump to 64-bit code: CS=0x18 (code64 in trampoline GDT)
    jmp  0x18:ap_lm64

[BITS 64]
align 4
ap_lm64:
    ; Null out data segments (flat model, DS/ES/SS=0 is fine in 64-bit)
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  fs, ax
    mov  gs, ax

    ; Load RSP, ap_main address, and ap_index from mailbox BEFORE signaling ready
    ; (BSP may overwrite mailbox once we signal)
    mov  rsp, [0x7008]      ; ap_rsp
    mov  rax, [0x7000]      ; ap_main address
    xor  edi, edi
    mov  edi, [0x7018]      ; ap_index → RDI (first arg, SysV ABI)

    ; Signal BSP: we are alive and in 64-bit mode
    mov  dword [0x7014], 1

    ; Call ap_main(ap_index)
    call rax

    ; Should never return
.hang:
    cli
    hlt
    jmp  .hang

; ── Minimal GDT (embedded in trampoline, at known physical address) ──────────
align 8
ap_gdt:
    dq 0                         ; [0x00] null
    dq 0x00CF9A000000FFFF        ; [0x08] code32 (G=1,D=1,P=1,DPL=0,exec/read)
    dq 0x00CF92000000FFFF        ; [0x10] data32 (G=1,B=1,P=1,DPL=0,rw)
    dq 0x00AF9A000000FFFF        ; [0x18] code64 (G=1,L=1,P=1,DPL=0,exec/read)
ap_gdt_end:

align 4
ap_gdtr:
    dw ap_gdt_end - ap_gdt - 1  ; GDT limit
    dd ap_gdt                    ; GDT base (physical address, ORG 0x8000)
