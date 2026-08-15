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
#define SYS_WRITE       7   /* rdi = user buffer, rsi = length (capped) -> kprint's it to
                              * the kernel console/serial. The only I/O exec'd ELFs have. */
#define SYS_GFX_PRESENT_RECT 8   /* rdi = user pixel buffer, rsi = packed x:y:w:h (16 bits
                              * each, high to low) -> copies + blits only that sub-rect.
                              * The real framebuffer is uncached MMIO (PD@0x4000, PCD+PWT —
                              * see boot/multiboot_entry.asm); writing the whole screen every
                              * frame there is the dominant per-frame cost, so a compositor
                              * that only needs to move something small (e.g. the cursor)
                              * should use this instead of SYS_GFX_PRESENT. */
#define SYS_OPEN        9   /* rdi = user path ptr, rsi = path length (no NUL, capped at
                              * MAX_PATH-1, fs/vfs.h), rdx = flags (O_RDONLY etc, fs/vfs.h)
                              * -> rax = fd (>=0), or a negative vfs_open() error code
                              * sign-extended into rax (read it back as int64_t) */
#define SYS_CLOSE       10  /* rdi = fd -> rax = 0, or (uint64_t)-1 on error */
#define SYS_READ        11  /* rdi = fd, rsi = user buffer ptr, rdx = size -> rax = bytes
                              * read (>=0), or (uint64_t)-1 on error (read as int64_t) */
#define SYS_SEEK        12  /* rdi = fd, rsi = absolute offset -> rax = 0, or
                              * (uint64_t)-1 on error */
#define SYS_FWRITE      13  /* rdi = fd, rsi = user buffer ptr, rdx = size -> rax = bytes
                              * written (>=0), or (uint64_t)-1 on error (read as int64_t).
                              * A write starting at or before EOF that reaches past it grows
                              * the file (FAT32 only, fs/fat32.c: fat32_write — allocates and
                              * links new FAT clusters, updates the on-disk directory entry).
                              * A write starting PAST the current EOF (a sparse "hole") isn't
                              * supported and fails. Combine with O_CREATE (fs/vfs.h) on
                              * SYS_OPEN to make a brand-new file first. */
#define SYS_STAT        14  /* rdi = user path ptr, rsi = path length (no NUL, capped at
                              * MAX_PATH-1), rdx = user vfs_stat_t* (fs/vfs.h) -> rax = 0, or
                              * (uint64_t)-1 if the path doesn't resolve. Doesn't open the
                              * file — no fd is allocated, no O_* flags involved. */
#define SYS_FSTAT       15  /* rdi = fd, rsi = user vfs_stat_t* -> rax = 0, or (uint64_t)-1
                              * if fd isn't open. Same result shape as SYS_STAT, just for a
                              * file the caller already has open instead of a fresh path. */
#define SYS_READDIR     16  /* rdi = fd (an already-open directory), rsi = 0-based index,
                              * rdx = user dirent_t* (fs/vfs.h) -> rax = 0 (entry copied into
                              * the buffer), or (uint64_t)-1 (index past the last entry, or
                              * any other error — indistinguishable, same as vfs_readdir()
                              * itself can't tell them apart). Open a directory the same way
                              * as a file (SYS_OPEN on its path) — the fd works for either. */
#define SYS_UNLINK      17  /* rdi = user path ptr, rsi = path length (no NUL, capped at
                              * MAX_PATH-1) -> rax = 0, or (uint64_t)-1 on error (not found,
                              * it's a directory — no rmdir semantics — or the underlying FS
                              * driver refuses, e.g. an ext4 extent-based file). */
