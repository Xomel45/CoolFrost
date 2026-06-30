#include "./stdio.h"
#include "./ctype.h"
#include "../drivers/screen.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "./stdlib.h"

void printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    while (*format) {
        if (*format == '%') {
            format++;

            // ---- %ld, %lu, %lx ----
            if (*format == 'l') {
                format++;

                if (*format == 'd') {
                    int64_t v = va_arg(args, int64_t);
                    char buf[32];
                    itoa(v, buf, 10);
                    for (char *p = buf; *p; p++) kprint_char(*p);
                }
                else if (*format == 'u') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 10);
                    for (char *p = buf; *p; p++) kprint_char(*p);
                }
                else if (*format == 'x') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 16);
                    for (char *p = buf; *p; p++) kprint_char(*p);
                }
                else if (*format == 'o') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 8);
                    for (char *p = buf; *p; p++) kprint_char(*p);
                }

                format++;
                continue;
            }

            // ---- %d ----
            if (*format == 'd') {
                int32_t v = va_arg(args, int32_t);
                char buf[32];
                itoa(v, buf, 10);
                for (char *p = buf; *p; p++) kprint_char(*p);
            }
            // ---- %u ----
            else if (*format == 'u') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 10);
                for (char *p = buf; *p; p++) kprint_char(*p);
            }
            // ---- %x ----
            else if (*format == 'x') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 16);
                for (char *p = buf; *p; p++) kprint_char(*p);
            }
            // ---- %X ----
            else if (*format == 'X') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 16);
                for (char *p = buf; *p; p++) kprint_char(toupper(*p));
            }
            // ---- %o ----
            else if (*format == 'o') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 8);
                for (char *p = buf; *p; p++) kprint_char(*p);
            }
            // ---- %f ----
            else if (*format == 'f') {
                double v = va_arg(args, double);
                char buf[64];
                ftoa(v, buf, 10, 6);
                for (char *p = buf; *p; p++) kprint_char(*p);
            } else if (*format == 'p') {
                void *v = va_arg(args, void *);
                char buf[32];
                kprint_char('0');
                kprint_char('x');
                itoa((uintptr_t)v, buf, 16);
                for (char *p = buf; *p; p++) kprint_char(*p);
            }
            // ---- %c ----
            else if (*format == 'c') {
                char v = va_arg(args, int);
                kprint_char(v);
            }
            // ---- %s ----
            else if (*format == 's') {
                char *v = va_arg(args, char *);
                for (char *p = v; *p; p++) kprint_char(*p);
            }
            // --- %% ---
            else if (*format == '%') {
                kprint_char('%');
            }

            format++;
        }
        else {
            kprint_char(*format++);
        }
    }

    va_end(args);
}

int sprintf(char *s, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char *start = s;

    while (*format) {
        if (*format == '%') {
            format++;

            // ---- %ld, %lu, %lx ----
            if (*format == 'l') {
                format++;

                if (*format == 'd') {
                    int64_t v = va_arg(args, int64_t);
                    char buf[32];
                    itoa(v, buf, 10);
                    for (char *p = buf; *p; p++) *s++ = *p;
                }
                else if (*format == 'u') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 10);
                    for (char *p = buf; *p; p++) *s++ = *p;
                }
                else if (*format == 'x') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 16);
                    for (char *p = buf; *p; p++) *s++ = *p;
                }
                else if (*format == 'o') {
                    uint64_t v = va_arg(args, uint64_t);
                    char buf[32];
                    itoa(v, buf, 8);
                    for (char *p = buf; *p; p++) *s++ = *p;
                }

                format++;
                continue;
            }

            // ---- %d ----
            if (*format == 'd') {
                int32_t v = va_arg(args, int32_t);
                char buf[32];
                itoa(v, buf, 10);
                for (char *p = buf; *p; p++) *s++ = *p;
            }
            // ---- %u ----
            else if (*format == 'u') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 10);
                for (char *p = buf; *p; p++) *s++ = *p;
            }
            // ---- %x ----
            else if (*format == 'x') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 16);
                for (char *p = buf; *p; p++) *s++ = *p;
            }
            // ---- %X ----
            else if (*format == 'X') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 16);
                for (char *p = buf; *p; p++) *s++ = toupper(*p);
            }
            // ---- %o ----
            else if (*format == 'o') {
                uint32_t v = va_arg(args, uint32_t);
                char buf[32];
                itoa(v, buf, 8);
                for (char *p = buf; *p; p++) *s++ = *p;
            }
            // ---- %f ----
            else if (*format == 'f') {
                double v = va_arg(args, double);
                char buf[64];
                ftoa(v, buf, 10, 6);
                for (char *p = buf; *p; p++) *s++ = *p;
            } else if (*format == 'p') {
                void *v = va_arg(args, void *);
                char buf[32];
                *s++ = '0';
                *s++ = 'x';
                itoa((uintptr_t)v, buf, 16);
                for (char *p = buf; *p; p++) *s++ = *p;
            }
            // ---- %c ----
            else if (*format == 'c') {
                char v = va_arg(args, int);
                *s++ = v;
            }
            // ---- %s ----
            else if (*format == 's') {
                char *v = va_arg(args, char *);
                for (char *p = v; *p; p++) *s++ = *p;
            }
            // --- %% ---
            else if (*format == '%') {
                *s++ = '%';
            }

            format++;
        }
        else {
            *s++ = *format++;
        }
    }

    *s = '\0';
    va_end(args);
    return s - start;
}

