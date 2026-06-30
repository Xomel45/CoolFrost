#include "format.h"
#include "stdio.h"

/* ── hex_dump ───────────────────────────────────────────────────────────── */

static const char _hx[] = "0123456789abcdef";

static void _print_hex_byte(uint8_t b) {
    char s[3] = { _hx[b >> 4], _hx[b & 0xf], '\0' };
    printf("%s", s);
}

void hex_dump(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i += 16) {
        /* Offset */
        char off[12];
        snprintf(off, sizeof(off), "%08lx", (uint64_t)i);
        printf("%s  ", off);

        /* Hex bytes */
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) _print_hex_byte(p[i + j]);
            else              printf("  ");
            printf(" ");
            if (j == 7) printf(" ");
        }

        /* ASCII */
        printf("|");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            char cs[2] = { (c >= 0x20 && c < 0x7f) ? (char)c : '.', '\0' };
            printf("%s", cs);
        }
        printf("|\n");
    }
}

/* ── fmt_size ───────────────────────────────────────────────────────────── */

void fmt_size(uint64_t bytes, char *out, size_t sz) {
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    uint64_t v = bytes;
    while (v >= 1024 && u < 4) { v /= 1024; u++; }
    if (u == 0) {
        snprintf(out, sz, "%lu B", bytes);
        return;
    }
    /* Compute integer and two-decimal-digit fractional parts */
    uint64_t whole = bytes;
    for (int i = 0; i < u; i++) whole /= 1024;
    uint64_t rem = bytes;
    for (int i = 0; i < u - 1; i++) rem /= 1024;
    rem = ((rem % 1024) * 100u) / 1024u;
    snprintf(out, sz, "%lu.%02lu %s", whole, rem, units[u]);
}

/* ── fmt_ip4 ────────────────────────────────────────────────────────────── */

void fmt_ip4(uint32_t ip_be, char *out, size_t sz) {
    uint8_t *b = (uint8_t *)&ip_be;
    snprintf(out, sz, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

/* ── fmt_mac ────────────────────────────────────────────────────────────── */

void fmt_mac(const uint8_t mac[6], char *out, size_t sz) {
    if (sz < 18) return;
    const char *h = "0123456789abcdef";
    char *p = out;
    for (int i = 0; i < 6; i++) {
        *p++ = h[mac[i] >> 4];
        *p++ = h[mac[i] & 0xf];
        *p++ = (i < 5) ? ':' : '\0';
    }
    (void)sz;
}

/* ── fmt_uptime ─────────────────────────────────────────────────────────── */

void fmt_uptime(uint64_t ms, char *out, size_t sz) {
    uint64_t s   = ms / 1000;
    uint64_t min = s / 60; s %= 60;
    uint64_t hr  = min / 60; min %= 60;
    uint64_t day = hr / 24; hr %= 24;
    if (day)
        snprintf(out, sz, "%lud %02luh %02lum %02lus", day, hr, min, s);
    else if (hr)
        snprintf(out, sz, "%luh %02lum %02lus", hr, min, s);
    else
        snprintf(out, sz, "%lum %02lus", min, s);
}

/* ── fmt_hexstr ─────────────────────────────────────────────────────────── */

void fmt_hexstr(const uint8_t *buf, size_t len, char *out, size_t sz) {
    const char *h = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len && (i * 2 + 2) < sz; i++) {
        out[i*2]     = h[buf[i] >> 4];
        out[i*2 + 1] = h[buf[i] & 0xf];
    }
    out[i * 2] = '\0';
}
