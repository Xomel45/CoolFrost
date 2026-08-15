#include "usyscall.h"

/* Exercises SYS_FWRITE end to end: overwrites the start of second.txt with
 * new content, closes, then re-opens fresh (a new vfs_node_t, a fresh
 * directory walk — not any kind of cache) and reads it back to prove the
 * write actually landed on disk. */

static const char path[]        = "/hda1/second.txt";
static const char new_text[]    = "OVERWRITTEN BY SYS_FWRITE TEST\n";
static const char open_fail[]   = "write_test: open failed\n";
static const char write_fail[]  = "write_test: write failed\n";
static const char readback_hdr[] = "write_test: read back -> ";

void _start(void *arg) {
    (void)arg;

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                     ustrlen(path), O_WRONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    int64_t n = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                    (uint64_t)(unsigned long)new_text, sizeof(new_text) - 1);
    usyscall1(SYS_CLOSE, (uint64_t)fd);

    if (n != (int64_t)(sizeof(new_text) - 1)) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)write_fail, sizeof(write_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path, ustrlen(path), O_RDONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail, sizeof(open_fail) - 1);
        usyscall0(SYS_EXIT);
    }

    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)readback_hdr, sizeof(readback_hdr) - 1);
    char buf[128];
    int64_t r = (int64_t)usyscall3(SYS_READ, (uint64_t)fd, (uint64_t)(unsigned long)buf, sizeof(buf));
    if (r > 0)
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)buf, (uint64_t)r);

    usyscall1(SYS_CLOSE, (uint64_t)fd);
    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
