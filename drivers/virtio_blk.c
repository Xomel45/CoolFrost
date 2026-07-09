#include "virtio_blk.h"
#include "pci.h"
#include "../cpu/ports.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

/* ── VirtIO legacy PCI I/O BAR register offsets ────────────────────────── */
#define VIO_DEVFEAT   0x00u   /* device features (RO, 32-bit)         */
#define VIO_DRVFEAT   0x04u   /* driver features (RW, 32-bit)         */
#define VIO_QADDR     0x08u   /* virtqueue PFN  (RW, 32-bit)          */
#define VIO_QSIZE     0x0Cu   /* queue size     (RO, 16-bit)          */
#define VIO_QSEL      0x0Eu   /* queue select   (RW, 16-bit)          */
#define VIO_QNOTIFY   0x10u   /* queue notify   (WO, 16-bit)          */
#define VIO_STATUS    0x12u   /* device status  (RW, 8-bit)           */
#define VIO_ISR       0x13u   /* ISR status     (RO, 8-bit)           */
#define VIO_CFG       0x14u   /* device config  (RO, capacity 64-bit) */

/* Device status bits */
#define VIRTIO_ACK     0x01u
#define VIRTIO_DRV     0x02u
#define VIRTIO_DRV_OK  0x04u
#define VIRTIO_FAILED  0x80u

/* Descriptor flags */
#define VRING_F_NEXT   0x01u
#define VRING_F_WRITE  0x02u   /* device writes to this descriptor   */

/* VirtIO-blk request type */
#define VIRTIO_BLK_T_IN   0u   /* read  */
#define VIRTIO_BLK_T_OUT  1u   /* write */

/* ── Virtqueue layout ───────────────────────────────────────────────────── */
/* Supports QUEUE_SIZE up to 170 entries (desc+avail fit in one 4096-page). */
#define VQ_MAXSIZE  128u
#define VRING_SIZE  8192u     /* two 4096-byte pages                  */

/* We use a single static virtqueue; indices are based on runtime vq_size. */
static uint8_t  virtq_mem[VRING_SIZE] __attribute__((aligned(4096)));
static uint16_t vq_size;           /* entries as reported by device    */
static uint16_t vq_avail_head;     /* next slot in avail ring we write */
static uint16_t vq_used_last;      /* last used->idx we consumed       */
static uint16_t vq_io_base;        /* PCI BAR0 I/O port base           */

/* Layout helpers (all into virtq_mem) */
/* Descriptor table: vq_size × 16 bytes, starting at offset 0 */
#define DESC_ADDR(i)  ((volatile uint64_t *)(virtq_mem + (i)*16u))
#define DESC_LEN(i)   ((volatile uint32_t *)(virtq_mem + (i)*16u + 8u))
#define DESC_FLAGS(i) ((volatile uint16_t *)(virtq_mem + (i)*16u + 12u))
#define DESC_NEXT(i)  ((volatile uint16_t *)(virtq_mem + (i)*16u + 14u))

/* Available ring: at offset vq_size*16 */
#define AVAIL_BASE    ((uint32_t)vq_size * 16u)
#define AVAIL_IDX     ((volatile uint16_t *)(virtq_mem + AVAIL_BASE + 2u))
#define AVAIL_RING(i) ((volatile uint16_t *)(virtq_mem + AVAIL_BASE + 4u + (i)*2u))

/* Used ring: second page (offset 4096) */
#define USED_IDX      ((volatile uint16_t *)(virtq_mem + 4096u + 2u))
#define USED_ID(i)    ((volatile uint32_t *)(virtq_mem + 4096u + 4u + (i)*8u))

/* ── VirtIO-blk request structures ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t type;       /* VIRTIO_BLK_T_IN / T_OUT */
    uint32_t reserved;
    uint64_t sector;
} vblk_req_hdr_t;

static vblk_req_hdr_t vblk_hdr    __attribute__((aligned(4)));
static volatile uint8_t vblk_stat;
static uint8_t vblk_dma[VQ_MAXSIZE * 512u] __attribute__((aligned(4096)));

