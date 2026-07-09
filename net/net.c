#include "net.h"
#include "arp.h"
#include "ip.h"
#include "dhcp.h"
#include "../drivers/e1000.h"
#include "../drivers/screen.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

/* ── Global network configuration ───────────────────────────────────────── */
mac_addr_t net_mac  = {{0}};
ip_addr_t  net_ip   = {{0}};
ip_addr_t  net_mask = {{0}};
ip_addr_t  net_gw   = {{0}};
int        net_up   = 0;

/* ── Static frame buffers ────────────────────────────────────────────────── */
static uint8_t eth_rx_frame[1518];
static uint8_t eth_tx_frame[1518];

/* ── eth_send: build Ethernet frame and call e1000_send ─────────────────── */
void eth_send(const mac_addr_t *dst, uint16_t etype,
              const void *payload, uint16_t plen) {
    if (plen > 1500u) return;

    /* Destination MAC */
    memcpy(eth_tx_frame, (uint8_t *)dst->b,     6u);
    /* Source MAC */
    memcpy(eth_tx_frame + 6, (uint8_t *)net_mac.b, 6u);
    /* Ethertype (big-endian) */
    eth_tx_frame[12] = (uint8_t)(etype >> 8);
    eth_tx_frame[13] = (uint8_t)(etype);
    /* Payload */
    memcpy(eth_tx_frame + 14, (uint8_t *)payload, plen);

    e1000_send(eth_tx_frame, (uint16_t)(14u + plen));
}

/* ── net_poll: receive one frame and dispatch it ─────────────────────────── */
void net_poll(void) {
    int len = e1000_recv(eth_rx_frame, sizeof(eth_rx_frame));
    if (len < 14) return;

    /* Check destination MAC (accept our MAC and broadcast) */
    mac_addr_t dst;
    memcpy((uint8_t *)dst.b, eth_rx_frame, 6u);
    mac_addr_t bcast = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};
    if (!mac_eq(&dst, &net_mac) && !mac_eq(&dst, &bcast)) return;

    mac_addr_t src;
    memcpy((uint8_t *)src.b, eth_rx_frame + 6, 6u);

    uint16_t etype = (uint16_t)(((uint16_t)eth_rx_frame[12] << 8) | eth_rx_frame[13]);
    const uint8_t *payload = eth_rx_frame + 14;
    int plen = len - 14;

    switch (etype) {
        case ETH_TYPE_ARP: arp_handle(&src, payload, plen); break;
        case ETH_TYPE_IP:  ip_recv(payload, plen);          break;
    }
}

/* ── net_parse_ip: "a.b.c.d" → ip_addr_t ────────────────────────────────── */
int net_parse_ip(const char *s, ip_addr_t *out) {
    int octet = 0, idx = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (*s - '0');
            if (octet > 255) return -1;
        } else if (*s == '.' && idx < 3) {
            out->b[idx++] = (uint8_t)octet;
            octet = 0;
        } else {
            break;
        }
    }
    if (idx != 3) return -1;
    out->b[3] = (uint8_t)octet;
    return 0;
}

/* ── net_init: initialise NIC and obtain IP via DHCP ────────────────────── */
void net_init(void) {
    if (e1000_init() != 0) {
        kprint("net: no supported NIC found\n");
        return;
    }

    kprint("net: running DHCP...\n");
    if (dhcp_discover() == 0) {
        printf("net: %d.%d.%d.%d / %d.%d.%d.%d  gw %d.%d.%d.%d\n",
               net_ip.b[0],   net_ip.b[1],   net_ip.b[2],   net_ip.b[3],
               net_mask.b[0], net_mask.b[1], net_mask.b[2], net_mask.b[3],
               net_gw.b[0],   net_gw.b[1],   net_gw.b[2],   net_gw.b[3]);
        net_up = 1;
    } else {
        kprint("net: DHCP timeout — no IP assigned\n");
    }
}
