#include "nvme.h"
#include "pci.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

#include <stdint.h>

/* ── NVMe controller register offsets ────────────────────────────────────── */
#define NVME_CAP_LO  0x00u
#define NVME_CAP_HI  0x04u
#define NVME_CC      0x14u
#define NVME_CSTS    0x1Cu
#define NVME_AQA     0x24u
#define NVME_ASQ_LO  0x28u
#define NVME_ASQ_HI  0x2Cu
#define NVME_ACQ_LO  0x30u
#define NVME_ACQ_HI  0x34u

/* ── CC register bits ────────────────────────────────────────────────────── */
#define CC_EN        (1u  << 0)
#define CC_CSS_NVM   (0u  << 4)
#define CC_MPS_4K    (0u  << 7)
#define CC_AMS_RR    (0u  << 11)
#define CC_SHN_NONE  (0u  << 14)
#define CC_IOSQES_64 (6u  << 16)
#define CC_IOCQES_16 (4u  << 20)

/* ── CSTS bits ───────────────────────────────────────────────────────────── */
#define CSTS_RDY     (1u << 0)
#define CSTS_CFS     (1u << 1)

/* ── Queue parameters ────────────────────────────────────────────────────── */
#define NVME_Q_DEPTH  64
#define NVME_SQE_SIZE 64
#define NVME_CQE_SIZE 16

/* ── Admin command opcodes ───────────────────────────────────────────────── */
#define ADM_CREATE_IO_SQ  0x01u
#define ADM_CREATE_IO_CQ  0x05u
#define ADM_IDENTIFY      0x06u

/* ── NVM I/O command opcodes ─────────────────────────────────────────────── */
#define IO_WRITE  0x01u
#define IO_READ   0x02u

/* ── Identify CNS ────────────────────────────────────────────────────────── */
#define ID_CNS_NS    0u
#define ID_CNS_CTRL  1u

/* ── Submission Queue Entry — 64 bytes ───────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t cdw0;   /* OPC[7:0] | FUSE[9:8] | PSDT[15:14] | CID[31:16] */
    uint32_t nsid;
    uint64_t cdw2_3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

/* ── Completion Queue Entry — 16 bytes ───────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status_phase; /* bit0=phase tag, bits[15:1]=status field */
} nvme_cqe_t;

/* ── Static state ────────────────────────────────────────────────────────── */
static nvme_drive_t drives[MAX_NVME_DRIVES];
static uint8_t      num_drives = 0;

/*
 * Per-drive queue memory.  Each slot is exactly 4096 bytes so that
 * every drive's queue starts at a 4KB-aligned address.
 *
 * Admin SQ: 64 entries × 64 B = 4096 B per drive.
 * Admin CQ: 64 entries × 16 B = 1024 B — padded to 4096 B per drive.
 * I/O SQ / I/O CQ: same layout.
 * Identify buffer: 4096 B per drive.
 */
static uint8_t admin_sq[MAX_NVME_DRIVES * NVME_Q_DEPTH * NVME_SQE_SIZE] __attribute__((aligned(4096)));
static uint8_t admin_cq[MAX_NVME_DRIVES * 4096]                          __attribute__((aligned(4096)));
static uint8_t io_sq[MAX_NVME_DRIVES * NVME_Q_DEPTH * NVME_SQE_SIZE]    __attribute__((aligned(4096)));
static uint8_t io_cq[MAX_NVME_DRIVES * 4096]                             __attribute__((aligned(4096)));
static uint8_t id_buf[MAX_NVME_DRIVES * 4096]                            __attribute__((aligned(4096)));

/* Shared 4KB bounce buffer for I/O data (max 8 × 512-byte sectors per command) */
static uint8_t io_data[4096] __attribute__((aligned(4096)));

/* ── MMIO helpers ────────────────────────────────────────────────────────── */
static inline uint32_t rd32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void wr32(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = v;
}
static inline void wr64(uint64_t base, uint32_t off, uint64_t v) {
    wr32(base, off,     (uint32_t)v);
    wr32(base, off + 4, (uint32_t)(v >> 32));
}

/*
 * Doorbell register pointer.
 * q_id=0 → admin queues; q_id=1 → I/O queue pair 1.
 * is_cq=0 → SQ tail doorbell; is_cq=1 → CQ head doorbell.
 */
static inline volatile uint32_t *doorbell(nvme_drive_t *d, uint32_t q_id, int is_cq) {
    uint64_t addr = d->bar0 + 0x1000u
                  + (2u * q_id + (uint32_t)is_cq) * (4u << d->dstrd);
    return (volatile uint32_t *)(uintptr_t)addr;
}

