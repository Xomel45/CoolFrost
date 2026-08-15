#ifndef USERLAND_USYSCALL_H
#define USERLAND_USYSCALL_H

#include <stdint.h>

/* Standalone copy of the int 0x80 ABI (cpu/syscall.h) for programs built
 * outside kernel.elf entirely (see userland/user.ld) — deliberately not
 * shared via #include, this is "userland headers" vs "kernel headers",
 * they're never the same file even when the numbers have to match. */
#define SYS_EXIT  0
#define SYS_WRITE 7
#define SYS_OPEN  9    /* a1 = path ptr, a2 = path length (no NUL), a3 = flags -> fd,
                        * or a negative error code sign-extended into the return value
                        * (cast back to int64_t to read it) */
#define SYS_CLOSE 10   /* a1 = fd -> 0, or -1 */
#define SYS_READ  11   /* a1 = fd, a2 = buffer ptr, a3 = size -> bytes read, or -1 */
#define SYS_SEEK  12   /* a1 = fd, a2 = absolute offset -> 0, or -1 */
#define SYS_FWRITE 13  /* a1 = fd, a2 = buffer ptr, a3 = size -> bytes written, or -1.
                        * Writing past the file's current size grows it (FAT32 only, via
                        * new FAT clusters), as long as the write starts at or before the
                        * current end — no sparse "hole" writes, see cpu/syscall.h. */
#define SYS_STAT   14  /* a1 = path ptr, a2 = path length (no NUL), a3 = vfs_stat_t* ->
                        * 0, or -1 if the path doesn't resolve. No fd, no O_* flags. */
#define SYS_FSTAT  15  /* a1 = fd, a2 = vfs_stat_t* -> 0, or -1 if fd isn't open. */
#define SYS_READDIR 16 /* a1 = fd (an open directory), a2 = 0-based index, a3 = dirent_t* ->
                        * 0 (entry filled), or -1 (past the last entry, or any error). */
#define SYS_UNLINK 17  /* a1 = path ptr, a2 = path length (no NUL) -> 0, or -1 on error. */
#define SYS_SBRK   18  /* a1 = increment (int64_t; 0 = query, negative = unsupported) ->
                        * break BEFORE this call, or -1. See userland/umalloc.h for the
                        * actual malloc()/free() built on top — this is raw page-granularity
                        * heap growth only, no notion of individual allocations. */
#define SYS_RENAME 19  /* a1 = old path ptr, a2 = old path length (no NUL), a3 = new path
                        * ptr, a4 = new path length (no NUL) -> 0, or -1 on error. Same
                        * filesystem only — no data copied, see cpu/syscall.h. The first
                        * syscall here needing usyscall4 (below), not usyscall3. */

/* Also mirrored here (defined in cpu/syscall.h from the start, but no
 * userland test needed them until now): raw whole-buffer GFX ops. */
#define SYS_FB_INFO 5          /* no args -> width<<32|height, or 0 if gfx isn't up */
#define SYS_GFX_PRESENT_RECT 8 /* a1 = user pixel buffer, a2 = packed x:y:w:h — needs the
                                * CALLER's own full backbuffer to upload from, unlike
                                * SYS_GFX_FLUSH_RECT below. */

/* Screen API scaffold (cpu/syscall.h has the full picture — draw straight
 * into the kernel's own screen backbuffer, gfx/gfx.c, without maintaining
 * a local copy the way SYS_GFX_PRESENT[_RECT] requires). Only these three
 * are actually wired up; SYS_GFX_DRAW_RECT(22)/HLINE(23)/VLINE(24)/
 * DRAW_CHAR(25) are reserved numbers with no dispatch case yet — calling
 * them today just returns -1, safely. */
#define SYS_GFX_FILL_RECT 20  /* a1 = packed x:y:w:h (16 bits each, high to low), a2 =
                              * 0xXRGB color -> 0, or -1 if gfx isn't up. */
#define SYS_GFX_DRAW_TEXT 21  /* a1 = packed x:y (16 bits each, high to low, in the low 32
                              * bits), a2 = text ptr, a3 = text length (no NUL, capped at
                              * 256), a4 = packed fg:bg (32 bits each, high to low; bg =
                              * GFX_TRANSPARENT, below, skips background pixels) -> 0, or
                              * -1. Needs usyscall4 (below), same as SYS_RENAME. */
#define SYS_GFX_FLUSH_RECT 26 /* a1 = packed x:y:w:h (16 bits each, high to low) -> 0, or
                              * -1. Blits that region of the kernel's CURRENT backbuffer to
                              * the real framebuffer — no upload. The correct thing to call
                              * after SYS_GFX_FILL_RECT/DRAW_TEXT (SYS_GFX_PRESENT_RECT would
                              * overwrite whatever they just drew with this program's own,
                              * likely-uninitialized, pixel buffer instead). */

/* gfx/gfx.h: GFX_TRANSPARENT — an impossible XRGB value (bit 24 set) used
 * as SYS_GFX_DRAW_TEXT's bg to mean "don't paint a background, just the
 * glyph". */
#define GFX_TRANSPARENT 0x01000000u

/* fs/vfs.h open flags, duplicated here for the same reason as the syscall
 * numbers above: userland headers never #include kernel headers. */
#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREATE  0x04
#define O_APPEND  0x08

/* fs/vfs.h VFS_FILE/VFS_DIRECTORY, for reading vfs_stat_t.type below */
#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02

/* fs/vfs.h: vfs_stat_t, duplicated byte-for-byte — same split as proc_args_t
 * further down. What SYS_STAT/SYS_FSTAT fill in. */
typedef struct {
    uint64_t size;
    uint32_t inode;
    uint8_t  type;
} vfs_stat_t;

/* fs/vfs.h: dirent_t, duplicated byte-for-byte — what SYS_READDIR fills in.
 * MAX_FILENAME (fs/vfs.h) is 128; hardcoded here for the same "never share
 * kernel headers" reason as everything else in this file. */
#define VFS_MAX_FILENAME 128

typedef struct {
    char     name[VFS_MAX_FILENAME];
    uint64_t size;
    uint8_t  type;
} dirent_t;

static inline uint64_t usyscall0(uint64_t num) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline uint64_t usyscall1(uint64_t num, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1) : "memory");
    return ret;
}

static inline uint64_t usyscall2(uint64_t num, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "memory");
    return ret;
}

static inline uint64_t usyscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return ret;
}

/* r10, not rcx, is the 4th SysV integer-arg register available to `int`
 * (rcx holds the return address after a real `syscall` instruction, not
 * used here — see cpu/syscall.h). GCC/Clang have no single-letter operand
 * constraint for a fixed non-rdi/rsi/rdx/rax register, so a1 is bound into
 * r10 explicitly via a local register variable instead. */
static inline uint64_t usyscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "memory");
    return ret;
}

/* strlen() isn't available to a -nostdlib userland program. */
static inline uint64_t ustrlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Process-side view of the argv page kernel/elf.c builds for every exec'd
 * process and passes as _start(void *arg) — argc plus pointers into strings
 * packed in the same page. argv[0] is the executable's own path (Unix
 * convention, enforced by the shell's `exec` command, not by the kernel).
 * PROC_MAX_ARGS must match ELF_MAX_ARGS in kernel/elf.h and this struct's
 * layout must match kernel/elf.c's proc_args_t byte-for-byte — same
 * "userland headers never share kernel headers" split as the syscall
 * numbers above, just for a struct instead of a set of #defines. */
#define PROC_MAX_ARGS 8

typedef struct {
    uint64_t argc;
    char    *argv[PROC_MAX_ARGS];
} proc_args_t;

#endif
