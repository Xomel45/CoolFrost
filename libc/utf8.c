#include "utf8.h"

/* ── utf8_decode ─────────────────────────────────────────────────────────── *
 * Decodes one code point from *src and advances the pointer.               *
 * Always advances at least 1 byte to prevent infinite loops on bad input.  */
rune_t utf8_decode(const char **src) {
    const unsigned char *s = (const unsigned char *)*src;

    if (!*s) return RUNE_EOF;

    rune_t cp;
    int    extra;
    unsigned char lead = *s++;

    if (lead < 0x80u) {
        /* Single byte: 0xxxxxxx */
        *src = (const char *)s;
        return (rune_t)lead;
    } else if ((lead & 0xE0u) == 0xC0u) {
        cp = lead & 0x1Fu; extra = 1;
    } else if ((lead & 0xF0u) == 0xE0u) {
        cp = lead & 0x0Fu; extra = 2;
    } else if ((lead & 0xF8u) == 0xF0u) {
        cp = lead & 0x07u; extra = 3;
    } else {
        /* Invalid lead byte */
        *src = (const char *)s;
        return RUNE_ERROR;
    }

    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0u) != 0x80u) {
            *src = (const char *)s;
            return RUNE_ERROR;
        }
        cp = (cp << 6) | (*s++ & 0x3Fu);
    }

    *src = (const char *)s;

    /* Reject overlong encodings */
    if (extra == 1 && cp < 0x80u)    return RUNE_ERROR;
    if (extra == 2 && cp < 0x800u)   return RUNE_ERROR;
    if (extra == 3 && cp < 0x10000u) return RUNE_ERROR;

    /* Reject surrogates and values > RUNE_MAX */
    if (cp >= 0xD800u && cp <= 0xDFFFu) return RUNE_ERROR;
    if (cp > RUNE_MAX)                   return RUNE_ERROR;

    return cp;
}

/* ── utf8_strlen ─────────────────────────────────────────────────────────── */
size_t utf8_strlen(const char *s) {
    size_t n = 0;
    /* Count bytes that are NOT continuation bytes (10xxxxxx). */
    while (*s) {
        if (!utf8_is_cont((unsigned char)*s)) n++;
        s++;
    }
    return n;
}

/* ── utf8_advance ────────────────────────────────────────────────────────── */
const char *utf8_advance(const char *s, size_t n) {
    while (*s && n > 0) {
        utf8_decode(&s);
        n--;
    }
    return s;
}

/* ── utf8_codepoint_offset ───────────────────────────────────────────────── */
size_t utf8_codepoint_offset(const char *s, size_t n) {
    const char *start = s;
    s = utf8_advance(s, n);
    return (size_t)(s - start);
}

/* ── utf8_valid ──────────────────────────────────────────────────────────── */
int utf8_valid(const char *s, size_t len) {
    const char *end = s + len;
    int bounded = (len != (size_t)-1);

    while (*s && (!bounded || s < end)) {
        rune_t cp = utf8_decode(&s);
        if (cp == RUNE_ERROR) return 0;
    }
    return 1;
}

/* ── utf8_casecmp ────────────────────────────────────────────────────────── *
 * Case-insensitive compare of two UTF-8 strings.                           *
 * Case folding: ASCII letters only (A-Z → a-z).                           */
int utf8_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        rune_t ca = utf8_decode(&a);
        rune_t cb = utf8_decode(&b);
        ca = rune_to_lower(ca);
        cb = rune_to_lower(cb);
        if (ca != cb) {
            if (ca < cb) return -1;
            return 1;
        }
    }
    /* Handle different lengths */
    rune_t ca = (rune_t)(unsigned char)*a;
    rune_t cb = (rune_t)(unsigned char)*b;
    if (ca == cb) return 0;
    return (ca < cb) ? -1 : 1;
}

/* ── utf8_strchr ─────────────────────────────────────────────────────────── */
const char *utf8_strchr(const char *s, rune_t cp) {
    while (*s) {
        const char *prev = s;
        rune_t c = utf8_decode(&s);
        if (c == cp) return prev;
    }
    /* Special case: looking for the null terminator */
    if (cp == 0) return s;
    return (const char *)0;
}

/* ── utf8_strncpy_cp ─────────────────────────────────────────────────────── *
 * Copy at most n codepoints of src into dst, leaving room for '\0'.        */
size_t utf8_strncpy_cp(char *dst, size_t buf_sz, const char *src, size_t n) {
    if (!buf_sz) return 0;
    size_t copied = 0;
    char  *end    = dst + buf_sz - 1;   /* leave 1 byte for '\0' */

    while (*src && copied < n) {
        const char *prev = src;
        rune_t cp = utf8_decode(&src);
        int    blen = utf8_byte_len(cp);

        if (dst + blen > end) break;   /* not enough space */

        /* Re-encode directly into dst */
        utf8_encode(cp, dst);
        dst   += blen;
        copied++;
        (void)prev;
    }
    *dst = '\0';
    return copied;
}