/* ── Drive table ────────────────────────────────────────────────────────── */
#define MAX_VBLK 4
static vblk_drive_t vblk_drives[MAX_VBLK];
static int          vblk_ndrive = 0;

/* ── Submit a 3-descriptor request chain and poll for completion ──────────── */
/* Returns 0 on success, -1 on error. */
static int vblk_issue(uint32_t type, uint64_t lba, uint16_t count,
                      void *dma_buf, uint32_t dma_len) {
    /* desc[0]: request header (device reads) */
    *DESC_ADDR(0)  = (uint64_t)(uintptr_t)&vblk_hdr;
    *DESC_LEN(0)   = 16u;
    *DESC_FLAGS(0) = VRING_F_NEXT;
    *DESC_NEXT(0)  = 1u;

    /* desc[1]: data buffer (device reads on write, writes on read) */
    *DESC_ADDR(1)  = (uint64_t)(uintptr_t)dma_buf;
    *DESC_LEN(1)   = dma_len;
    *DESC_FLAGS(1) = (uint16_t)((type == VIRTIO_BLK_T_IN ? VRING_F_WRITE : 0u)
                                | VRING_F_NEXT);
    *DESC_NEXT(1)  = 2u;

    /* desc[2]: status byte (device writes) */
    *DESC_ADDR(2)  = (uint64_t)(uintptr_t)&vblk_stat;
    *DESC_LEN(2)   = 1u;
    *DESC_FLAGS(2) = VRING_F_WRITE;
    *DESC_NEXT(2)  = 0u;

    vblk_hdr.type     = type;
    vblk_hdr.reserved = 0u;
    vblk_hdr.sector   = lba;
    (void)count;

    /* Put descriptor chain head in available ring */
    *AVAIL_RING(vq_avail_head % vq_size) = 0u;   /* chain head = desc[0] */
    __asm__ volatile("" ::: "memory");
    *AVAIL_IDX = (uint16_t)(vq_avail_head + 1u);
    __asm__ volatile("" ::: "memory");
    vq_avail_head++;

    /* Notify device: queue 0 */
    port_word_out((uint16_t)(vq_io_base + VIO_QNOTIFY), 0u);

    /* Poll used ring */
    for (uint32_t i = 0; i < 10000000u; i++) {
        __asm__ volatile("" ::: "memory");
        if (*USED_IDX != vq_used_last) break;
    }
    if (*USED_IDX == vq_used_last) return -1;   /* timeout */
    vq_used_last = *USED_IDX;

    return (vblk_stat == 0u) ? 0 : -1;
}