#define SYS_SBRK        18  /* rdi = increment (bytes, as int64_t; 0 = query, negative =
                              * unsupported/fails) -> rax = the break BEFORE this call (so
                              * [old, old+increment) is the newly usable range), or
                              * (uint64_t)-1 if this task has no private address space (not
                              * an exec'd process), the request would cross heap_end
                              * (cpu/sched.h: task_t), or a page allocation failed partway
                              * (nothing is left half-mapped in a way the caller can rely on
                              * — brk only advances on full success). Raw page-granularity
                              * primitive; userland/umalloc.h layers a real malloc()/free()
                              * on top, the kernel itself has no notion of individual
                              * allocations, only the break. */
#define SYS_RENAME      19  /* rdi = old path ptr, rsi = old path length (no NUL), rdx = new
                              * path ptr, r10 = new path length (no NUL) — the first syscall
                              * in this kernel needing a 4th argument; r10 is the SysV
                              * register for it (rcx is unusable after `int`, this cpu never
                              * gets to a syscall/sysret path where that would matter) and
                              * registers_t already saves it (cpu/interrupt.asm), just never
                              * read by a case before this one. Both paths capped at
                              * MAX_PATH-1. -> rax = 0, or (uint64_t)-1 on error: source not
                              * found, it's a directory (no rmdir semantics), old/new paths
                              * are on DIFFERENT mounted filesystems (fs/vfs.c: vfs_rename
                              * refuses — would need an actual data copy, not a same-FS
                              * detach-and-reattach), or the underlying FS driver refuses
                              * (e.g. an ext4 extent-based file). No data is copied either
                              * way — same underlying file, just a different directory
                              * entry (fs/fat32.c: fat32_rename, fs/ext2.c: ext2_rename). */

/* ── Screen API — scaffold ────────────────────────────────────────────────
 *
 * Everything above SYS_GFX_PRESENT[_RECT] makes an exec'd userland ELF
 * maintain its OWN full pixel buffer and blit the whole thing (or a
 * sub-rect) — every window/compositor-style program has to reimplement
 * fill/rect/line/text drawing itself in userland C. gfx/gfx.c already HAS
 * all of that as plain C functions (gfx_fill_rect, gfx_draw_rect,
 * gfx_hline, gfx_vline, gfx_draw_text, gfx_draw_char) operating on
 * gfx_screen() — the WM (user/wm.c) already calls them directly since it's
 * linked into kernel.elf itself, not a separate exec'd process. This block
 * exposes that SAME set of primitives to an exec'd process too, so it can
 * draw straight into the kernel's screen backbuffer without carrying its
 * own copy — same draw-then-SYS_GFX_PRESENT_RECT split every case here
 * uses (a primitive touches the backbuffer; presenting to the real
 * framebuffer is always a separate, explicit step, so a caller doing
 * several draws only pays for one blit).
 *
 * Two are wired up for real (proof this scaffold works end to end, not
 * just reserved numbers) — SYS_GFX_FILL_RECT and SYS_GFX_DRAW_TEXT, picked
 * as the two most broadly useful on their own (a solid box, a line of
 * text). The rest are RESERVED numbers only: no `case` for them yet in
 * cpu/syscall.c's switch, so calling one today just falls through to the
 * default case and returns (uint64_t)-1 — safe, not a crash, exactly what
 * an unimplemented syscall should do. Wiring each one up later is a small,
 * mechanical extension of the two real ones below (same is_user_range/
 * gfx_screen()-null checks, same pack/unpack style), not a redesign. */
#define SYS_GFX_FILL_RECT 20  /* rdi = packed x:y:w:h (16 bits each, high to low — same
                              * packing SYS_GFX_PRESENT_RECT's rsi already uses), rsi =
                              * 0xXRGB color -> rax = 0, or (uint64_t)-1 if gfx isn't up
                              * (gfx_screen() == NULL, e.g. VGA text fallback, no linear
                              * framebuffer). gfx/gfx.c: gfx_fill_rect. */
#define SYS_GFX_DRAW_TEXT 21  /* rdi = packed x:y (16 bits each, high to low, packed into
                              * the low 32 bits of rdi), rsi = user text ptr, rdx = text
                              * length (no NUL, capped at 256), r10 = packed fg:bg (32 bits
                              * each, high to low; bg = GFX_TRANSPARENT, gfx/gfx.h, skips
                              * background pixels — same sentinel gfx_draw_text already
                              * uses) -> rax = 0, or (uint64_t)-1 if gfx isn't up or the text
                              * pointer fails is_user_range. gfx/gfx.c: gfx_draw_text. */
#define SYS_GFX_DRAW_RECT 22  /* RESERVED — 1px outline, gfx/gfx.c: gfx_draw_rect. Same
                              * packed-x:y:w:h + color shape as SYS_GFX_FILL_RECT would
                              * naturally extend to. Not dispatched yet. */
#define SYS_GFX_HLINE     23  /* RESERVED — gfx/gfx.c: gfx_hline. packed x:y, w, color.
                              * Not dispatched yet. */
#define SYS_GFX_VLINE     24  /* RESERVED — gfx/gfx.c: gfx_vline. packed x:y, h, color.
                              * Not dispatched yet. */
#define SYS_GFX_DRAW_CHAR 25  /* RESERVED — single CP437 glyph, gfx/gfx.c: gfx_draw_char.
                              * Same shape as SYS_GFX_DRAW_TEXT with a 1-byte "string".
                              * Not dispatched yet. */
#define SYS_GFX_FLUSH_RECT 26 /* rdi = packed x:y:w:h (16 bits each, high to low) -> rax = 0,
                              * or (uint64_t)-1 if gfx isn't up. Blits that region of the
                              * kernel's CURRENT screen backbuffer straight to the real
                              * framebuffer — no upload, unlike SYS_GFX_PRESENT_RECT (which
                              * needs the caller's OWN full pixel buffer to copy FROM first,
                              * and would stomp whatever SYS_GFX_FILL_RECT/DRAW_TEXT just
                              * drew with that buffer's — likely garbage — content instead).
                              * This is the correct pairing for the two direct-draw
                              * primitives above: draw into the kernel's own backbuffer,
                              * then flush just the touched region here. Also wired up for
                              * real (gfx/gfx.c: gfx_present_rect), not reserved. */

/* Installs the int 0x80 gate at DPL=3. Called once from kstart(), after
 * isr_install() (needs the IDT already allocated/loaded). */
void syscall_install(void);

/* Dispatches one syscall; called from syscall_stub (cpu/interrupt.asm).
 * Writes the return value into r->rax directly. */
void syscall_dispatch(registers_t *r);

#endif
