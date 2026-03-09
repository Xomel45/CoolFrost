#include "ata.h"
#include "../cpu/ports.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

/* ── Internal state ────────────────────────────────────────────────────── */
static ata_drive_t drives[MAX_ATA_DRIVES];
static uint8_t     num_drives = 0;

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* 400ns delay by reading the alternate status port 4 times */
static void ata_io_delay(uint16_t ctrl) {
    port_byte_in(ctrl);
    port_byte_in(ctrl);
    port_byte_in(ctrl);
    port_byte_in(ctrl);
}

/* Wait until BSY clears. Returns 0 on success, -1 on timeout. */
static int ata_wait_bsy(uint16_t io) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = port_byte_in(io + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

/* Wait until DRQ sets (or ERR/DF). Returns 0 on success, -1 on error. */
static int ata_wait_drq(uint16_t io) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = port_byte_in(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return -1;
        if (status & ATA_SR_DF)   return -1;
        if (status & ATA_SR_DRQ)  return 0;
    }
    return -1;
}

/* Soft-reset a bus via the control register */
static void ata_soft_reset(uint16_t ctrl) {
    port_byte_out(ctrl, 0x04);  /* set SRST bit  */
    ata_io_delay(ctrl);
    port_byte_out(ctrl, 0x00);  /* clear SRST    */
    ata_io_delay(ctrl);
}

/* ── Probe one drive ───────────────────────────────────────────────────── */
static void ata_identify_drive(uint16_t io, uint16_t ctrl, uint8_t is_slave) {
    uint8_t drive_byte = is_slave ? 0xB0 : 0xA0;

    /* Select drive */
    port_byte_out(io + ATA_REG_DRIVE, drive_byte);
    ata_io_delay(ctrl);

    /* Zero out sector count & LBA registers */
    port_byte_out(io + ATA_REG_SECCOUNT, 0);
    port_byte_out(io + ATA_REG_LBA_LO,   0);
    port_byte_out(io + ATA_REG_LBA_MID,  0);
    port_byte_out(io + ATA_REG_LBA_HI,   0);

    /* Send IDENTIFY */
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_delay(ctrl);

    /* If status == 0, drive does not exist */
    uint8_t status = port_byte_in(io + ATA_REG_STATUS);
    if (status == 0)
        return;

    /* Wait for BSY to clear */
    if (ata_wait_bsy(io) != 0)
        return;

    /* Check LBA_MID and LBA_HI — if non-zero, it's not ATA (might be ATAPI) */
    if (port_byte_in(io + ATA_REG_LBA_MID) != 0 ||
        port_byte_in(io + ATA_REG_LBA_HI)  != 0)
        return;

    /* Wait for DRQ or error */
    if (ata_wait_drq(io) != 0)
        return;

    /* Read the 256-word IDENTIFY data */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = port_word_in(io + ATA_REG_DATA);

    /* Fill drive info */
    ata_drive_t *drv = &drives[num_drives];
    drv->present   = 1;
    drv->io_base   = io;
    drv->ctrl_base = ctrl;
    drv->is_slave  = is_slave;

    /* Total 28-bit LBA sectors: words 60-61 (little-endian dword) */
    drv->sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);

    /* Model string: words 27-46, bytes are swapped in each word */
    for (int i = 0; i < 20; i++) {
        drv->model[i * 2]     = (char)(identify[27 + i] >> 8);
        drv->model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
    }
    drv->model[40] = '\0';

    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && drv->model[i] == ' '; i--)
        drv->model[i] = '\0';

    num_drives++;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void ata_init(void) {
    num_drives = 0;
    memset(drives, 0, sizeof(drives));

    /* Soft-reset both buses */
    ata_soft_reset(ATA_PRIMARY_CTRL);
    ata_soft_reset(ATA_SECONDARY_CTRL);

    /* Probe all four possible positions */
    ata_identify_drive(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0); /* pri master */
    ata_identify_drive(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   1); /* pri slave  */
    ata_identify_drive(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0); /* sec master */
    ata_identify_drive(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1); /* sec slave  */

    for (uint8_t i = 0; i < num_drives; i++) {
        ata_drive_t *d = &drives[i];
        uint32_t mb = d->sectors / 2048; /* sectors × 512 / 1048576 */
        printf("ata%d: %s  %u MB  (%u sectors)\n",
                i, d->model, mb, d->sectors);
    }
}

