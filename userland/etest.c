#include "usyscall.h"

/* ext2 counterpart of userland/ctest.c — exercises SYS_OPEN's O_CREATE and
 * SYS_FWRITE's growth support on the ext2 driver (fs/ext2.c: ext2_create/
 * ext2_write) instead of FAT32:
 *   1. Creates a brand-new file on the ext2 partition (mounted at /ext1 by
 *      the shell — see Makefile: ext2_part.img is formatted with 1024-byte
 *      blocks, so the 12 direct block pointers only cover 12KB).
 *   2. Writes 13000 bytes in one call — more than 12 direct blocks can
 *      hold, forcing fs/ext2.c: get_or_alloc_block to allocate the
 *      single-indirect pointer block itself AND several blocks through it,
 *      all within one ext2_write call.
 *   3. Appends 2000 more, forcing further growth from an already-nonzero,
 *      already-past-direct-blocks chain — checks the indirect block gets
 *      re-read and extended rather than re-allocated.
 *   4. Re-opens fresh and reads the whole 15000 bytes back.
 */

static const char path[]         = "/ext1/newfile.txt";
static const char open_fail[]    = "etest: open/create failed\n";
static const char write_fail[]   = "etest: write failed\n";
static const char seek_fail[]    = "etest: seek failed\n";
static const char wrote_hdr[]    = "etest: wrote ";
static const char readback_hdr[] = "etest: read back ";
static const char bytes_sfx[]    = " bytes\n";

static char chunk1[13000];
static char chunk2[2000];

static void fill(char *buf, uint64_t n, char base) {
    for (uint64_t i = 0; i < n; i++)
        buf[i] = (char)(base + (char)(i % 26));
}

static void print_u64(uint64_t v) {
    char buf[20];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)buf, (uint64_t)n);
}

void _start(void *arg) {
    (void)arg;

    fill(chunk1, sizeof(chunk1), 'A');
    fill(chunk2, sizeof(chunk2), 'a');

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                     ustrlen(path), O_WRONLY | O_CREATE);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    int64_t n1 = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                     (uint64_t)(unsigned long)chunk1, sizeof(chunk1));
    if (n1 != (int64_t)sizeof(chunk1)) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)write_fail, sizeof(write_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    if (usyscall2(SYS_SEEK, (uint64_t)fd, (uint64_t)sizeof(chunk1)) != 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)seek_fail, sizeof(seek_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    int64_t n2 = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                     (uint64_t)(unsigned long)chunk2, sizeof(chunk2));
    if (n2 != (int64_t)sizeof(chunk2)) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)write_fail, sizeof(write_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    usyscall1(SYS_CLOSE, (uint64_t)fd);

    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)wrote_hdr, sizeof(wrote_hdr) - 1);
    print_u64((uint64_t)(n1 + n2));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bytes_sfx, sizeof(bytes_sfx) - 1);

    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path, ustrlen(path), O_RDONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    char buf[512];
    uint64_t total = 0;
    for (;;) {
        int64_t r = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                        (uint64_t)(unsigned long)buf, sizeof(buf));
        if (r <= 0) break;
        total += (uint64_t)r;
    }
    usyscall1(SYS_CLOSE, (uint64_t)fd);

    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)readback_hdr, sizeof(readback_hdr) - 1);
    print_u64(total);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)bytes_sfx, sizeof(bytes_sfx) - 1);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
