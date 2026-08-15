#include "usyscall.h"

/* Deliberately fragments a file on ext2 so `defrag /ext1` (fs/ext2.c:
 * ext2_defrag) has something real to fix — same recipe as userland/
 * dftest.c uses for FAT32:
 *   1. Creates edtarget.txt, writes 1 byte — exactly one block.
 *   2. Creates edobs.txt (kept, never deleted) right after it — lands in
 *      the very next block, since nothing else has touched this part of
 *      the volume yet.
 *   3. Appends 70000 bytes to edtarget.txt — comfortably past ext2's 12
 *      direct blocks (12KB at this volume's 1024-byte block size, see
 *      Makefile's ext2_part.img recipe) into the single-indirect range.
 *      Block 0 is already fixed at step 1's location; growing past it has
 *      to skip over edobs's block, guaranteeing at least one break.
 *
 * Only sets up the fragmented state and exits — running `defrag /ext1`
 * and confirming the result is done from the shell/host side afterward.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static uint8_t bigbuf[70000];

void _start(void *arg) {
    (void)arg;

    for (uint32_t i = 0; i < sizeof(bigbuf); i++)
        bigbuf[i] = (uint8_t)(i & 0xFF);

    static const char target[] = "/ext1/edtarget.txt";
    static const char obs[]    = "/ext1/edobs.txt";

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)target,
                                     ustrlen(target), O_WRONLY | O_CREATE);
    report(fd >= 0, "create target succeeded");
    if (fd < 0) { usyscall0(SYS_EXIT); }

    int64_t n = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                    (uint64_t)(unsigned long)bigbuf, 1);
    report(n == 1, "first 1-byte (1-block) write succeeded");

    int64_t ofd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)obs,
                                      ustrlen(obs), O_WRONLY | O_CREATE);
    report(ofd >= 0, "create obstruction succeeded");
    if (ofd >= 0) {
        int64_t on = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)ofd,
                                         (uint64_t)(unsigned long)bigbuf, 1);
        report(on == 1, "obstruction write succeeded");
        usyscall1(SYS_CLOSE, (uint64_t)ofd);
    }

    if (usyscall2(SYS_SEEK, (uint64_t)fd, 1) != 0) {
        report(0, "seek to offset 1 succeeded");
        usyscall1(SYS_CLOSE, (uint64_t)fd);
        usyscall0(SYS_EXIT);
    }

    int64_t n2 = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                     (uint64_t)(unsigned long)bigbuf, sizeof(bigbuf));
    report(n2 == (int64_t)sizeof(bigbuf), "large append (forces a skip past the obstruction) succeeded");

    usyscall1(SYS_CLOSE, (uint64_t)fd);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