/* ── Submit one admin command and poll CQ for completion ─────────────────── */
static int admin_cmd(nvme_drive_t *d, nvme_sqe_t *cmd) {
    int idx = (int)(d - drives);

    nvme_sqe_t *sq   = (nvme_sqe_t *)(admin_sq + idx * NVME_Q_DEPTH * NVME_SQE_SIZE);
    nvme_sqe_t *slot = &sq[d->admin_sq_tail];
    *slot = *cmd;
    /* Embed command ID into cdw0[31:16] */
    slot->cdw0 = (cmd->cdw0 & 0x0000FFFFu) | ((uint32_t)d->next_cid++ << 16);

    asm volatile("" ::: "memory");

    d->admin_sq_tail = (d->admin_sq_tail + 1) % NVME_Q_DEPTH;
    *doorbell(d, 0, 0) = d->admin_sq_tail;

    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t *)(admin_cq + idx * 4096);
    for (int t = 0; t < 2000000; t++) {
        asm volatile("" ::: "memory");
        if ((cq[d->admin_cq_head].status_phase & 1u) == d->admin_cq_phase) {
            uint16_t sc = (cq[d->admin_cq_head].status_phase >> 1) & 0x7FFu;
            d->admin_cq_head = (d->admin_cq_head + 1) % NVME_Q_DEPTH;
            if (d->admin_cq_head == 0) d->admin_cq_phase ^= 1;
            *doorbell(d, 0, 1) = d->admin_cq_head;
            return (sc == 0) ? 0 : -1;
        }
    }
    return -1; /* timeout */
}

/* ── Submit one I/O command and poll CQ for completion ───────────────────── */
static int io_cmd(nvme_drive_t *d, nvme_sqe_t *cmd) {
    int idx = (int)(d - drives);

    nvme_sqe_t *sq   = (nvme_sqe_t *)(io_sq + idx * NVME_Q_DEPTH * NVME_SQE_SIZE);
    nvme_sqe_t *slot = &sq[d->io_sq_tail];
    *slot = *cmd;
    slot->cdw0 = (cmd->cdw0 & 0x0000FFFFu) | ((uint32_t)d->next_cid++ << 16);

    asm volatile("" ::: "memory");

    d->io_sq_tail = (d->io_sq_tail + 1) % NVME_Q_DEPTH;
    *doorbell(d, 1, 0) = d->io_sq_tail;

    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t *)(io_cq + idx * 4096);
    for (int t = 0; t < 2000000; t++) {
        asm volatile("" ::: "memory");
        if ((cq[d->io_cq_head].status_phase & 1u) == d->io_cq_phase) {
            uint16_t sc = (cq[d->io_cq_head].status_phase >> 1) & 0x7FFu;
            d->io_cq_head = (d->io_cq_head + 1) % NVME_Q_DEPTH;
            if (d->io_cq_head == 0) d->io_cq_phase ^= 1;
            *doorbell(d, 1, 1) = d->io_cq_head;
            return (sc == 0) ? 0 : -1;
        }
    }
    return -1; /* timeout */
}

