#include "usyscall.h"
#include "umalloc.h"

/* Exercises userland/umalloc.h's malloc()/free() (and transitively
 * SYS_SBRK, cpu/syscall.c) — this project's first dynamically-sized
 * userland memory, everything before this used static buffers sized at
 * compile time.
 *   1. Allocates four differently-sized blocks (16B up through 10000B —
 *      the last one alone is bigger than umalloc_grow's minimum 4KB chunk,
 *      forcing a second/third SYS_SBRK call mid-run), fills each with a
 *      distinct byte pattern.
 *   2. Reads all four back — proves no two allocations alias the same
 *      memory (a corrupt allocator would show up here as garbled
 *      patterns), even across the multi-page block.
 *   3. Frees the second block, allocates a new smaller one, and re-checks
 *      EVERY block (the freed one's old neighbors included) — proves free()
 *      didn't corrupt anything still alive around it.
 */

static const char pass_pfx[] = "PASS: ";
static const char fail_pfx[] = "FAIL: ";
static const char nl[]       = "\n";

static void report(int ok, const char *label) {
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)(ok ? pass_pfx : fail_pfx), 6);
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)label, ustrlen(label));
    usyscall2(SYS_WRITE, (uint64_t)(unsigned long)nl, 1);
}

static void fill(uint8_t *buf, uint64_t n, uint8_t base) {
    for (uint64_t i = 0; i < n; i++) buf[i] = (uint8_t)(base + (i % 251));
}

static int check(const uint8_t *buf, uint64_t n, uint8_t base) {
    for (uint64_t i = 0; i < n; i++)
        if (buf[i] != (uint8_t)(base + (i % 251))) return 0;
    return 1;
}

#define N_BLOCKS 4
static const uint64_t sizes[N_BLOCKS] = { 16, 100, 4000, 10000 };
static const uint8_t  bases[N_BLOCKS] = { 0x11, 0x42, 0x99, 0xC7 };

void _start(void *arg) {
    (void)arg;

    void *ptr[N_BLOCKS];

    int all_nonnull = 1;
    for (int i = 0; i < N_BLOCKS; i++) {
        ptr[i] = umalloc(sizes[i]);
        if (!ptr[i]) all_nonnull = 0;
        else fill((uint8_t *)ptr[i], sizes[i], bases[i]);
    }
    report(all_nonnull, "all allocations returned non-NULL");

    int all_correct = 1;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (!ptr[i] || !check((uint8_t *)ptr[i], sizes[i], bases[i]))
            all_correct = 0;
    }
    report(all_correct, "all block contents intact before any free");

    ufree(ptr[1]);
    void *replacement = umalloc(40);
    report(replacement != (void *)0, "reallocation after free returned non-NULL");
    if (replacement) fill((uint8_t *)replacement, 40, 0x5A);

    int survivors_intact = 1;
    if (!check((uint8_t *)ptr[0], sizes[0], bases[0])) survivors_intact = 0;
    if (!check((uint8_t *)ptr[2], sizes[2], bases[2])) survivors_intact = 0;
    if (!check((uint8_t *)ptr[3], sizes[3], bases[3])) survivors_intact = 0;
    if (replacement && !check((uint8_t *)replacement, 40, 0x5A)) survivors_intact = 0;
    report(survivors_intact, "surviving blocks intact after free + reallocation");

    ufree(ptr[0]);
    ufree(ptr[2]);
    ufree(ptr[3]);
    if (replacement) ufree(replacement);

    usyscall0(SYS_EXIT);
    for (;;) { }   /* unreachable: SYS_EXIT never returns */
}
