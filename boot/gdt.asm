align 16
gdt64_start:
    dq 0                ; Null descriptor

; Code segment: L=1 (64-bit), G=1, D=0, P=1, DPL=0, S=1, type=1010 (exec/read)
gdt64_code:
    dw 0xFFFF           ; Segment limit, bits 0-15
    dw 0x0000           ; Segment base, bits 0-15
    db 0x00             ; Segment base, bits 16-23
    db 10011010b        ; Flags: P=1, DPL=00, S=1, type=1010
    db 10101111b        ; Flags: G=1, D=0, L=1, AVL=0 | limit bits 16-19 = 0xF
    db 0x00             ; Segment base, bits 24-31

; Data segment: G=1, B=1 (or 0 - irrelevant in 64-bit), P=1, DPL=0, S=1, type=0010 (read/write)
gdt64_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b        ; Flags: P=1, DPL=00, S=1, type=0010
    db 11001111b        ; Flags: G=1, D=1, L=0 | limit bits 16-19 = 0xF
    db 0x00

gdt64_end:

; GDT descriptor: dq base so LGDT works in both 32-bit (reads 6 bytes) and 64-bit (reads 10 bytes)
align 4
gdt64_descriptor:
    dw gdt64_end - gdt64_start - 1     ; Limit (always one less)
    dq gdt64_start                      ; 64-bit base address

CODE_SEG equ gdt64_code - gdt64_start  ; = 0x08
DATA_SEG equ gdt64_data - gdt64_start  ; = 0x10