/* ── Initialise one NVMe controller ──────────────────────────────────────── */
static int nvme_init_one(uint8_t bus, uint8_t slot) {
    if (num_drives >= MAX_NVME_DRIVES) return -1;

    int idx = num_drives;
    nvme_drive_t *d = &drives[idx];
    memset(d, 0, sizeof(*d));
    d->admin_cq_phase = 1;
    d->io_cq_phase    = 1;

    /* Enable memory space + bus mastering in PCI command register */
    uint32_t pci_cmd = pci_config_read_dword(bus, slot, 0, 0x04u);
    pci_cmd |= 0x06u;
    pci_config_write_dword(bus, slot, 0, 0x04u, pci_cmd);

    /* Read 64-bit BAR0 (NVMe always uses a 64-bit memory BAR) */
    uint32_t bar0_lo = pci_config_read_dword(bus, slot, 0, 0x10u);
    uint32_t bar0_hi = pci_config_read_dword(bus, slot, 0, 0x14u);
    d->bar0 = ((uint64_t)bar0_hi << 32) | ((uint64_t)bar0_lo & ~0xFULL);
    if (!d->bar0) return -1;

    /* CAP[35:32] = DSTRD (doorbell stride field) */
    d->dstrd = rd32(d->bar0, NVME_CAP_HI) & 0xFu;

    /* Disable controller and wait for RDY=0 */
    uint32_t cc = rd32(d->bar0, NVME_CC);
    if (cc & CC_EN) {
        wr32(d->bar0, NVME_CC, cc & ~CC_EN);
        for (int t = 0; t < 1000000; t++)
            if (!(rd32(d->bar0, NVME_CSTS) & CSTS_RDY)) break;
    }

    /* Set admin queue attributes and base addresses */
    uint64_t asq = (uint64_t)(uintptr_t)(admin_sq + idx * NVME_Q_DEPTH * NVME_SQE_SIZE);
    uint64_t acq = (uint64_t)(uintptr_t)(admin_cq + idx * 4096u);

    wr32(d->bar0, NVME_AQA, ((NVME_Q_DEPTH - 1u) << 16) | (NVME_Q_DEPTH - 1u));
    wr64(d->bar0, NVME_ASQ_LO, asq);
    wr64(d->bar0, NVME_ACQ_LO, acq);

    /* Enable controller */
    wr32(d->bar0, NVME_CC, CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR |
                            CC_SHN_NONE | CC_IOSQES_64 | CC_IOCQES_16 | CC_EN);

    /* Wait for RDY=1 */
    for (int t = 0; t < 2000000; t++) {
        uint32_t csts = rd32(d->bar0, NVME_CSTS);
        if (csts & CSTS_CFS) return -1;
        if (csts & CSTS_RDY) break;
        if (t == 1999999) return -1;
    }

    /* ── Identify controller ── */
    uint8_t *ib = id_buf + idx * 4096u;
    memset(ib, 0, 4096u);

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = ADM_IDENTIFY;
    cmd.prp1  = (uint64_t)(uintptr_t)ib;
    cmd.cdw10 = ID_CNS_CTRL;
    if (admin_cmd(d, &cmd) != 0) return -1;

    /* Model number: bytes 24-63 (40 bytes, space-padded) */
    for (int i = 0; i < 40; i++)
        d->model[i] = (char)ib[24 + i];
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';

    /* ── Identify namespace 1 ── */
    memset(ib, 0, 4096u);
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = ADM_IDENTIFY;
    cmd.nsid  = 1;
    cmd.prp1  = (uint64_t)(uintptr_t)ib;
    cmd.cdw10 = ID_CNS_NS;
    if (admin_cmd(d, &cmd) != 0) return -1;

    /* NSZE at bytes 0-7: namespace size in logical blocks */
    uint64_t nsze = 0;
    /* CoolFrost memcpy(source, dest, n): copies FROM ib TO &nsze */
    memcpy(ib, (uint8_t *)&nsze, 8u);

    /* FLBAS at byte 26: active LBA format index [3:0] */
    uint8_t flbas = ib[26] & 0x0Fu;
    /* LBAF[flbas] at byte 128 + flbas*4: LBADS at bits[23:16] */
    uint32_t lbaf = 0;
    memcpy(ib + 128u + flbas * 4u, (uint8_t *)&lbaf, 4u);
    uint8_t lbads = (uint8_t)((lbaf >> 16) & 0xFFu);
    d->lba_size = 1u << lbads;
    if (d->lba_size < 512u) d->lba_size = 512u;

    /* Convert to 512-byte sector count */
    d->sectors = (d->lba_size >= 512u)
               ? nsze * (d->lba_size / 512u)
               : nsze / (512u / d->lba_size);

    /* ── Create I/O Completion Queue (QID=1) ── */
    uint64_t icq_phys = (uint64_t)(uintptr_t)(io_cq + idx * 4096u);
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = ADM_CREATE_IO_CQ;
    cmd.prp1  = icq_phys;
    cmd.cdw10 = ((NVME_Q_DEPTH - 1u) << 16) | 1u; /* QSIZE-1 | QID=1 */
    cmd.cdw11 = 0x01u;  /* PC=1 (physically contiguous), IEN=0 */
    if (admin_cmd(d, &cmd) != 0) return -1;

    /* ── Create I/O Submission Queue (QID=1, CQID=1) ── */
    uint64_t isq_phys = (uint64_t)(uintptr_t)(io_sq + idx * NVME_Q_DEPTH * NVME_SQE_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = ADM_CREATE_IO_SQ;
    cmd.prp1  = isq_phys;
    cmd.cdw10 = ((NVME_Q_DEPTH - 1u) << 16) | 1u; /* QSIZE-1 | QID=1 */
    cmd.cdw11 = (1u << 16) | 0x01u;               /* CQID=1, PC=1 */
    if (admin_cmd(d, &cmd) != 0) return -1;

    d->present = 1;
    num_drives++;
    return 0;
}

/* ── Public: initialise all NVMe controllers found on PCI ────────────────── */
void nvme_init(void) {
    num_drives = 0;
    memset(drives, 0, sizeof(drives));

    for (uint16_t bus = 0; bus < 256 && num_drives < MAX_NVME_DRIVES; bus++) {
        for (uint8_t slot = 0; slot < 32 && num_drives < MAX_NVME_DRIVES; slot++) {
            if (pci_get_vendor(bus, slot) == 0xFFFF) continue;
            /* NVMe: class=0x01 (storage), subclass=0x08 (NVM), progif=0x02 */
            if (pci_get_class_code(bus, slot) != 0x01) continue;
            if (pci_get_subclass(bus, slot)   != 0x08) continue;
            if (pci_get_progif(bus, slot)     != 0x02) continue;

            if (nvme_init_one((uint8_t)bus, slot) == 0) {
                nvme_drive_t *d = &drives[num_drives - 1];
                uint64_t mb = d->sectors / 2048u;
                printf("nvme%d: %s  %lu MB  (%lu sectors, LBA=%u B)\n",
                       (int)(num_drives - 1), d->model, mb, d->sectors, d->lba_size);
            }
        }
    }
}

/* ── Read `count` 512-byte sectors from drive ────────────────────────────── */
int nvme_read_sectors(uint8_t drv_idx, uint64_t lba, uint8_t count, void *buf) {
    if (drv_idx >= num_drives || !drives[drv_idx].present) return -1;
    nvme_drive_t *d = &drives[drv_idx];
    uint8_t *out = (uint8_t *)buf;

    if (d->lba_size == 512u) {
        /* Fast path: 1:1 mapping between ATA sectors and NVMe logical blocks */
        uint8_t done = 0;
        while (done < count) {
            /* Bounce buffer holds 4096 B = 8 sectors max */
            uint8_t batch = count - done;
            if (batch > 8u) batch = 8u;

            nvme_sqe_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cdw0  = IO_READ;
            cmd.nsid  = 1;
            cmd.prp1  = (uint64_t)(uintptr_t)io_data;
            cmd.cdw10 = (uint32_t)(lba + done);
            cmd.cdw11 = (uint32_t)((lba + done) >> 32);
            cmd.cdw12 = (uint32_t)(batch - 1u); /* NLB: number of blocks - 1 */
            if (io_cmd(d, &cmd) != 0) return -1;

            /* Copy io_data → out (CoolFrost memcpy: source first) */
            memcpy(io_data, out + (size_t)done * 512u, (size_t)batch * 512u);
            done += batch;
        }
        return 0;
    }

    if (d->lba_size == 4096u) {
        /* 4 KB logical blocks: each NVMe block covers 8 ATA sectors.
         * Handle one ATA sector at a time with read + extract. */
        for (uint8_t i = 0; i < count; i++) {
            uint64_t cur     = lba + i;
            uint64_t nlba    = cur / 8u;
            uint32_t off_in  = (uint32_t)(cur % 8u) * 512u;

            nvme_sqe_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cdw0  = IO_READ;
            cmd.nsid  = 1;
            cmd.prp1  = (uint64_t)(uintptr_t)io_data;
            cmd.cdw10 = (uint32_t)nlba;
            cmd.cdw11 = (uint32_t)(nlba >> 32);
            cmd.cdw12 = 0; /* NLB=0 → 1 block */
            if (io_cmd(d, &cmd) != 0) return -1;

            /* Extract the 512-byte slice (CoolFrost memcpy: source first) */
            memcpy(io_data + off_in, out + (size_t)i * 512u, 512u);
        }
        return 0;
    }

    return -1; /* unsupported LBA size */
}

/* ── Write `count` 512-byte sectors to drive ─────────────────────────────── */
int nvme_write_sectors(uint8_t drv_idx, uint64_t lba, uint8_t count, const void *buf) {
    if (drv_idx >= num_drives || !drives[drv_idx].present) return -1;
    nvme_drive_t *d = &drives[drv_idx];
    const uint8_t *in = (const uint8_t *)buf;

    if (d->lba_size != 512u) return -1; /* only 512-byte sectors for writes */

    uint8_t done = 0;
    while (done < count) {
        uint8_t batch = count - done;
        if (batch > 8u) batch = 8u;

        /* Copy in → io_data (CoolFrost memcpy: source first; cast away const) */
        memcpy((uint8_t *)(in + (size_t)done * 512u), io_data, (size_t)batch * 512u);

        nvme_sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0  = IO_WRITE;
        cmd.nsid  = 1;
        cmd.prp1  = (uint64_t)(uintptr_t)io_data;
        cmd.cdw10 = (uint32_t)(lba + done);
        cmd.cdw11 = (uint32_t)((lba + done) >> 32);
        cmd.cdw12 = (uint32_t)(batch - 1u);
        if (io_cmd(d, &cmd) != 0) return -1;

        done += batch;
    }
    return 0;
}

/* ── Accessors ───────────────────────────────────────────────────────────── */
nvme_drive_t *nvme_get_drive(uint8_t idx) {
    if (idx >= MAX_NVME_DRIVES) return 0;
    return &drives[idx];
}

uint8_t nvme_drive_count(void) {
    return num_drives;
}
