#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "../libc/mem.h"
#include "../cpu/timer.h"

#define DHCP_XID    0xABCD1234u
#define DHCP_PORT_C 68u   /* client port */
#define DHCP_PORT_S 67u   /* server port */

/* DHCP message types */
#define DHCPDISCOVER 1u
#define DHCPOFFER    2u
#define DHCPREQUEST  3u
#define DHCPACK      5u

/* Shared receive/send buffer */
static dhcp_msg_t dhcp_pkt;

/* State set by the receive callback */
static volatile int      dhcp_got_offer = 0;
static volatile int      dhcp_got_ack   = 0;
static volatile ip_addr_t dhcp_offered_ip;
static volatile ip_addr_t dhcp_server_ip;
static volatile ip_addr_t dhcp_offered_mask;
static volatile ip_addr_t dhcp_offered_gw;

/* ── Parse DHCP options ──────────────────────────────────────────────────── */
static uint8_t dhcp_option_type(const uint8_t *opts, int olen) {
    int i = 4; /* skip magic cookie */
    while (i < olen && opts[i] != 0xFF) {
        if (opts[i] == 0) { i++; continue; }
        uint8_t code = opts[i];
        uint8_t len  = opts[i + 1];
        if (code == 53 && len == 1) return opts[i + 2];
        i += 2 + len;
    }
    return 0;
}

static void dhcp_parse_options(const uint8_t *opts, int olen) {
    int i = 4;
    while (i < olen && opts[i] != 0xFF) {
        if (opts[i] == 0) { i++; continue; }
        uint8_t code = opts[i];
        uint8_t len  = opts[i + 1];
        const uint8_t *val = opts + i + 2;
        if (code == 1 && len == 4)   /* subnet mask */
            memcpy((uint8_t *)&dhcp_offered_mask, (uint8_t *)val, 4);
        if (code == 3 && len >= 4)   /* router / default gateway */
            memcpy((uint8_t *)&dhcp_offered_gw, (uint8_t *)val, 4);
        if (code == 54 && len == 4)  /* server identifier */
            memcpy((uint8_t *)&dhcp_server_ip, (uint8_t *)val, 4);
        i += 2 + len;
    }
}

/* ── UDP receive callback on port 68 ────────────────────────────────────── */
static void dhcp_recv(const ip_addr_t *src_ip, uint16_t src_port,
                      const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)src_port;
    if (len < sizeof(dhcp_msg_t)) return;
    const dhcp_msg_t *msg = (const dhcp_msg_t *)data;

    /* Verify XID and BOOTREPLY */
    if (ntohl(msg->xid) != DHCP_XID) return;
    if (msg->op != 2) return;

    /* Verify magic cookie: 99.130.83.99 = 0x63825363 */
    if (msg->options[0] != 0x63 || msg->options[1] != 0x82 ||
        msg->options[2] != 0x53 || msg->options[3] != 0x63) return;

    uint8_t mtype = dhcp_option_type(msg->options, 312);
    dhcp_parse_options(msg->options, 312);

    if (mtype == DHCPOFFER) {
        /* Save offered IP */
        memcpy((uint8_t *)&dhcp_offered_ip, (uint8_t *)msg->yiaddr.b, 4);
        dhcp_got_offer = 1;
    } else if (mtype == DHCPACK) {
        memcpy((uint8_t *)&dhcp_offered_ip, (uint8_t *)msg->yiaddr.b, 4);
        dhcp_got_ack = 1;
    }
}

/* ── Build and send a DHCP Discover or Request ───────────────────────────── */
static void dhcp_send(uint8_t msg_type) {
    memset(&dhcp_pkt, 0, sizeof(dhcp_pkt));

    dhcp_pkt.op    = 1;          /* BOOTREQUEST */
    dhcp_pkt.htype = 1;          /* Ethernet */
    dhcp_pkt.hlen  = 6;
    dhcp_pkt.xid   = htonl(DHCP_XID);
    dhcp_pkt.flags = htons(0x8000u); /* broadcast flag */

    /* Client hardware address (our MAC) */
    memcpy(dhcp_pkt.chaddr, (uint8_t *)net_mac.b, 6);

    /* Options: magic cookie */
    uint8_t *o = dhcp_pkt.options;
    o[0] = 0x63; o[1] = 0x82; o[2] = 0x53; o[3] = 0x63;
    int oi = 4;

    /* Option 53: DHCP Message Type */
    o[oi++] = 53; o[oi++] = 1; o[oi++] = msg_type;

    if (msg_type == DHCPDISCOVER) {
        /* Option 55: Parameter Request List */
        o[oi++] = 55; o[oi++] = 3;
        o[oi++] = 1;  /* subnet mask */
        o[oi++] = 3;  /* router */
        o[oi++] = 6;  /* DNS */

        /* Option 61: Client Identifier */
        o[oi++] = 61; o[oi++] = 7; o[oi++] = 1;
        memcpy(o + oi, (uint8_t *)net_mac.b, 6); oi += 6;

    } else if (msg_type == DHCPREQUEST) {
        /* Option 50: Requested IP Address */
        o[oi++] = 50; o[oi++] = 4;
        memcpy(o + oi, (uint8_t *)&dhcp_offered_ip, 4); oi += 4;

        /* Option 54: Server Identifier */
        o[oi++] = 54; o[oi++] = 4;
        memcpy(o + oi, (uint8_t *)&dhcp_server_ip, 4); oi += 4;
    }

    o[oi++] = 0xFF; /* End option */

    ip_addr_t bcast = {{255,255,255,255}};
    udp_send(&bcast, DHCP_PORT_C, DHCP_PORT_S, &dhcp_pkt, sizeof(dhcp_pkt));
}

/* ── Public: full DHCP handshake ─────────────────────────────────────────── */
int dhcp_discover(void) {
    udp_register(DHCP_PORT_C, dhcp_recv);

    /* Zero out volatile state */
    dhcp_got_offer = 0;
    dhcp_got_ack   = 0;

    /* ── Phase 1: Discover → Offer ── */
    dhcp_send(DHCPDISCOVER);
    uint64_t deadline = get_tick() + 3000;
    while (get_tick() < deadline) {
        net_poll();
        if (dhcp_got_offer) break;
    }
    if (!dhcp_got_offer) return -1;

    /* ── Phase 2: Request → Ack ── */
    dhcp_send(DHCPREQUEST);
    deadline = get_tick() + 3000;
    while (get_tick() < deadline) {
        net_poll();
        if (dhcp_got_ack) break;
    }
    if (!dhcp_got_ack) return -1;

    /* Apply configuration */
    memcpy((uint8_t *)net_ip.b, (uint8_t *)&dhcp_offered_ip,   4);
    memcpy((uint8_t *)net_mask.b, (uint8_t *)&dhcp_offered_mask, 4);
    memcpy((uint8_t *)net_gw.b, (uint8_t *)&dhcp_offered_gw,   4);

    return 0;
}
