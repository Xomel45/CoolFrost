#include "ahci.h"
#include "pci.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"
#include "../cpu/timer.h"

/* ── HBA generic register offsets (at ABAR) ────────────────────────────── */
#define HBA_CAP   0x00u
#define HBA_GHC   0x04u
#define HBA_IS    0x08u
#define HBA_PI    0x0Cu
#define HBA_VS    0x10u

#define GHC_AE    (1u << 31)
#define GHC_HR    (1u << 0)

/* ── Port register offsets (at ABAR + 0x100 + port*0x80) ─────────────── */
#define POFF_CLB   0x00u
#define POFF_CLBU  0x04u
#define POFF_FB    0x08u
#define POFF_FBU   0x0Cu
#define POFF_IS    0x10u
#define POFF_IE    0x14u
#define POFF_CMD   0x18u
#define POFF_TFD   0x20u
#define POFF_SIG   0x24u
#define POFF_SSTS  0x28u
#define POFF_SCTL  0x2Cu
#define POFF_SERR  0x30u
#define POFF_SACT  0x34u
#define POFF_CI    0x38u

#define CMD_ST    (1u << 0)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define TFD_ERR   (1u << 0)
#define TFD_BSY   (1u << 7)

/* ── ATA commands ──────────────────────────────────────────────────────── */
#define ATA_READ_DMA_EXT   0x25u
#define ATA_WRITE_DMA_EXT  0x35u
#define ATA_IDENTIFY       0xECu

#define SATA_SIG_ATA 0x00000101u

/* ── Static memory ─────────────────────────────────────────────────────── */
#define MAX_AHCI_PORTS  8
#define MAX_AHCI_DRIVES 4
#define AHCI_DMA_SECTS  64u    /* max sectors per transfer */

/* Command List: 32 entries × 32 bytes = 1024 bytes, 1024-byte aligned */
static uint8_t ahci_clb [MAX_AHCI_PORTS][1024] __attribute__((aligned(1024)));
/* FIS receive buffer: 256 bytes, 256-byte aligned */
static uint8_t ahci_fb  [MAX_AHCI_PORTS][256]  __attribute__((aligned(256)));
/* Command Table: CFIS(64)+ACMD(16)+pad(48)+PRDT(16) = 144 bytes, 128-byte aligned */
static uint8_t ahci_ctbl[MAX_AHCI_PORTS][256]  __attribute__((aligned(128)));
/* DMA bounce buffer */
static uint8_t ahci_dma [AHCI_DMA_SECTS * 512] __attribute__((aligned(4096)));

static ahci_drive_t ahci_drives[MAX_AHCI_DRIVES];
static int          ahci_ndrive = 0;

/* AHCI MMIO base (volatile to prevent caching of MMIO reads) */
static volatile uint8_t *ahci_bar = 0;

/* ── Register accessors ─────────────────────────────────────────────────── */
static uint32_t hba_rd(uint32_t off) {
    return *(volatile uint32_t *)(ahci_bar + off);
}
static void hba_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(ahci_bar + off) = v;
}
static uint32_t preg_rd(int p, uint32_t off) {
    return *(volatile uint32_t *)(ahci_bar + 0x100u + (uint32_t)p * 0x80u + off);
}
static void preg_wr(int p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(ahci_bar + 0x100u + (uint32_t)p * 0x80u + off) = v;
}

/* ── Stop / start port DMA engine ──────────────────────────────────────── */
static void port_stop(int p) {
    uint32_t cmd = preg_rd(p, POFF_CMD);
    cmd &= ~CMD_ST;
    preg_wr(p, POFF_CMD, cmd);
    uint64_t dl = get_tick() + 500;
    while (get_tick() < dl && (preg_rd(p, POFF_CMD) & CMD_CR));

    cmd = preg_rd(p, POFF_CMD);
    cmd &= ~CMD_FRE;
    preg_wr(p, POFF_CMD, cmd);
    dl = get_tick() + 500;
    while (get_tick() < dl && (preg_rd(p, POFF_CMD) & CMD_FR));
}

static void port_start(int p) {
    uint64_t dl = get_tick() + 500;
    while (get_tick() < dl && (preg_rd(p, POFF_CMD) & CMD_CR));
    uint32_t cmd = preg_rd(p, POFF_CMD);
    cmd |= CMD_FRE | CMD_ST;
    preg_wr(p, POFF_CMD, cmd);
}

