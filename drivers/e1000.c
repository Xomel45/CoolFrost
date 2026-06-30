#include "e1000.h"
#include "pci.h"
#include "../net/net.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

#include <stdint.h>

/* ── Register offsets ─────────────────────────────────────────────────────── */
#define E1000_CTRL   0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD   0x0014u
#define E1000_IMC    0x00D8u
#define E1000_RCTL   0x0100u
#define E1000_TCTL   0x0400u
#define E1000_TIPG   0x0410u
#define E1000_RDBAL  0x2800u
#define E1000_RDBAH  0x2804u
#define E1000_RDLEN  0x2808u
#define E1000_RDH    0x2810u
#define E1000_RDT    0x2818u
#define E1000_TDBAL  0x3800u
#define E1000_TDBAH  0x3804u
#define E1000_TDLEN  0x3808u
#define E1000_TDH    0x3810u
#define E1000_TDT    0x3818u
#define E1000_RAL    0x5400u
#define E1000_RAH    0x5404u
#define E1000_MTA    0x5200u  /* 128 × 4-byte entries */

/* ── CTRL register bits ───────────────────────────────────────────────────── */
#define CTRL_SLU  (1u << 6)   /* Set Link Up */
#define CTRL_RST  (1u << 26)  /* Full device reset (self-clearing) */

/* ── RCTL register bits ───────────────────────────────────────────────────── */
#define RCTL_EN     (1u <<  1)  /* Receiver Enable */
#define RCTL_BAM    (1u << 15)  /* Broadcast Accept Mode */
#define RCTL_SECRC  (1u << 26)  /* Strip Ethernet CRC */

/* ── TCTL register bits ───────────────────────────────────────────────────── */
#define TCTL_EN   (1u << 1)   /* Transmit Enable */
#define TCTL_PSP  (1u << 3)   /* Pad Short Packets */

/* ── Tx descriptor command byte ──────────────────────────────────────────── */
#define TX_CMD_EOP   0x01u  /* End of Packet */
#define TX_CMD_IFCS  0x02u  /* Insert FCS/CRC */
#define TX_CMD_RS    0x08u  /* Report Status (sets DD when done) */

/* ── Descriptor status byte ──────────────────────────────────────────────── */
#define DESC_DD  0x01u  /* Descriptor Done */

/* ── Ring parameters ──────────────────────────────────────────────────────── */
#define RX_RING_SIZE  8
#define TX_RING_SIZE  8
#define RX_BUF_SIZE   2048

/* ── Rx descriptor (16 bytes) ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

/* ── Tx descriptor (16 bytes) ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

/* ── Static DMA memory ───────────────────────────────────────────────────── */
static e1000_rx_desc_t rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_RING_SIZE][RX_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_buf[2048] __attribute__((aligned(4096)));

/* ── Driver state ─────────────────────────────────────────────────────────── */
static uint64_t bar0 = 0;
static int      rx_cur = 0;
static int      tx_cur = 0;

/* ── MMIO helpers ─────────────────────────────────────────────────────────── */
static inline uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(bar0 + off);
}
static inline void wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(bar0 + off) = v;
}

/* ── EEPROM read (82540EM / QEMU style) ──────────────────────────────────── */
static uint16_t eerd_read(uint8_t addr) {
    wr32(E1000_EERD, ((uint32_t)addr << 8) | 1u);
    for (int t = 0; t < 2000000; t++) {
        asm volatile("" ::: "memory");
        if (rd32(E1000_EERD) & (1u << 4)) break;
    }
    return (uint16_t)(rd32(E1000_EERD) >> 16);
}

