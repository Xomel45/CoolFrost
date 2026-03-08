; Defined in isr.c
[extern isr_handler]
[extern irq_handler]

[BITS 64]

; ────────────────────────────────────────────────────────────────────────────
; Common ISR stub
;
; Stack layout on entry (from high to low address):
;   [pushed by CPU]   ss, rsp, rflags, cs, rip
;   [pushed by stub]  err_code, int_no
;
; We push all GPRs, pass RSP (→ registers_t*) as RDI, call isr_handler,
; then restore and iretq.
; ────────────────────────────────────────────────────────────────────────────
isr_common_stub:
    ; Save all general-purpose registers (no pusha in 64-bit mode)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Call C handler: arg1 (rdi) = pointer to registers_t on stack
    mov rdi, rsp
    cld
    call isr_handler

    ; Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; discard int_no and err_code
    iretq           ; pops rip, cs, rflags, rsp, ss

; ────────────────────────────────────────────────────────────────────────────
; Common IRQ stub (same as ISR stub, but calls irq_handler)
; ────────────────────────────────────────────────────────────────────────────
irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov rdi, rsp
    cld
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

; ────────────────────────────────────────────────────────────────────────────
; ISR stubs (one per CPU exception)
; Exceptions that push an error code: 8, 10-14, 17, 21, 28-30
; All others: push a dummy 0 first so the frame is always consistent.
; ────────────────────────────────────────────────────────────────────────────
global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31

; IRQ stubs
global irq0
global irq1
global irq2
global irq3
global irq4
global irq5
global irq6
global irq7
global irq8
global irq9
global irq10
global irq11
global irq12
global irq13
global irq14
global irq15

; 0: Divide By Zero
isr0:
    push 0
    push 0
    jmp isr_common_stub

; 1: Debug
isr1:
    push 0
    push 1
    jmp isr_common_stub

; 2: Non Maskable Interrupt
isr2:
    push 0
    push 2
    jmp isr_common_stub

; 3: Breakpoint
isr3:
    push 0
    push 3
    jmp isr_common_stub

; 4: Overflow
isr4:
    push 0
    push 4
    jmp isr_common_stub

; 5: Out of Bounds
isr5:
    push 0
    push 5
    jmp isr_common_stub

; 6: Invalid Opcode
isr6:
    push 0
    push 6
    jmp isr_common_stub

; 7: Coprocessor Not Available
isr7:
    push 0
    push 7
    jmp isr_common_stub

; 8: Double Fault (CPU pushes error code)
isr8:
    push 8
    jmp isr_common_stub

; 9: Coprocessor Segment Overrun
isr9:
    push 0
    push 9
    jmp isr_common_stub

; 10: Bad TSS (CPU pushes error code)
isr10:
    push 10
    jmp isr_common_stub

; 11: Segment Not Present (CPU pushes error code)
isr11:
    push 11
    jmp isr_common_stub

; 12: Stack Fault (CPU pushes error code)
isr12:
    push 12
    jmp isr_common_stub

; 13: General Protection Fault (CPU pushes error code)
isr13:
    push 13
    jmp isr_common_stub

; 14: Page Fault (CPU pushes error code)
isr14:
    push 14
    jmp isr_common_stub

; 15: Reserved
isr15:
    push 0
    push 15
    jmp isr_common_stub

; 16: Floating Point Exception
isr16:
    push 0
    push 16
    jmp isr_common_stub

; 17: Alignment Check
isr17:
    push 0
    push 17
    jmp isr_common_stub

; 18: Machine Check
isr18:
    push 0
    push 18
    jmp isr_common_stub

; 19-31: Reserved
isr19:
    push 0
    push 19
    jmp isr_common_stub

isr20:
    push 0
    push 20
    jmp isr_common_stub

isr21:
    push 0
    push 21
    jmp isr_common_stub

isr22:
    push 0
    push 22
    jmp isr_common_stub

isr23:
    push 0
    push 23
    jmp isr_common_stub

isr24:
    push 0
    push 24
    jmp isr_common_stub

isr25:
    push 0
    push 25
    jmp isr_common_stub

isr26:
    push 0
    push 26
    jmp isr_common_stub

isr27:
    push 0
    push 27
    jmp isr_common_stub

isr28:
    push 0
    push 28
    jmp isr_common_stub

isr29:
    push 0
    push 29
    jmp isr_common_stub

isr30:
    push 0
    push 30
    jmp isr_common_stub

isr31:
    push 0
    push 31
    jmp isr_common_stub

; ────────────────────────────────────────────────────────────────────────────
; IRQ handlers (remapped to vectors 32-47)
; ────────────────────────────────────────────────────────────────────────────
irq0:
    push 0
    push 32
    jmp irq_common_stub

irq1:
    push 1
    push 33
    jmp irq_common_stub

irq2:
    push 2
    push 34
    jmp irq_common_stub

irq3:
    push 3
    push 35
    jmp irq_common_stub

irq4:
    push 4
    push 36
    jmp irq_common_stub

irq5:
    push 5
    push 37
    jmp irq_common_stub

irq6:
    push 6
    push 38
    jmp irq_common_stub

irq7:
    push 7
    push 39
    jmp irq_common_stub

irq8:
    push 8
    push 40
    jmp irq_common_stub

irq9:
    push 9
    push 41
    jmp irq_common_stub

irq10:
    push 10
    push 42
    jmp irq_common_stub

irq11:
    push 11
    push 43
    jmp irq_common_stub

irq12:
    push 12
    push 44
    jmp irq_common_stub

irq13:
    push 13
    push 45
    jmp irq_common_stub

irq14:
    push 14
    push 46
    jmp irq_common_stub

irq15:
    push 15
    push 47
    jmp irq_common_stub
