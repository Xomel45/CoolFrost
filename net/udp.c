#include "udp.h"
#include "ip.h"
#include "../libc/mem.h"

#define MAX_UDP_HANDLERS 8

typedef struct {
    uint16_t      port;
    udp_handler_t fn;
    int           used;
} udp_reg_t;

static udp_reg_t handlers[MAX_UDP_HANDLERS];

/* ── Register a handler for a specific port ──────────────────────────────── */
void udp_register(uint16_t port, udp_handler_t fn) {
    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (handlers[i].used && handlers[i].port == port) {
            handlers[i].fn = fn;
            return;
        }
    }
    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (!handlers[i].used) {
            handlers[i].port = port;
            handlers[i].fn   = fn;
            handlers[i].used = 1;
            return;
        }
    }
}

/* ── Send a UDP datagram ─────────────────────────────────────────────────── */
void udp_send(const ip_addr_t *dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t dlen) {
    static uint8_t udp_tx[1480];
    if (dlen > (uint16_t)(sizeof(udp_tx) - 8)) return;

    udp_hdr_t *hdr = (udp_hdr_t *)udp_tx;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)(8 + dlen));
    hdr->checksum = 0; /* skip checksum for IPv4 UDP */

    memcpy(udp_tx + 8, (uint8_t *)data, dlen);
    ip_send(dst_ip, IP_PROTO_UDP, udp_tx, (uint16_t)(8 + dlen));
}

/* ── Dispatch incoming UDP payload ───────────────────────────────────────── */
void udp_handle(const ip_addr_t *src_ip, const uint8_t *payload, int len) {
    if (len < 8) return;
    const udp_hdr_t *hdr = (const udp_hdr_t *)payload;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t ulen     = ntohs(hdr->length);
    if (ulen < 8) return;
    uint16_t dlen = (uint16_t)(ulen - 8);
    if (dlen > (uint16_t)(len - 8)) dlen = (uint16_t)(len - 8);

    for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
        if (handlers[i].used && handlers[i].port == dst_port && handlers[i].fn) {
            handlers[i].fn(src_ip, src_port, payload + 8, dlen);
            return;
        }
    }
}
