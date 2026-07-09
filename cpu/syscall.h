#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

/* int 0x80 syscall numbers. Ring3 convention: rax = number, args in
 * rdi/rsi/rdx/r10/r8 (SysV order, skipping rcx which `int` doesn't clobber
 * but we avoid anyway for symmetry with a future syscall/sysret path).
 * Return value goes back in rax. */
#define SYS_EXIT        0   /* no args, never returns                         */
#define SYS_YIELD       1   /* no args                                        */
#define SYS_GET_TICKS   2   /* no args -> rax = current tick count            */
#define SYS_EVENT_POLL  3   /* rdi = event_t* (user ptr) -> rax = 1 got one, 0 empty */
#define SYS_GFX_PRESENT 4   /* rdi = user pixel buffer (screen w*h*4 bytes) -> copied
                              * into the kernel backbuffer, then blitted to the real FB */
#define SYS_FB_INFO     5   /* no args -> rax = (width<<32)|height, or 0 if gfx isn't up */
#define SYS_GET_FONT    6   /* rdi = user buffer (>=256*16 bytes) -> filled with the
                              * 8x16 CP437 glyph table (drivers/screen.c), once at startup */

/* Installs the int 0x80 gate at DPL=3. Called once from kstart(), after
 * isr_install() (needs the IDT already allocated/loaded). */
void syscall_install(void);

/* Dispatches one syscall; called from syscall_stub (cpu/interrupt.asm).
 * Writes the return value into r->rax directly. */
void syscall_dispatch(registers_t *r);

#endif
