#ifndef FORMAT_H
#define FORMAT_H

#include <stdint.h>
#include <stddef.h>

/* ── Hex dump to screen ─────────────────────────────────────────────────── *
 * Prints buf[0..len) in classic hex+ASCII format, 16 bytes per line.        */
void hex_dump(const void *buf, size_t len);

/* ── Human-readable byte count ──────────────────────────────────────────── *
 * fmt_size(1536, out, sz) → "1.50 KB"                                       */
void fmt_size(uint64_t bytes, char *out, size_t sz);

/* ── IPv4 address (network byte order) ─────────────────────────────────── *
 * fmt_ip4(0xC0A80101, out, sz) → "192.168.1.1"                             */
void fmt_ip4(uint32_t ip_be, char *out, size_t sz);

/* ── MAC address ────────────────────────────────────────────────────────── *
 * fmt_mac(mac, out, sz) → "aa:bb:cc:dd:ee:ff"                              */
void fmt_mac(const uint8_t mac[6], char *out, size_t sz);

/* ── Uptime from milliseconds ───────────────────────────────────────────── *
 * fmt_uptime(3723000, out, sz) → "1h 02m 03s"                              */
void fmt_uptime(uint64_t ms, char *out, size_t sz);

/* ── Zero-padded hex byte string ────────────────────────────────────────── *
 * fmt_hexstr(buf, 4, out, sz) → "deadbeef"  (no spaces, no 0x)             */
void fmt_hexstr(const uint8_t *buf, size_t len, char *out, size_t sz);

#endif
