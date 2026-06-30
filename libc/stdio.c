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
 * n  = max bytes including null terminator (SIZE_MAX = unbounded).          *
 * Returns chars that WOULD have been written (C99 semantics).               *
 * Supports: flags (-,0), width, length modifier (l), specifiers duxXocsfp% */
int vsnprintf(char *s, size_t n, const char *fmt, va_list args) {
    size_t total = 0;

/* Emit one character into the bounded output buffer */
#define _EC(c) do { \
    if (n > 1 && total < n - 1) s[total] = (char)(c); \
    total++; \
} while (0)

/* Emit value string 'vp' with optional width / alignment / padding */
#define _EMIT_PADDED(vp, width, zero_pad, left_align) do { \
    const char *_v = (vp); \
    size_t _vl = 0; \
    for (const char *_p = _v; *_p; _p++) _vl++; \
    if (!(left_align) && (int)_vl < (width)) { \
        char _pad = (zero_pad) ? '0' : ' '; \
        for (int _i = 0; _i < (width) - (int)_vl; _i++) _EC(_pad); \
    } \
    for (const char *_p = _v; *_p; _p++) _EC(*_p); \
    if ((left_align) && (int)_vl < (width)) { \
        for (int _i = 0; _i < (width) - (int)_vl; _i++) _EC(' '); \
    } \
} while (0)

    while (*fmt) {
        if (*fmt != '%') { _EC(*fmt++); continue; }
        fmt++;

        /* ── Parse flags ── */
        int left_align = 0, zero_pad = 0;
        for (;;) {
            if (*fmt == '-') { left_align = 1; zero_pad = 0; fmt++; }
            else if (*fmt == '0' && !left_align) { zero_pad = 1; fmt++; }
            else break;
        }

        /* ── Parse width ── */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        /* ── Parse length modifier ── */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        /* ── Build value string, then emit with padding ── */
        char buf[72];
        const char *val = buf;
        buf[0] = '\0';

        if (*fmt == 'd') {
            int64_t v = is_long ? va_arg(args, int64_t) : (int64_t)va_arg(args, int32_t);
            itoa(v, buf, 10);
        } else if (*fmt == 'u') {
            uint64_t v = is_long ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, uint32_t);
            itoa((int64_t)v, buf, 10);
        } else if (*fmt == 'x') {
            uint64_t v = is_long ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, uint32_t);
            itoa((int64_t)v, buf, 16);
        } else if (*fmt == 'X') {
            uint64_t v = is_long ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, uint32_t);
            itoa((int64_t)v, buf, 16);
            for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
        } else if (*fmt == 'o') {
            uint64_t v = is_long ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, uint32_t);
            itoa((int64_t)v, buf, 8);
        } else if (*fmt == 'f') {
            ftoa(va_arg(args, double), buf, 10, 6);
        } else if (*fmt == 'p') {
            buf[0] = '0'; buf[1] = 'x';
            itoa((int64_t)(uintptr_t)va_arg(args, void *), buf + 2, 16);
        } else if (*fmt == 'c') {
            buf[0] = (char)va_arg(args, int); buf[1] = '\0';
        } else if (*fmt == 's') {
            val = va_arg(args, const char *);
            if (!val) val = "(null)";
        } else if (*fmt == '%') {
            buf[0] = '%'; buf[1] = '\0';
        } else {
            /* Unknown specifier — pass through literally */
            buf[0] = '%'; buf[1] = *fmt; buf[2] = '\0';
        }
        fmt++;

        _EMIT_PADDED(val, width, zero_pad, left_align);
    }

#undef _EC
#undef _EMIT_PADDED

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
