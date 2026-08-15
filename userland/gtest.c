#include "usyscall.h"

/* Smoke-tests the screen API scaffold (cpu/syscall.h: SYS_GFX_FILL_RECT/
 * DRAW_TEXT/FLUSH_RECT) — draws directly into the kernel's own screen
 * backbuffer (no local pixel buffer maintained by this program at all,
 * unlike a real WM/compositor) and flushes just the touched region.
 *
 * This only checks the syscalls return success — there's no way to
 * visually confirm pixel content from this headless, text-only serial-log
 * testing setup (no screenshot capability here). If SYS_FB_INFO reports no
 * framebuffer at all (possible depending on how QEMU was launched), the
 * rest is skipped rather than reported as failure — that's an environment
 * fact, not a bug in the scaffold.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char skip_pfx[] = "SKIP: ";
static const char nl[]       = "\n";

static void report(const char *pfx, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)pfx, 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static const char msg[] = "CoolFrost";

void _start(void *arg) {
    (void)arg;

    uint64_t fb = usyscall0(SYS_FB_INFO);
    if (fb == 0) {
        report(skip_pfx, "no linear framebuffer in this environment — gfx syscalls untestable here");
        usyscall0(SYS_EXIT);
    }
    report(pass_pfx, "SYS_FB_INFO reports a framebuffer");

    /* A small filled box near the top-left, a line of text just below it. */
    uint64_t rect_packed = ((uint64_t)10 << 48) | ((uint64_t)10 << 32) | ((uint64_t)60 << 16) | 20;
    uint64_t rc1 = usyscall2(SYS_GFX_FILL_RECT, rect_packed, 0x00FF3366u);
    report(rc1 == 0 ? pass_pfx : fail_pfx, "SYS_GFX_FILL_RECT returned success");

    uint64_t xy_packed = ((uint64_t)10 << 16) | 35;
    uint64_t fgbg = ((uint64_t)0x00FFFFFFu << 32) | (uint64_t)GFX_TRANSPARENT;
    uint64_t rc2 = usyscall4(SYS_GFX_DRAW_TEXT, xy_packed,
                             (uint64_t)(unsigned long)msg, ustrlen(msg), fgbg);
    report(rc2 == 0 ? pass_pfx : fail_pfx, "SYS_GFX_DRAW_TEXT returned success");

    /* Flush a region covering both the box and the text line. */
    uint64_t flush_packed = ((uint64_t)0 << 48) | ((uint64_t)0 << 32) | ((uint64_t)100 << 16) | 60;
    uint64_t rc3 = usyscall1(SYS_GFX_FLUSH_RECT, flush_packed);
    report(rc3 == 0 ? pass_pfx : fail_pfx, "SYS_GFX_FLUSH_RECT returned success");

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
