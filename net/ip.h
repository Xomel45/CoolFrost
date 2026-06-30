#ifndef IP_H
#define IP_H

#include "net.h"

/* ── IPv4 header (20 bytes, no options) ─────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t   version_ihl;   /* (4 << 4) | 5 */
    uint8_t   tos;
    uint16_t  total_len;     /* big-endian, includes header */
    uint16_t  id;            /* big-endian */
    uint16_t  flags_frag;    /* big-endian: 0x4000 = DF */
    uint8_t   ttl;
    uint8_t   protocol;      /* IP_PROTO_ICMP or IP_PROTO_UDP */
    uint16_t  checksum;      /* over IP header only */
    ip_addr_t src;
    ip_addr_t dst;
} ip_hdr_t;

/* Compute 16-bit ones-complement checksum over `len` bytes of `data`. */
uint16_t ip_checksum(const void *data, int len);

/* Build and send an IPv4 packet (resolves destination MAC via ARP). */
void ip_send(const ip_addr_t *dst, uint8_t proto,
             const void *payload, uint16_t len);

/* Dispatch an incoming IPv4 payload (called from net.c dispatcher). */
void ip_recv(const uint8_t *frame_payload, int len);

#endif
