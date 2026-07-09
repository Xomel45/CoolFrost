#ifndef UTF8_H
#define UTF8_H

#include <stdint.h>
#include <stddef.h>

/* ── Unicode code point type ────────────────────────────────────────────── */
typedef uint32_t rune_t;

#define RUNE_ERROR  0xFFFDu         /* Unicode replacement character */
#define RUNE_EOF    ((rune_t)(-1))  /* returned at end of string     */
#define RUNE_MAX    0x10FFFFu

/* ── Encoding helpers (inline, no malloc) ──────────────────────────────── */

/* Bytes needed to encode code point cp (0 = invalid). */
static inline int utf8_byte_len(rune_t cp) {
    if (cp < 0x80u)    return 1;
    if (cp < 0x800u)   return 2;
    if (cp < 0x10000u) return (cp >= 0xD800u && cp <= 0xDFFFu) ? 0 : 3;
    return (cp <= RUNE_MAX) ? 4 : 0;
}

/* 1 if byte is a UTF-8 continuation byte (10xxxxxx). */
static inline int utf8_is_cont(unsigned char b) { return (b & 0xC0u) == 0x80u; }

/* 1 if byte is the start of a multi-byte sequence (11xxxxxx). */
static inline int utf8_is_lead(unsigned char b) { return (b & 0xC0u) == 0xC0u; }

/* Encode cp into dst (must have ≥4 bytes). Returns bytes written. */
static inline int utf8_encode(rune_t cp, char *dst) {
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        /* Encode surrogates as RUNE_ERROR */
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            dst[0] = (char)0xEFu; dst[1] = (char)0xBFu; dst[2] = (char)0xBDu;
            return 3;
        }
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= RUNE_MAX) {
        dst[0] = (char)(0xF0u | (cp >> 18));
        dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    /* Invalid: encode as replacement character */
    dst[0] = (char)0xEFu; dst[1] = (char)0xBFu; dst[2] = (char)0xBDu;
    return 3;
}

/* ── Decode / iterate ───────────────────────────────────────────────────── */

/* Decode one code point from *src, advance *src past the sequence.
 * Returns RUNE_ERROR for invalid sequences, RUNE_EOF at '\0'.             */
rune_t      utf8_decode(const char **src);

/* Number of code points in null-terminated UTF-8 string (O(n) bytes). */
size_t      utf8_strlen(const char *s);

/* Advance pointer by n codepoints; stops at '\0'. */
const char *utf8_advance(const char *s, size_t n);

/* Byte offset of the nth codepoint from s (does not include terminator). */
size_t      utf8_codepoint_offset(const char *s, size_t n);

/* Validate s[0..len): 1 = valid, 0 = invalid.
 * Pass len=(size_t)-1 to scan until '\0'.                                 */
int         utf8_valid(const char *s, size_t len);

/* Case-insensitive string compare (ASCII fold only). */
int         utf8_casecmp(const char *a, const char *b);

/* First occurrence of codepoint cp in s; NULL if not found. */
const char *utf8_strchr(const char *s, rune_t cp);

/* Copy at most n codepoints of src into dst (buf_sz byte limit incl. NUL).
 * Returns number of codepoints copied.                                    */
size_t      utf8_strncpy_cp(char *dst, size_t buf_sz, const char *src, size_t n);

/* ── Rune predicates (inline) ───────────────────────────────────────────── */
static inline int   rune_is_ascii (rune_t c) { return c < 0x80u; }
static inline int   rune_is_digit (rune_t c) { return c >= '0' && c <= '9'; }
static inline int   rune_is_upper (rune_t c) { return c >= 'A' && c <= 'Z'; }
static inline int   rune_is_lower (rune_t c) { return c >= 'a' && c <= 'z'; }
static inline int   rune_is_alpha (rune_t c) { return rune_is_upper(c) || rune_is_lower(c); }
static inline int   rune_is_alnum (rune_t c) { return rune_is_alpha(c) || rune_is_digit(c); }
static inline int   rune_is_space (rune_t c) {
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v';
}
static inline int   rune_is_print (rune_t c) {
    return (c >= 0x20u && c < 0x7Fu) || c > 0x9Fu;
}
static inline int   rune_is_hex   (rune_t c) {
    return rune_is_digit(c) || (c>='A'&&c<='F') || (c>='a'&&c<='f');
}
static inline rune_t rune_to_upper(rune_t c) { return rune_is_lower(c) ? c-32u : c; }
static inline rune_t rune_to_lower(rune_t c) { return rune_is_upper(c) ? c+32u : c; }
static inline int    rune_hex_val (rune_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    return -1;
}

#endif