int ata_read_sectors(uint8_t drive_idx, uint32_t lba, uint8_t count, void *buffer) {
    if (drive_idx >= num_drives)      return -1;
    if (!drives[drive_idx].present)   return -1;
    if (count == 0)                   return -1;

    ata_drive_t *drv = &drives[drive_idx];
    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slave_bit = drv->is_slave ? 0x10 : 0x00;

    /* Wait for drive ready */
    if (ata_wait_bsy(io) != 0) return -1;

    /* Select drive, LBA mode, top 4 bits of 28-bit LBA */
    port_byte_out(io + ATA_REG_DRIVE,
                  0xE0 | slave_bit | ((lba >> 24) & 0x0F));
    ata_io_delay(ctrl);

    /* Sector count */
    port_byte_out(io + ATA_REG_SECCOUNT, count);

    /* LBA bytes */
    port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* Send READ command */
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* Read each sector */
    uint16_t *buf = (uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0)
            return -1;

        for (int w = 0; w < 256; w++)
            buf[s * 256 + w] = port_word_in(io + ATA_REG_DATA);

        /* 400ns delay between sectors */
        ata_io_delay(ctrl);
    }

    return 0;
}

int ata_write_sectors(uint8_t drive_idx, uint32_t lba, uint8_t count, const void *buffer) {
    if (drive_idx >= num_drives)      return -1;
    if (!drives[drive_idx].present)   return -1;
    if (count == 0)                   return -1;

    ata_drive_t *drv = &drives[drive_idx];
    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slave_bit = drv->is_slave ? 0x10 : 0x00;

    if (ata_wait_bsy(io) != 0) return -1;

    port_byte_out(io + ATA_REG_DRIVE,
                  0xE0 | slave_bit | ((lba >> 24) & 0x0F));
    ata_io_delay(ctrl);

    port_byte_out(io + ATA_REG_SECCOUNT, count);
    port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));

    /* Send WRITE command */
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    const uint16_t *buf = (const uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0)
            return -1;

        for (int w = 0; w < 256; w++)
            port_word_out(io + ATA_REG_DATA, buf[s * 256 + w]);

        ata_io_delay(ctrl);
    }

    /* Flush write cache */
    port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_bsy(io);

    return 0;
}

ata_drive_t *ata_get_drive(uint8_t index) {
    if (index >= MAX_ATA_DRIVES) return 0;
    return &drives[index];
}

uint8_t ata_drive_count(void) {
    return num_drives;
}

/* ── MBR partition table reader ────────────────────────────────────────── */

int ata_read_partitions(uint8_t drive_idx, mbr_partition_t parts[4]) {
    uint8_t mbr[ATA_SECTOR_SIZE];

    if (ata_read_sectors(drive_idx, 0, 1, mbr) != 0)
        return -1;

    /* Check MBR boot signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return -2;

    /* Partition table starts at offset 446, four 16-byte entries */
    /* memcpy(source, dest, n) — note: CoolFrost's memcpy has source first */
    memcpy((uint8_t *)&mbr[446], (uint8_t *)parts, sizeof(mbr_partition_t) * 4);

    return 0;
}

const char *partition_type_name(uint8_t type) {
    switch (type) {
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/HPFS";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Extended LBA";
        case 0x11: return "Hidden FAT12";
        case 0x14: return "Hidden FAT16 <32M";
        case 0x16: return "Hidden FAT16";
        case 0x1B: return "Hidden FAT32";
        case 0x1C: return "Hidden FAT32 LBA";
        case 0x1E: return "Hidden FAT16 LBA";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x85: return "Linux extended";
        case 0x8E: return "Linux LVM";
        case 0xEE: return "GPT Protective";
        case 0xEF: return "EFI System";
        default:   return "Unknown";
    }
}
