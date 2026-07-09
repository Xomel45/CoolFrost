#include "ip.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "../libc/mem.h"

/* ── Static Tx buffer ────────────────────────────────────────────────────── */
static uint8_t ip_tx_buf[1500];

/* ── Ones-complement checksum ────────────────────────────────────────────── */
uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Send IPv4 packet ─────────────────────────────────────────────────────── */
void ip_send(const ip_addr_t *dst, uint8_t proto,
             const void *payload, uint16_t plen) {
    if (plen > (uint16_t)(sizeof(ip_tx_buf) - 20)) return;

    /* Determine destination MAC */
    mac_addr_t dst_mac;

    if (ip_is_bcast(dst)) {
        /* Directed broadcast */
        dst_mac = (mac_addr_t){{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};
    } else if (ip_is_any(&net_ip)) {
        /* No IP yet (DHCP phase) — only allow broadcasts */
        return;
    } else {
        /* Check if on-subnet; if not, route via gateway */
        int on_subnet = 1;
        for (int i = 0; i < 4; i++) {
            if ((dst->b[i] & net_mask.b[i]) != (net_ip.b[i] & net_mask.b[i])) {
                on_subnet = 0; break;
            }
        }
        const ip_addr_t *next_hop = on_subnet ? dst : &net_gw;
        if (arp_resolve(next_hop, &dst_mac) != 0) return;
    }

    /* Build IP header */
    ip_hdr_t *hdr = (ip_hdr_t *)ip_tx_buf;
    memset(hdr, 0, 20);
    hdr->version_ihl = (4u << 4) | 5u;
    hdr->tos         = 0;
    hdr->total_len   = htons((uint16_t)(20 + plen));
    hdr->id          = 0;
    hdr->flags_frag  = htons(0x4000u); /* DF */
    hdr->ttl         = 64;
    hdr->protocol    = proto;
    hdr->checksum    = 0;
    hdr->src         = net_ip;
    hdr->dst         = *dst;
    hdr->checksum    = ip_checksum(hdr, 20);

    /* Copy payload */
    memcpy(ip_tx_buf + 20, (uint8_t *)payload, plen);

    eth_send(&dst_mac, ETH_TYPE_IP, ip_tx_buf, (uint16_t)(20 + plen));
}

/* ── Receive and dispatch an IPv4 frame payload ──────────────────────────── */
void ip_recv(const uint8_t *payload, int len) {
    if (len < 20) return;
    const ip_hdr_t *hdr = (const ip_hdr_t *)payload;

    /* Accept only IPv4 with no options (IHL=5) */
    if ((hdr->version_ihl >> 4) != 4) return;
    int ihl = (hdr->version_ihl & 0xFu) * 4;
    if (ihl < 20 || ihl > len) return;

    /* Accept packets addressed to us or broadcast */
    if (!ip_eq(&hdr->dst, &net_ip) && !ip_is_bcast(&hdr->dst)) return;

    const uint8_t *data = payload + ihl;
    int dlen = ntohs(hdr->total_len) - ihl;
    if (dlen < 0 || dlen > len - ihl) dlen = len - ihl;

    switch (hdr->protocol) {
        case IP_PROTO_ICMP: icmp_handle(&hdr->src, data, dlen); break;
        case IP_PROTO_UDP:  udp_handle(&hdr->src, data, dlen);  break;
    }
}