/* ── Initialise one E1000 controller ─────────────────────────────────────── */
static int e1000_init_one(uint8_t bus, uint8_t slot) {
    /* Enable memory space + bus master in PCI command register */
    uint32_t cmd = pci_config_read_dword(bus, slot, 0, 0x04);
    cmd |= 0x06u;
    pci_config_write_dword(bus, slot, 0, 0x04, cmd);

    /* Read 32-bit BAR0 */
    uint32_t bar_raw = pci_config_read_dword(bus, slot, 0, 0x10);
    if (bar_raw & 1u) return -1;           /* I/O BAR — skip */
    if ((bar_raw & 0x6u) == 0x4u) {        /* 64-bit BAR */
        uint32_t bar_hi = pci_config_read_dword(bus, slot, 0, 0x14);
        bar0 = ((uint64_t)bar_hi << 32) | (bar_raw & ~0xFu);
    } else {
        bar0 = bar_raw & ~0xFu;
    }
    if (!bar0) return -1;

    /* Full device reset */
    wr32(E1000_CTRL, rd32(E1000_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 20000; i++);
    for (int t = 0; t < 1000000; t++) if (!(rd32(E1000_CTRL) & CTRL_RST)) break;

    /* Disable all interrupts */
    wr32(E1000_IMC, 0xFFFFFFFFu);

    /* Set Link Up */
    wr32(E1000_CTRL, rd32(E1000_CTRL) | CTRL_SLU);

    /* Read MAC from EEPROM words 0-2 */
    uint16_t w0 = eerd_read(0);
    uint16_t w1 = eerd_read(1);
    uint16_t w2 = eerd_read(2);
    net_mac.b[0] = (uint8_t)(w0);       net_mac.b[1] = (uint8_t)(w0 >> 8);
    net_mac.b[2] = (uint8_t)(w1);       net_mac.b[3] = (uint8_t)(w1 >> 8);
    net_mac.b[4] = (uint8_t)(w2);       net_mac.b[5] = (uint8_t)(w2 >> 8);

    /* If EEPROM gave all-zeroes, fall back to RAL/RAH */
    if (!net_mac.b[0] && !net_mac.b[1] && !net_mac.b[2]) {
        uint32_t ral = rd32(E1000_RAL);
        uint32_t rah = rd32(E1000_RAH);
        net_mac.b[0] = (uint8_t)(ral);       net_mac.b[1] = (uint8_t)(ral >> 8);
        net_mac.b[2] = (uint8_t)(ral >> 16); net_mac.b[3] = (uint8_t)(ral >> 24);
        net_mac.b[4] = (uint8_t)(rah);       net_mac.b[5] = (uint8_t)(rah >> 8);
    }

    /* Clear multicast table */
    for (int i = 0; i < 128; i++) wr32(E1000_MTA + (uint32_t)(i * 4), 0);

    /* ── Setup Rx ring ── */
    for (int i = 0; i < RX_RING_SIZE; i++) {
        rx_ring[i].addr   = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    uint64_t rx_phys = (uint64_t)(uintptr_t)rx_ring;
    wr32(E1000_RDBAL, (uint32_t)rx_phys);
    wr32(E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    wr32(E1000_RDLEN, RX_RING_SIZE * 16);
    wr32(E1000_RDH,   0);
    wr32(E1000_RDT,   RX_RING_SIZE - 1);  /* give all descriptors to HW */
    rx_cur = 0;

    /* RCTL: EN | BAM | SECRC  (accept unicast for our MAC + broadcast) */
    wr32(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* ── Setup Tx ring ── */
    for (int i = 0; i < TX_RING_SIZE; i++) {
        tx_ring[i].addr   = 0;
        tx_ring[i].status = DESC_DD; /* mark all as "done" initially */
    }
    uint64_t tx_phys = (uint64_t)(uintptr_t)tx_ring;
    wr32(E1000_TDBAL, (uint32_t)tx_phys);
    wr32(E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    wr32(E1000_TDLEN, TX_RING_SIZE * 16);
    wr32(E1000_TDH,   0);
    wr32(E1000_TDT,   0);
    tx_cur = 0;

    /* TCTL: EN | PSP | CT=0x0F<<4 | COLD=0x40<<12 */
    wr32(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    /* TIPG recommended for 82540EM */
    wr32(E1000_TIPG, 0x00702008u);

    /* Program our MAC into Receive Address Register 0 */
    uint32_t ral = (uint32_t)net_mac.b[0] | ((uint32_t)net_mac.b[1] << 8)
                 | ((uint32_t)net_mac.b[2] << 16) | ((uint32_t)net_mac.b[3] << 24);
    uint32_t rah = (uint32_t)net_mac.b[4] | ((uint32_t)net_mac.b[5] << 8)
                 | (1u << 31); /* Address Valid bit */
    wr32(E1000_RAL, ral);
    wr32(E1000_RAH, rah);

    return 0;
}

/* ── Public init: scan PCI for Intel Ethernet controllers ────────────────── */
int e1000_init(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_get_vendor(bus, slot) != 0x8086) continue;
            if (pci_get_class_code(bus, slot) != 0x02) continue;
            if (pci_get_subclass(bus, slot)   != 0x00) continue;

            uint16_t dev = (uint16_t)(pci_config_read_dword(bus, slot, 0, 0) >> 16);
            /* Known e1000-compatible device IDs */
            if (dev != 0x100E && dev != 0x100F && dev != 0x1000 &&
                dev != 0x1001 && dev != 0x100C && dev != 0x1010 &&
                dev != 0x1012 && dev != 0x107C && dev != 0x10D3)
                continue;

            if (e1000_init_one((uint8_t)bus, slot) == 0) {
                printf("e1000: %02x:%02x.0 device %04x  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                       bus, slot, dev,
                       net_mac.b[0], net_mac.b[1], net_mac.b[2],
                       net_mac.b[3], net_mac.b[4], net_mac.b[5]);
                return 0;
            }
        }
    }
    return -1;
}

/* ── Send one Ethernet frame (blocking) ──────────────────────────────────── */
int e1000_send(const void *buf, uint16_t len) {
    if (!bar0 || len > 2048) return -1;

    /* Copy frame data to the shared Tx bounce buffer */
    memcpy((uint8_t *)buf, tx_buf, len);

    tx_ring[tx_cur].addr    = (uint64_t)(uintptr_t)tx_buf;
    tx_ring[tx_cur].length  = len;
    tx_ring[tx_cur].cso     = 0;
    tx_ring[tx_cur].cmd     = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_ring[tx_cur].status  = 0;
    tx_ring[tx_cur].css     = 0;
    tx_ring[tx_cur].special = 0;

    asm volatile("" ::: "memory");

    int next = (tx_cur + 1) % TX_RING_SIZE;
    wr32(E1000_TDT, (uint32_t)next);

    /* Poll until DD (descriptor done) bit is set */
    for (int t = 0; t < 2000000; t++) {
        asm volatile("" ::: "memory");
        if (tx_ring[tx_cur].status & DESC_DD) {
            tx_cur = next;
            return 0;
        }
    }
    return -1; /* transmit timeout */
}

/* ── Receive one Ethernet frame (non-blocking) ───────────────────────────── */
int e1000_recv(void *buf, uint16_t max_len) {
    if (!bar0) return 0;

    asm volatile("" ::: "memory");
    if (!(rx_ring[rx_cur].status & DESC_DD)) return 0; /* ring empty */

    uint16_t len = rx_ring[rx_cur].length;
    if (len > max_len) len = max_len;

    /* Copy received data: FROM rx_bufs[rx_cur] TO buf */
    memcpy(rx_bufs[rx_cur], (uint8_t *)buf, len);

    /* Return descriptor to hardware */
    rx_ring[rx_cur].status = 0;
    int old = rx_cur;
    rx_cur = (rx_cur + 1) % RX_RING_SIZE;
    wr32(E1000_RDT, (uint32_t)old);

    return len;
}
