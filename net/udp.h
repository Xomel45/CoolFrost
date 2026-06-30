#ifndef UDP_H
#define UDP_H

#include "net.h"

/* ── UDP header (8 bytes) ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* UDP header + data length */
    uint16_t checksum;  /* 0 = not computed (valid for IPv4) */
} udp_hdr_t;

typedef void (*udp_handler_t)(const ip_addr_t *src_ip, uint16_t src_port,
                               const uint8_t *data, uint16_t len);

/* Register a callback for incoming UDP packets on `port`.
 * Only one handler per port; a second call overwrites the first. */
void udp_register(uint16_t port, udp_handler_t fn);

/* Send a UDP datagram. */
void udp_send(const ip_addr_t *dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t len);

/* Dispatch an incoming UDP frame payload (called from ip_recv). */
void udp_handle(const ip_addr_t *src_ip, const uint8_t *payload, int len);

#endif
