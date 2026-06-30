#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ── Core address types ───────────────────────────────────────────────────── */
typedef struct { uint8_t b[6]; } mac_addr_t;
typedef struct { uint8_t b[4]; } ip_addr_t;

/* ── Protocol constants ──────────────────────────────────────────────────── */
#define ETH_TYPE_IP   0x0800u
#define ETH_TYPE_ARP  0x0806u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_UDP  17u

/* ── Global network configuration ───────────────────────────────────────── */
extern mac_addr_t net_mac;
extern ip_addr_t  net_ip;
extern ip_addr_t  net_mask;
extern ip_addr_t  net_gw;
extern int        net_up;    /* 1 once IP is configured */

/* ── Byte-order helpers (x86 is LE, network is BE) ──────────────────────── */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >>  8)
         | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── Address helpers ─────────────────────────────────────────────────────── */
static inline int mac_eq(const mac_addr_t *a, const mac_addr_t *b) {
    for (int i = 0; i < 6; i++) if (a->b[i] != b->b[i]) return 0;
    return 1;
}
static inline int ip_eq(const ip_addr_t *a, const ip_addr_t *b) {
    return a->b[0] == b->b[0] && a->b[1] == b->b[1]
        && a->b[2] == b->b[2] && a->b[3] == b->b[3];
}
static inline int ip_is_any(const ip_addr_t *a) {
    return !a->b[0] && !a->b[1] && !a->b[2] && !a->b[3];
}
static inline int ip_is_bcast(const ip_addr_t *a) {
    return a->b[0]==0xFF && a->b[1]==0xFF && a->b[2]==0xFF && a->b[3]==0xFF;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Build and send one raw Ethernet frame via e1000 */
void eth_send(const mac_addr_t *dst, uint16_t etype,
              const void *payload, uint16_t len);

/* Receive one pending frame (if any) and dispatch through the stack */
void net_poll(void);

/* Parse "a.b.c.d" string → ip_addr_t; returns 0 on success */
int net_parse_ip(const char *s, ip_addr_t *out);

/* Initialise NIC, run DHCP, print result */
void net_init(void);

#endif
