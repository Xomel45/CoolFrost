#ifndef DHCP_H
#define DHCP_H

#include "net.h"

/* ── DHCP message (548 bytes, RFC 2131 minimum) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t   op;           /* 1=request, 2=reply */
    uint8_t   htype;        /* 1=Ethernet */
    uint8_t   hlen;         /* 6 */
    uint8_t   hops;         /* 0 */
    uint32_t  xid;          /* transaction id */
    uint16_t  secs;
    uint16_t  flags;        /* 0x8000 = broadcast */
    ip_addr_t ciaddr;       /* client IP (0 if unknown) */
    ip_addr_t yiaddr;       /* your (client) IP from server */
    ip_addr_t siaddr;       /* next server IP */
    ip_addr_t giaddr;       /* relay agent IP */
    uint8_t   chaddr[16];   /* client MAC, padded to 16 */
    uint8_t   sname[64];    /* server hostname */
    uint8_t   file[128];    /* boot filename */
    uint8_t   options[312]; /* magic cookie (4) + DHCP options */
} dhcp_msg_t;               /* total: 548 bytes */

/* Perform DHCP Discover → Offer → Request → Ack.
 * On success sets net_ip, net_mask, net_gw and returns 0.
 * Returns -1 on timeout. */
int dhcp_discover(void);

#endif
