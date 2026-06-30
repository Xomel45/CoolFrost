#include "arp.h"
#include "net.h"
#include "../libc/mem.h"
#include "../cpu/timer.h"

#define ARP_CACHE_SIZE 16

/* ── ARP cache ───────────────────────────────────────────────────────────── */
typedef struct {
    ip_addr_t  ip;
    mac_addr_t mac;
    int        valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_next = 0;

/* ── Cache operations ─────────────────────────────────────────────────────── */

mac_addr_t *arp_lookup(const ip_addr_t *ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(&arp_cache[i].ip, ip))
            return &arp_cache[i].mac;
    }
    return 0;
}

static void arp_insert(const ip_addr_t *ip, const mac_addr_t *mac) {
    /* Overwrite existing entry if present */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_eq(&arp_cache[i].ip, ip)) {
            arp_cache[i].mac = *mac;
            return;
        }
    }
    /* Evict oldest entry (round-robin) */
    arp_cache[arp_cache_next].ip    = *ip;
    arp_cache[arp_cache_next].mac   = *mac;
    arp_cache[arp_cache_next].valid = 1;
    arp_cache_next = (arp_cache_next + 1) % ARP_CACHE_SIZE;
}

/* ── Send ARP request for `target_ip` ───────────────────────────────────── */
static void arp_send_request(const ip_addr_t *target_ip) {
    arp_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hw_type    = htons(1);
    pkt.proto_type = htons(0x0800);
    pkt.hw_len     = 6;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(1);
    pkt.sender_mac = net_mac;
    pkt.sender_ip  = net_ip;
    /* target_mac stays all-zero for a request */
    pkt.target_ip  = *target_ip;

    mac_addr_t bcast = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};
    eth_send(&bcast, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

/* ── Handle incoming ARP frame ───────────────────────────────────────────── */
void arp_handle(const mac_addr_t *src_mac, const uint8_t *payload, int len) {
    if (len < (int)sizeof(arp_pkt_t)) return;
    const arp_pkt_t *p = (const arp_pkt_t *)payload;

    if (ntohs(p->hw_type)    != 1)      return;
    if (ntohs(p->proto_type) != 0x0800) return;

    /* Learn sender's IP→MAC mapping */
    arp_insert(&p->sender_ip, &p->sender_mac);
    (void)src_mac;

    /* If it's a request for our IP, reply */
    if (ntohs(p->opcode) == 1 && !ip_is_any(&net_ip)
        && ip_eq(&p->target_ip, &net_ip)) {
        arp_pkt_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.hw_type    = htons(1);
        reply.proto_type = htons(0x0800);
        reply.hw_len     = 6;
        reply.proto_len  = 4;
        reply.opcode     = htons(2);
        reply.sender_mac = net_mac;
        reply.sender_ip  = net_ip;
        reply.target_mac = p->sender_mac;
        reply.target_ip  = p->sender_ip;
        eth_send(&p->sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}

/* ── Resolve IP → MAC with timeout ──────────────────────────────────────── */
int arp_resolve(const ip_addr_t *ip, mac_addr_t *out) {
    /* Fast path: already in cache */
    mac_addr_t *cached = arp_lookup(ip);
    if (cached) { *out = *cached; return 0; }

    /* Send ARP request and poll for up to 1000 ms */
    arp_send_request(ip);
    uint64_t deadline = get_tick() + 1000;
    while (get_tick() < deadline) {
        net_poll();
        cached = arp_lookup(ip);
        if (cached) { *out = *cached; return 0; }
    }
    return -1;
}