/* ── Initialise one port: set CLB/FB, point slot-0 to ctbl ─────────────── */
static void port_init(int p) {
    port_stop(p);

    uint64_t clb_pa  = (uint64_t)(uintptr_t)ahci_clb[p];
    uint64_t fb_pa   = (uint64_t)(uintptr_t)ahci_fb[p];
    uint64_t ct_pa   = (uint64_t)(uintptr_t)ahci_ctbl[p];

    preg_wr(p, POFF_CLB,  (uint32_t)(clb_pa & 0xFFFFFFFFu));
    preg_wr(p, POFF_CLBU, (uint32_t)(clb_pa >> 32));
    preg_wr(p, POFF_FB,   (uint32_t)(fb_pa  & 0xFFFFFFFFu));
    preg_wr(p, POFF_FBU,  (uint32_t)(fb_pa  >> 32));

    preg_wr(p, POFF_IS,   0xFFFFFFFFu);
    preg_wr(p, POFF_SERR, 0xFFFFFFFFu);
    preg_wr(p, POFF_IE,   0u);           /* polling mode: no IRQs */

    /* Zero command list; slot 0 points to ahci_ctbl[p] */
    memset(ahci_clb[p], 0, 1024u);
    volatile uint32_t *cle = (volatile uint32_t *)ahci_clb[p];
    cle[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);
    cle[3] = (uint32_t)(ct_pa >> 32);

    port_start(p);
}

/* ── Issue one ATA DMA command on slot 0, poll for completion ────────────── */
/* dma_buf: physical == virtual (identity mapped); 512-byte sector size  */
static int ahci_issue(int p, uint8_t cmd_b, uint64_t lba, uint16_t count,
                      void *dma_buf, int write) {
    uint64_t dma_pa = (uint64_t)(uintptr_t)dma_buf;
    uint64_t ct_pa  = (uint64_t)(uintptr_t)ahci_ctbl[p];

    /* Update slot-0 command list entry */
    volatile uint32_t *cle = (volatile uint32_t *)ahci_clb[p];
    uint32_t dw0 = 5u | (1u << 16);   /* CFL=5 DWORDs, PRDTL=1 */
    if (write) dw0 |= (1u << 6);      /* W bit */
    cle[0] = dw0;
    cle[1] = 0u;                       /* PRDBC cleared by HBA */
    cle[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);
    cle[3] = (uint32_t)(ct_pa >> 32);

    /* Build command table */
    uint8_t *ct = ahci_ctbl[p];
    memset(ct, 0, 256u);

    /* CFIS: H2D Register FIS (type 0x27, 20 bytes) */
    ct[0]  = 0x27u;           /* FIS type H2D */
    ct[1]  = 0x80u;           /* C bit: new command */
    ct[2]  = cmd_b;
    ct[4]  = (uint8_t)(lba);
    ct[5]  = (uint8_t)(lba >> 8);
    ct[6]  = (uint8_t)(lba >> 16);
    ct[7]  = 0x40u;           /* LBA mode, device 0 */
    ct[8]  = (uint8_t)(lba >> 24);
    ct[9]  = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)(count & 0xFF);
    ct[13] = (uint8_t)(count >> 8);

    /* PRD at offset 128: one entry covering the whole transfer */
    volatile uint32_t *prd = (volatile uint32_t *)(ct + 128);
    prd[0] = (uint32_t)(dma_pa & 0xFFFFFFFFu);
    prd[1] = (uint32_t)(dma_pa >> 32);
    prd[2] = 0u;
    prd[3] = (uint32_t)(count * 512u - 1u);

    /* Issue command on slot 0 */
    preg_wr(p, POFF_IS,   0xFFFFFFFFu);
    preg_wr(p, POFF_SERR, 0xFFFFFFFFu);
    preg_wr(p, POFF_CI,   1u);

    /* Poll until CI clears or error */
    uint64_t deadline = get_tick() + 5000u;
    while (get_tick() < deadline) {
        if (preg_rd(p, POFF_TFD) & TFD_ERR) return -1;
        if (!(preg_rd(p, POFF_CI) & 1u))     break;
    }
    return (preg_rd(p, POFF_CI) & 1u) ? -1 : 0;
}

/* ── Send IDENTIFY DEVICE, return sector count (0 = failed) ──────────────── */
static uint64_t port_identify(int p) {
    uint64_t dma_pa = (uint64_t)(uintptr_t)ahci_dma;
    uint64_t ct_pa  = (uint64_t)(uintptr_t)ahci_ctbl[p];
    memset(ahci_dma, 0, 512u);

    volatile uint32_t *cle = (volatile uint32_t *)ahci_clb[p];
    cle[0] = 5u | (1u << 16);   /* CFL=5, PRDTL=1, read */
    cle[1] = 0u;
    cle[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);
    cle[3] = (uint32_t)(ct_pa >> 32);

    uint8_t *ct = ahci_ctbl[p];
    memset(ct, 0, 256u);
    ct[0] = 0x27u;       /* H2D FIS */
    ct[1] = 0x80u;       /* C bit   */
    ct[2] = ATA_IDENTIFY;
    ct[7] = 0xA0u;       /* device 0, no LBA mode */

    volatile uint32_t *prd = (volatile uint32_t *)(ct + 128);
    prd[0] = (uint32_t)(dma_pa & 0xFFFFFFFFu);
    prd[1] = (uint32_t)(dma_pa >> 32);
    prd[2] = 0u;
    prd[3] = 511u;  /* 512 bytes - 1 */

    preg_wr(p, POFF_IS,   0xFFFFFFFFu);
    preg_wr(p, POFF_SERR, 0xFFFFFFFFu);
    preg_wr(p, POFF_CI,   1u);

    uint64_t deadline = get_tick() + 5000u;
    while (get_tick() < deadline) {
        if (preg_rd(p, POFF_TFD) & TFD_ERR) return 0u;
        if (!(preg_rd(p, POFF_CI) & 1u))     break;
    }
    if (preg_rd(p, POFF_CI) & 1u) return 0u;

    /* Words 100-103: 48-bit user addressable sector count */
    const uint16_t *id = (const uint16_t *)ahci_dma;
    uint64_t lba48 = (uint64_t)id[100]
                   | ((uint64_t)id[101] << 16)
                   | ((uint64_t)id[102] << 32)
                   | ((uint64_t)id[103] << 48);
    if (lba48) return lba48;
    /* Fallback: words 60-61 (28-bit) */
    return (uint64_t)id[60] | ((uint64_t)id[61] << 16);
}

