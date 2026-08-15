#include "usyscall.h"

/* Fragmentation probe for fs/fat32.c: fat32_alloc_run and fs/ext2.c:
 * alloc_block_near — constructs a small hole, a permanent occupant, another
 * small hole, five more permanent occupants, then writes a file that needs
 * far more than one cluster/block. On both filesystems the two 1-unit holes
 * are individually too small for that file, so a correct extent-aware
 * allocator has to skip past them and land on a genuinely large run further
 * out — a plain "take whatever's free next" allocator would instead split
 * the file across the holes.
 *
 * This program only performs the operations and reports whether the writes
 * themselves succeeded; it has no way to see physical cluster/block numbers
 * from userland. Actually confirming contiguity is done afterward by
 * inspecting the resulting image from the host (a Python FAT walk for
 * FAT32, `debugfs stat` for ext2) — see the scratchpad notes from whichever
 * session added this file.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static uint8_t fillbuf[4096];

/* Creates `path`, writes exactly `bytes` bytes (any nonzero value — content
 * doesn't matter, only how many allocation units it forces), closes it.
 * Returns 0 if every byte was written, -1 otherwise. */
static int create_and_fill(const char *path, uint32_t bytes) {
    int64_t fd = (int64_t)usyscall3(SYS_OPEN, (uint64_t)(unsigned long)path,
                                     ustrlen(path), O_WRONLY | O_CREATE);
    if (fd < 0) return -1;

    uint32_t written = 0;
    while (written < bytes) {
        uint32_t chunk = bytes - written;
        if (chunk > sizeof(fillbuf)) chunk = sizeof(fillbuf);
        int64_t n = (int64_t)usyscall3(SYS_FWRITE, (uint64_t)fd,
                                        (uint64_t)(unsigned long)fillbuf, chunk);
        if (n <= 0) break;
        written += (uint32_t)n;
    }

    usyscall1(SYS_CLOSE, (uint64_t)fd);
    return written == bytes ? 0 : -1;
}

static void unlink_path(const char *path) {
    usyscall2(SYS_UNLINK, (uint64_t)(unsigned long)path, ustrlen(path));
}

/* `prefix` is the mount path ("/hda1" or "/ext1"); `target_name` the plain
 * 8.3-safe basename the target file gets (needed so this same routine can
 * hand back a name the host-side FAT32 parser can look up in the directory
 * table without guessing). */
static void punch_and_probe(const char *prefix, const char *target_path, uint32_t target_bytes) {
    char h1[32], o1[32], h2[32], o2[32], o3[32], o4[32], o5[32], o6[32];
    const char *names[] = {"/frh1.txt", "/fro1.txt", "/frh2.txt", "/fro2.txt",
                            "/fro3.txt", "/fro4.txt", "/fro5.txt", "/fro6.txt"};
    char *bufs[] = {h1, o1, h2, o2, o3, o4, o5, o6};
    for (int i = 0; i < 8; i++) {
        int j = 0;
        while (prefix[j]) { bufs[i][j] = prefix[j]; j++; }
        int k = 0;
        while (names[i][k]) { bufs[i][j] = names[i][k]; j++; k++; }
        bufs[i][j] = '\0';
    }

    report(create_and_fill(h1, 1) == 0, "hole-1 marker file written");
    report(create_and_fill(o1, 1) == 0, "obstruction-1 written (kept)");
    unlink_path(h1);   /* punches a single isolated free unit behind o1 */

    report(create_and_fill(h2, 1) == 0, "hole-2 marker file written");
    report(create_and_fill(o2, 1) == 0, "obstruction-2 written (kept)");
    report(create_and_fill(o3, 1) == 0, "obstruction-3 written (kept)");
    report(create_and_fill(o4, 1) == 0, "obstruction-4 written (kept)");
    report(create_and_fill(o5, 1) == 0, "obstruction-5 written (kept)");
    report(create_and_fill(o6, 1) == 0, "obstruction-6 written (kept)");
    unlink_path(h2);   /* punches a second isolated free unit */

    int rc = create_and_fill(target_path, target_bytes);
    report(rc == 0, "target file (needs many units) written in full");
}

void _start(void *arg) {
    (void)arg;

    /* 70000 bytes forces multiple allocation units on either filesystem
     * regardless of actual cluster/block size (FAT32 cluster <= 64KB,
     * ext2 block here is 1KB — see Makefile's ext2_part.img recipe) while
     * staying well inside ext2's direct+single-indirect range, no
     * double-indirect growth involved. */
    punch_and_probe("/hda1", "/hda1/frtarget.txt", 70000);
    punch_and_probe("/ext1", "/ext1/frtarget.txt", 70000);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
