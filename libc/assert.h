#ifndef ASSERT_H
#define ASSERT_H

/* ── Kernel panic / assertion ───────────────────────────────────────────── */

__attribute__((noreturn))
void __kpanic(const char *file, int line, const char *msg);

/* Assert: panics with file + line + failed expression if cond is false */
#define KASSERT(cond) \
    do { if (!(cond)) __kpanic(__FILE__, __LINE__, #cond); } while (0)

/* Assert with a custom message instead of the expression text */
#define KASSERT_MSG(cond, msg) \
    do { if (!(cond)) __kpanic(__FILE__, __LINE__, (msg)); } while (0)

/* Unconditional panic */
#define KPANIC(msg) __kpanic(__FILE__, __LINE__, (msg))

/* Mark unreachable code — panics in debug, hints the optimizer in release */
#define UNREACHABLE() __kpanic(__FILE__, __LINE__, "unreachable")

/* Static assert (compile-time) */
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif
