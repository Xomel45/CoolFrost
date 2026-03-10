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

    /* 28-bit LBA sector count: words 60-61 */
    drv->sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
    /* 48-bit LBA sector count: words 100-103 (if supported) */
    if (identify[83] & (1 << 10)) {
        drv->sectors = (uint64_t)identify[100]
                     | ((uint64_t)identify[101] << 16)
                     | ((uint64_t)identify[102] << 32)
                     | ((uint64_t)identify[103] << 48);
    }

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
        uint64_t mb = d->sectors / 2048; /* sectors × 512 / 1048576 */
        printf("ata%d: %s  %lu MB  (%lu sectors)\n",
                i, d->model, mb, d->sectors);
    }
}

/* ATA command constants for 48-bit LBA */
#define ATA_CMD_READ_PIO_EXT    0x24
#define ATA_CMD_WRITE_PIO_EXT   0x34

int ata_read_sectors(uint8_t drive_idx, uint64_t lba, uint8_t count, void *buffer) {
    if (drive_idx >= num_drives)      return -1;
    if (!drives[drive_idx].present)   return -1;
    if (count == 0)                   return -1;

    ata_drive_t *drv = &drives[drive_idx];
    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slave_bit = drv->is_slave ? 0x10 : 0x00;

    if (ata_wait_bsy(io) != 0) return -1;

    if (lba >= 0x10000000ULL) {
        /* 48-bit LBA (READ SECTORS EXT) */
        port_byte_out(io + ATA_REG_DRIVE, 0x40 | slave_bit);
        ata_io_delay(ctrl);
        /* Write high bytes first ("Previous" content of registers) */
        port_byte_out(io + ATA_REG_SECCOUNT, 0);                          /* count high  */
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)((lba >> 24) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 32) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 40) & 0xFF));
        /* Write low bytes ("Current" content of registers) */
        port_byte_out(io + ATA_REG_SECCOUNT, count);
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
        port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
    } else {
        /* 28-bit LBA (READ SECTORS) */
        port_byte_out(io + ATA_REG_DRIVE,
                      0xE0 | slave_bit | ((lba >> 24) & 0x0F));
        ata_io_delay(ctrl);
        port_byte_out(io + ATA_REG_SECCOUNT, count);
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
        port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    uint16_t *buf = (uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq(io) != 0)
            return -1;
        for (int w = 0; w < 256; w++)
            buf[s * 256 + w] = port_word_in(io + ATA_REG_DATA);
        ata_io_delay(ctrl);
    }

    return 0;
}

