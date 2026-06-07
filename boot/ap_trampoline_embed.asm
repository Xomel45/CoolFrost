; Embeds the flat-binary trampoline into the kernel ELF as read-only data.
; Build order: ap_trampoline.bin must exist before this file is assembled.

[BITS 64]
section .rodata

global ap_trampoline_blob, ap_trampoline_size

ap_trampoline_blob:
    incbin "boot/ap_trampoline.bin"
ap_trampoline_blob_end:

ap_trampoline_size:
    dq ap_trampoline_blob_end - ap_trampoline_blob
