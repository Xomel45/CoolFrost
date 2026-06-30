#ifndef ICMP_H
#define ICMP_H

#include "net.h"

/* ── ICMP header (8 bytes) ───────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;      /* 8 = Echo Request, 0 = Echo Reply */
    uint8_t  code;      /* always 0 for echo */
    uint16_t checksum;  /* over ICMP header + data */
    uint16_t id;        /* echo id */
    uint16_t seq;       /* echo sequence number */
} icmp_hdr_t;

/* Handle an incoming ICMP payload (from ip_recv). */
void icmp_handle(const ip_addr_t *src_ip, const uint8_t *payload, int len);

/* Send a single ICMP Echo Request and wait for the Echo Reply.
 * timeout_ms: maximum wait in milliseconds.
 * Returns the round-trip time in ms, or -1 on timeout. */
int icmp_ping(const ip_addr_t *dst, uint32_t timeout_ms);

#endif
