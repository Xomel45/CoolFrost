#ifndef ARP_H
#define ARP_H

#include "net.h"

/* ── ARP packet (Ethernet/IPv4, 28 bytes) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t  hw_type;    /* 0x0001 = Ethernet */
    uint16_t  proto_type; /* 0x0800 = IPv4 */
    uint8_t   hw_len;     /* 6 */
    uint8_t   proto_len;  /* 4 */
    uint16_t  opcode;     /* 1 = request, 2 = reply */
    mac_addr_t sender_mac;
    ip_addr_t  sender_ip;
    mac_addr_t target_mac;
    ip_addr_t  target_ip;
} arp_pkt_t;

/* Look up `ip` in ARP cache.  Returns pointer to cached MAC or NULL. */
mac_addr_t *arp_lookup(const ip_addr_t *ip);

/* Handle an incoming ARP frame (raw payload after Ethernet header). */
void arp_handle(const mac_addr_t *src_mac, const uint8_t *payload, int len);

/* Resolve `ip` to its MAC address.
 * Sends an ARP request and polls until a reply arrives (≤1000 ms).
 * Returns 0 on success and writes the MAC into *out; returns -1 on timeout. */
int arp_resolve(const ip_addr_t *ip, mac_addr_t *out);

#endif
