[BITS 64]
global check_cpuid

; int check_cpuid(void);
; Returns: rax != 0 if CPUID is supported, 0 otherwise.
; Checks whether the CPUID ID bit (bit 21) in RFLAGS can be toggled.
check_cpuid:
    pushfq                          ; save original RFLAGS
    pushfq                          ; copy to stack for modification
    xor qword [rsp], 0x00200000     ; flip ID bit in the copy
    popfq                           ; load modified RFLAGS
    pushfq                          ; push the result back
    pop rax                         ; rax = actual RFLAGS after modification
    xor rax, [rsp]                  ; bits that actually changed
    popfq                           ; restore original RFLAGS
    and eax, 0x00200000             ; mask only ID bit (result fits in 32 bits)
    ret