/* ── Init one VirtIO-blk device ─────────────────────────────────────────── */
static int vblk_init_one(uint8_t bus, uint8_t slot, uint8_t func) {
    /* Enable I/O space + bus master */
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04u);
    cmd |= 0x05u;   /* I/O Enable + Bus Master */
    pci_config_write_dword(bus, slot, func, 0x04u, cmd);

    /* BAR0 = I/O space base */
    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10u);
    if (!(bar0 & 1u)) return -1;   /* must be I/O space */
    vq_io_base = (uint16_t)(bar0 & 0xFFFCu);

    /* VirtIO device initialisation sequence */
    port_byte_out((uint16_t)(vq_io_base + VIO_STATUS), 0u);                            /* reset */
    port_byte_out((uint16_t)(vq_io_base + VIO_STATUS), VIRTIO_ACK);                    /* ack   */
    port_byte_out((uint16_t)(vq_io_base + VIO_STATUS), VIRTIO_ACK | VIRTIO_DRV);       /* drv   */
    port_dword_out((uint16_t)(vq_io_base + VIO_DRVFEAT), 0u);                          /* no special features */

    /* Select queue 0 (requestq) and read its size */
    port_word_out((uint16_t)(vq_io_base + VIO_QSEL), 0u);
    vq_size = port_word_in((uint16_t)(vq_io_base + VIO_QSIZE));
    if (vq_size == 0u || vq_size > VQ_MAXSIZE) return -1;

    /* Zero virtqueue memory and set up */
    memset(virtq_mem, 0, VRING_SIZE);
    vq_avail_head = 0u;
    vq_used_last  = 0u;

    /* Tell device the PFN of our virtqueue */
    uint32_t pfn = (uint32_t)((uintptr_t)virtq_mem >> 12);
    port_dword_out((uint16_t)(vq_io_base + VIO_QADDR), pfn);

    /* Signal driver ready */
    port_byte_out((uint16_t)(vq_io_base + VIO_STATUS),
                  VIRTIO_ACK | VIRTIO_DRV | VIRTIO_DRV_OK);

    /* Read capacity (64-bit at device config offset 0) */
    uint32_t cap_lo = port_dword_in((uint16_t)(vq_io_base + VIO_CFG));
    uint32_t cap_hi = port_dword_in((uint16_t)(vq_io_base + VIO_CFG + 4u));
    uint64_t capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    vblk_drives[vblk_ndrive].active       = 1u;
    vblk_drives[vblk_ndrive].sector_count = capacity;
    printf("vblk%d: %llu MB (vq_size=%u)\n",
           vblk_ndrive,
           (unsigned long long)(capacity / 2048u),
           vq_size);
    vblk_ndrive++;
    return 0;
}

/* ── Public: scan PCI ───────────────────────────────────────────────────── */
int vblk_init(void) {
    for (uint16_t bus = 0; bus < 256u; bus++) {
        for (uint8_t slot = 0; slot < 32u; slot++) {
            uint8_t nfunc = pci_is_multifunction((uint8_t)bus, slot) ? 8 : 1;
            for (uint8_t func = 0; func < nfunc; func++) {
                if (pci_get_vendor(bus, slot, func) != 0x1AF4u) continue;
                uint16_t dev = (uint16_t)(pci_config_read_dword(bus, slot, func, 0x00u) >> 16);
                if (dev != 0x1001u) continue;   /* legacy virtio-blk */
                /* Subsystem device ID = 0x0002 confirms block device */
                uint16_t subsys = (uint16_t)(pci_config_read_dword(bus, slot, func, 0x2Cu) >> 16);
                if (subsys != 0x0002u) continue;
                if (vblk_ndrive >= MAX_VBLK) break;
                vblk_init_one(bus, slot, func);
            }
        }
    }
    return (vblk_ndrive > 0) ? 0 : -1;
}

/* ── Public: read / write ───────────────────────────────────────────────── */
int vblk_read_sectors(uint8_t drv, uint64_t lba, uint8_t count, void *buf) {
    if (drv >= (uint8_t)vblk_ndrive || !count) return count ? -1 : 0;
    if ((uint32_t)count * 512u > sizeof(vblk_dma)) return -1;
    if (vblk_issue(VIRTIO_BLK_T_IN, lba, count, vblk_dma,
                   (uint32_t)count * 512u) != 0) return -1;
    memcpy((uint8_t *)buf, vblk_dma, (size_t)count * 512u);
    return 0;
}

int vblk_write_sectors(uint8_t drv, uint64_t lba, uint8_t count, const void *buf) {
    if (drv >= (uint8_t)vblk_ndrive || !count) return count ? -1 : 0;
    if ((uint32_t)count * 512u > sizeof(vblk_dma)) return -1;
    memcpy(vblk_dma, (uint8_t *)buf, (size_t)count * 512u);
    return vblk_issue(VIRTIO_BLK_T_OUT, lba, count, vblk_dma,
                      (uint32_t)count * 512u);
}

int            vblk_drive_count(void)           { return vblk_ndrive; }
vblk_drive_t  *vblk_get_drive(uint8_t i)        {
    return (i < (uint8_t)vblk_ndrive) ? &vblk_drives[i] : 0;
}