/* ── Public: ahci_init ──────────────────────────────────────────────────── */
int ahci_init(void) {
    for (uint16_t bus = 0; bus < 256u; bus++) {
        for (uint8_t slot = 0; slot < 32u; slot++) {
          uint8_t nfunc = pci_is_multifunction((uint8_t)bus, slot) ? 8 : 1;
          for (uint8_t func = 0; func < nfunc; func++) {
            if (pci_get_vendor(bus, slot, func) == 0xFFFF) continue;
            if (pci_get_class_code(bus, slot, func) != 0x01u) continue;
            if (pci_get_subclass(bus, slot, func)   != 0x06u) continue;
            if (pci_get_progif(bus, slot, func)     != 0x01u) continue;

            /* Enable bus master + memory space */
            uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04u);
            cmd |= 0x06u;
            pci_config_write_dword(bus, slot, func, 0x04u, cmd);

            /* Read BAR5 (ABAR) — AHCI uses a 32-bit memory BAR here */
            uint32_t bar5 = pci_config_read_dword(bus, slot, func, 0x24u);
            bar5 &= ~0xFu;
            ahci_bar = (volatile uint8_t *)(uintptr_t)bar5;

            /* Enable AHCI mode */
            hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);

            /* Global HBA reset */
            hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_HR);
            uint64_t rst_dl = get_tick() + 1000u;
            while (get_tick() < rst_dl && (hba_rd(HBA_GHC) & GHC_HR));

            /* Re-enable AHCI after reset */
            hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);

            /* Probe implemented ports */
            uint32_t pi = hba_rd(HBA_PI);
            for (int p = 0; p < 32 && ahci_ndrive < MAX_AHCI_DRIVES; p++) {
                if (!(pi & (1u << p))) continue;
                /* Device must be present and comms established (DET == 3) */
                if ((preg_rd(p, POFF_SSTS) & 0xFu) != 3u) continue;
                /* Must be a SATA (non-ATAPI) device */
                if (preg_rd(p, POFF_SIG) != SATA_SIG_ATA) continue;

                port_init(p);
                uint64_t nsect = port_identify(p);
                if (!nsect) continue;

                ahci_drives[ahci_ndrive].port_idx     = (uint8_t)p;
                ahci_drives[ahci_ndrive].active       = 1;
                ahci_drives[ahci_ndrive].sector_count = nsect;
                printf("ahci%d: SATA port %d, %llu MB\n",
                       ahci_ndrive, p,
                       (unsigned long long)(nsect / 2048u));
                ahci_ndrive++;
            }
            return ahci_ndrive > 0 ? 0 : -1;
          }
        }
    }
    return -1;
}

/* ── Public: read / write ───────────────────────────────────────────────── */
int ahci_read_sectors(uint8_t drv, uint64_t lba, uint8_t count, void *buf) {
    if (drv >= (uint8_t)ahci_ndrive || !count) return count ? -1 : 0;
    if (count > AHCI_DMA_SECTS) return -1;
    int p = ahci_drives[drv].port_idx;
    if (ahci_issue(p, ATA_READ_DMA_EXT, lba, count, ahci_dma, 0) != 0)
        return -1;
    memcpy((uint8_t *)buf, ahci_dma, (size_t)count * 512u);
    return 0;
}

int ahci_write_sectors(uint8_t drv, uint64_t lba, uint8_t count, const void *buf) {
    if (drv >= (uint8_t)ahci_ndrive || !count) return count ? -1 : 0;
    if (count > AHCI_DMA_SECTS) return -1;
    int p = ahci_drives[drv].port_idx;
    memcpy(ahci_dma, (uint8_t *)buf, (size_t)count * 512u);
    return ahci_issue(p, ATA_WRITE_DMA_EXT, lba, count, ahci_dma, 1);
}

int ahci_drive_count(void)           { return ahci_ndrive; }
ahci_drive_t *ahci_get_drive(uint8_t i) {
    return (i < (uint8_t)ahci_ndrive) ? &ahci_drives[i] : 0;
}