int ata_write_sectors(uint8_t drive_idx, uint64_t lba, uint8_t count, const void *buffer) {
    if (drive_idx >= num_drives)      return -1;
    if (!drives[drive_idx].present)   return -1;
    if (count == 0)                   return -1;

    ata_drive_t *drv = &drives[drive_idx];
    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slave_bit = drv->is_slave ? 0x10 : 0x00;

    if (ata_wait_bsy(io) != 0) return -1;

    if (lba >= 0x10000000ULL) {
        /* 48-bit LBA (WRITE SECTORS EXT) */
        port_byte_out(io + ATA_REG_DRIVE, 0x40 | slave_bit);
        ata_io_delay(ctrl);
        port_byte_out(io + ATA_REG_SECCOUNT, 0);
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)((lba >> 24) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 32) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 40) & 0xFF));
        port_byte_out(io + ATA_REG_SECCOUNT, count);
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
        port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);
    } else {
        /* 28-bit LBA (WRITE SECTORS) */
        port_byte_out(io + ATA_REG_DRIVE,
                      0xE0 | slave_bit | ((lba >> 24) & 0x0F));
        ata_io_delay(ctrl);
        port_byte_out(io + ATA_REG_SECCOUNT, count);
        port_byte_out(io + ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
        port_byte_out(io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        port_byte_out(io + ATA_REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
        port_byte_out(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    }

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

/* ── GUID helpers ──────────────────────────────────────────────────────── */

int guid_is_zero(const guid_t *g) {
    return g->data1 == 0 && g->data2 == 0 && g->data3 == 0
        && g->data4[0] == 0 && g->data4[1] == 0 && g->data4[2] == 0
        && g->data4[3] == 0 && g->data4[4] == 0 && g->data4[5] == 0
        && g->data4[6] == 0 && g->data4[7] == 0;
}

static int guid_eq(const guid_t *a, const guid_t *b) {
    if (a->data1 != b->data1) return 0;
    if (a->data2 != b->data2) return 0;
    if (a->data3 != b->data3) return 0;
    for (int i = 0; i < 8; i++)
        if (a->data4[i] != b->data4[i]) return 0;
    return 1;
}

/* Well-known GPT partition type GUIDs (stored in mixed-endian per UEFI spec) */
static const guid_t GUID_EFI_SYSTEM       = {0xC12A7328, 0xF81F, 0x11D2, {0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B}};
static const guid_t GUID_MS_BASIC_DATA    = {0xEBD0A0A2, 0xB9E5, 0x4433, {0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7}};
static const guid_t GUID_LINUX_FS         = {0x0FC63DAF, 0x8483, 0x4772, {0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4}};
static const guid_t GUID_LINUX_SWAP       = {0x0657FD6D, 0xA4AB, 0x43C4, {0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F}};
static const guid_t GUID_LINUX_LVM        = {0xE6D6D379, 0xF507, 0x44C2, {0xA2,0x3C,0x23,0x8F,0x2A,0x3D,0xF9,0x28}};
static const guid_t GUID_MS_RESERVED      = {0xE3C9E316, 0x0B5C, 0x4DB8, {0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE}};
static const guid_t GUID_BIOS_BOOT        = {0x21686148, 0x6449, 0x6E6F, {0x74,0x4E,0x65,0x65,0x64,0x45,0x46,0x49}};

const char *gpt_type_name(const guid_t *type) {
    if (guid_is_zero(type))               return "Empty";
    if (guid_eq(type, &GUID_EFI_SYSTEM))  return "EFI System";
    if (guid_eq(type, &GUID_MS_BASIC_DATA)) return "Microsoft Basic Data";
    if (guid_eq(type, &GUID_LINUX_FS))    return "Linux filesystem";
    if (guid_eq(type, &GUID_LINUX_SWAP))  return "Linux swap";
    if (guid_eq(type, &GUID_LINUX_LVM))   return "Linux LVM";
    if (guid_eq(type, &GUID_MS_RESERVED)) return "Microsoft Reserved";
    if (guid_eq(type, &GUID_BIOS_BOOT))   return "BIOS Boot";
    return "Unknown";
}

/* ── Partition scheme detection ───────────────────────────────────────── */

uint8_t ata_detect_scheme(uint8_t drive_idx) {
    uint8_t mbr[ATA_SECTOR_SIZE];
    if (ata_read_sectors(drive_idx, 0, 1, mbr) != 0)
        return PART_SCHEME_NONE;

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return PART_SCHEME_NONE;

    /* Check if first partition is GPT protective (type 0xEE) */
    mbr_partition_t *parts = (mbr_partition_t *)&mbr[446];
    for (int i = 0; i < 4; i++) {
        if (parts[i].type == 0xEE)
            return PART_SCHEME_GPT;
    }

    return PART_SCHEME_MBR;
}

/* ── GPT reading ──────────────────────────────────────────────────────── */

int ata_read_gpt(uint8_t drive_idx, gpt_partition_entry_t *entries, int max) {
    /* Read GPT header at LBA 1 */
    uint8_t hdr_buf[ATA_SECTOR_SIZE];
    if (ata_read_sectors(drive_idx, 1, 1, hdr_buf) != 0)
        return -1;

    gpt_header_t *hdr = (gpt_header_t *)hdr_buf;

    /* Validate signature "EFI PART" */
    if (hdr->signature[0] != 'E' || hdr->signature[1] != 'F' ||
        hdr->signature[2] != 'I' || hdr->signature[3] != ' ' ||
        hdr->signature[4] != 'P' || hdr->signature[5] != 'A' ||
        hdr->signature[6] != 'R' || hdr->signature[7] != 'T')
        return -2;

    uint32_t num_entries  = hdr->num_partition_entries;
    uint32_t entry_size   = hdr->partition_entry_size;
    uint64_t entry_lba    = hdr->partition_entry_lba;

    if (entry_size < 128 || entry_size > 512)
        return -3;  /* unsupported entry size */

    /* Clamp to max */
    if (num_entries > (uint32_t)max)
        num_entries = (uint32_t)max;

    /* How many entries fit per sector */
    uint32_t entries_per_sector = ATA_SECTOR_SIZE / entry_size;
    int found = 0;
    uint8_t sec_buf[ATA_SECTOR_SIZE];

    for (uint32_t i = 0; i < num_entries; i++) {
        uint64_t sec_idx   = i / entries_per_sector;
        uint32_t sec_off   = (i % entries_per_sector) * entry_size;

        if (sec_off == 0) {
            /* Read next sector of entries */
            if (ata_read_sectors(drive_idx, entry_lba + sec_idx, 1, sec_buf) != 0)
                return -1;
        }

        gpt_partition_entry_t *pe = (gpt_partition_entry_t *)&sec_buf[sec_off];

        /* Skip empty entries */
        if (guid_is_zero(&pe->type_guid))
            continue;

        /* memcpy(source, dest, n) — CoolFrost non-standard order */
        memcpy((uint8_t *)pe, (uint8_t *)&entries[found], (int)sizeof(gpt_partition_entry_t));
        found++;
    }

    return found;
}

/* ── MBR partition type names ─────────────────────────────────────────── */

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
