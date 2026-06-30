#include "icmp.h"
#include "ip.h"
#include "../libc/mem.h"
#include "../cpu/timer.h"

#define ICMP_PING_ID   0x5A5Au
#define ICMP_DATA_LEN  32

/* ── Pending ping state (updated by icmp_handle while icmp_ping polls) ──── */
static volatile int      ping_waiting = 0;
static volatile int      ping_got     = 0;
static volatile uint16_t ping_seq_cur = 0;

/* ── Handle incoming ICMP payload ────────────────────────────────────────── */
void icmp_handle(const ip_addr_t *src_ip, const uint8_t *payload, int len) {
    if (len < 8) return;
    uint8_t type = payload[0];

    if (type == 0) {
        /* Echo Reply */
        uint16_t id  = (uint16_t)((payload[4] << 8) | payload[5]);
        uint16_t seq = (uint16_t)((payload[6] << 8) | payload[7]);
        if (ping_waiting && id == ICMP_PING_ID && seq == ping_seq_cur)
            ping_got = 1;
    } else if (type == 8) {
        /* Echo Request — send reply */
        if (len > 1480) return;
        static uint8_t reply[1480];
        /* Copy incoming ICMP packet (CoolFrost memcpy: source, dest, n) */
        memcpy((uint8_t *)payload, reply, (size_t)len);
        reply[0] = 0;            /* type = Echo Reply */
        reply[2] = 0; reply[3] = 0; /* clear checksum field */
        uint16_t cs = ip_checksum(reply, len);
        reply[2] = (uint8_t)(cs >> 8);
        reply[3] = (uint8_t)(cs);
        ip_send(src_ip, IP_PROTO_ICMP, reply, (uint16_t)len);
    }
}

/* ── Send Echo Request and wait for Echo Reply ───────────────────────────── */
int icmp_ping(const ip_addr_t *dst, uint32_t timeout_ms) {
    static uint16_t seq = 0;
    seq++;
    ping_seq_cur = seq;

    /* Build ICMP Echo Request */
    static uint8_t pkt[8 + ICMP_DATA_LEN];
    pkt[0] = 8;                               /* type: echo request */
    pkt[1] = 0;                               /* code */
    pkt[2] = 0; pkt[3] = 0;                  /* checksum (computed below) */
    pkt[4] = (uint8_t)(ICMP_PING_ID >> 8);
    pkt[5] = (uint8_t)(ICMP_PING_ID);
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)(seq);
    for (int i = 0; i < ICMP_DATA_LEN; i++)
        pkt[8 + i] = (uint8_t)('A' + i % 26);

    uint16_t cs = ip_checksum(pkt, 8 + ICMP_DATA_LEN);
    pkt[2] = (uint8_t)(cs >> 8);
    pkt[3] = (uint8_t)(cs);

    ping_got     = 0;
    ping_waiting = 1;

    uint64_t t0 = get_tick();
    ip_send(dst, IP_PROTO_ICMP, pkt, 8 + ICMP_DATA_LEN);

    uint64_t deadline = t0 + timeout_ms;
    while (get_tick() < deadline) {
        net_poll();
        if (ping_got) {
            ping_waiting = 0;
            return (int)(get_tick() - t0);
        }
    }
    ping_waiting = 0;
    return -1;
}
