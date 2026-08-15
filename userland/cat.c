#include "usyscall.h"

/* Exercises SYS_OPEN/SYS_READ/SYS_CLOSE plus argv end to end: opens the
 * file named in argv[1], or /hda1/greeting.txt if run with no arguments,
 * and echoes its contents back out via SYS_WRITE. */

static const char default_path[]  = "/hda1/greeting.txt";
static const char open_fail_pfx[] = "cat: failed to open ";
static const char read_fail_msg[] = "cat: read failed\n";
static const char nl[]            = "\n";

void _start(void *arg) {
    proc_args_t *args = (proc_args_t *)arg;
    const char *path = (args && args->argc > 1) ? args->argv[1] : default_path;

    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                     ustrlen(path), O_RDONLY);
    if (fd < 0) {
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)open_fail_pfx, sizeof(open_fail_pfx) - 1);
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)path, ustrlen(path));
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
        usyscall0(SYS_EXIT);
    }

    char buf[256];
    for (;;) {
        int64_t n = (int64_t)usyscall3(SYS_READ, (uint64_t)fd,
                                        (uint64_t)(unsigned long)buf, sizeof(buf));
        if (n < 0) {
            usyscall2(SYS_WRITE, (uint64_t)(unsigned long)read_fail_msg, sizeof(read_fail_msg) - 1);
            break;
        }
        if (n == 0) break;
        usyscall2(SYS_WRITE, (uint64_t)(unsigned long)buf, (uint64_t)n);
    }

    usyscall1(SYS_CLOSE, (uint64_t)fd);
    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