/* ── vsnprintf / snprintf ───────────────────────────────────────────────── *
 * n  = max bytes to write including the null terminator (0 = dry-run).     *
 * Returns the number of chars that would have been written (excl. '\0').   */
int vsnprintf(char *s, size_t n, const char *fmt, va_list args) {
    size_t total = 0;

/* Write one char into the bounded buffer */
#define _E(c) do { \
    if (n > 0 && total < n - 1) s[total] = (char)(c); \
    total++; \
} while (0)

/* Write a NUL-terminated string */
#define _ES(p) do { \
    for (const char *_q = (const char *)(p); *_q; _q++) _E(*_q); \
} while (0)

    while (*fmt) {
        if (*fmt != '%') { _E(*fmt++); continue; }
        fmt++;

        /* %l prefix */
        if (*fmt == 'l') {
            fmt++;
            char _buf[32];
            if (*fmt == 'd') {
                itoa((int64_t)va_arg(args, int64_t), _buf, 10); _ES(_buf);
            } else if (*fmt == 'u') {
                itoa((int64_t)va_arg(args, uint64_t), _buf, 10); _ES(_buf);
            } else if (*fmt == 'x') {
                itoa((int64_t)va_arg(args, uint64_t), _buf, 16); _ES(_buf);
            } else if (*fmt == 'o') {
                itoa((int64_t)va_arg(args, uint64_t), _buf, 8);  _ES(_buf);
            }
            fmt++; continue;
        }

        char _buf[64];
        if (*fmt == 'd') {
            itoa((int64_t)va_arg(args, int32_t), _buf, 10); _ES(_buf);
        } else if (*fmt == 'u') {
            itoa((int64_t)(uint64_t)va_arg(args, uint32_t), _buf, 10); _ES(_buf);
        } else if (*fmt == 'x') {
            itoa((int64_t)(uint64_t)va_arg(args, uint32_t), _buf, 16); _ES(_buf);
        } else if (*fmt == 'X') {
            itoa((int64_t)(uint64_t)va_arg(args, uint32_t), _buf, 16);
            for (char *p = _buf; *p; p++) _E(toupper(*p));
        } else if (*fmt == 'o') {
            itoa((int64_t)(uint64_t)va_arg(args, uint32_t), _buf, 8); _ES(_buf);
        } else if (*fmt == 'f') {
            ftoa(va_arg(args, double), _buf, 10, 6); _ES(_buf);
        } else if (*fmt == 'p') {
            _E('0'); _E('x');
            itoa((int64_t)(uintptr_t)va_arg(args, void *), _buf, 16); _ES(_buf);
        } else if (*fmt == 'c') {
            _E((char)va_arg(args, int));
        } else if (*fmt == 's') {
            const char *v = va_arg(args, const char *);
            if (!v) v = "(null)";
            _ES(v);
        } else if (*fmt == '%') {
            _E('%');
        }
        fmt++;
    }

#undef _E
#undef _ES

    if (n > 0) s[total < n ? total : n - 1] = '\0';
    return (int)total;
}

int snprintf(char *s, size_t n, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(s, n, fmt, args);
    va_end(args);
    return r;
}
