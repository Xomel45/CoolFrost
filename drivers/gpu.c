#include "gpu.h"
#include "pci.h"

/* ── BAR reading & size probing ─────────────────────────────────────────── */

/* Config-space offsets for BAR0-BAR5 */
static const uint8_t bar_offsets[6] = { 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24 };

/*
 * Returns the base address of BARn (32-bit or lower 32 bits of 64-bit).
 * Memory BARs have bit[0]=0; I/O BARs have bit[0]=1.
 */
static uint32_t read_bar(uint8_t bus, uint8_t slot, uint8_t bar) {
    return pci_config_read_dword(bus, slot, 0, bar_offsets[bar]);
}

/*
 * Probes the size of BARn by the standard write-0xFFFFFFFF / read-back trick.
 * Returns 0 for I/O BARs or if the BAR is unimplemented.
 * For 64-bit BARs only the lower 32 bits of the size are returned.
 */
static uint64_t probe_bar_size(uint8_t bus, uint8_t slot, uint8_t bar) {
    uint8_t  off     = bar_offsets[bar];
    uint32_t orig    = pci_config_read_dword(bus, slot, 0, off);

    /* I/O BAR — skip */
    if (orig & 1) return 0;

    /* Write all-ones, read size mask, restore */
    pci_config_write_dword(bus, slot, 0, off, 0xFFFFFFFF);
    uint32_t mask = pci_config_read_dword(bus, slot, 0, off);
    pci_config_write_dword(bus, slot, 0, off, orig);

    if (mask == 0 || mask == 0xFFFFFFFF) return 0;

    /* Lower 4 bits are attribute flags — mask them out, then negate */
    mask &= 0xFFFFFFF0;
    return (uint64_t)(~mask) + 1;
}

/* ── Vendor / type name helpers ─────────────────────────────────────────── */

static const char *vendor_name(uint16_t vid) {
    switch (vid) {
        case GPU_VENDOR_NVIDIA: return "NVIDIA";
        case GPU_VENDOR_AMD:    return "AMD";
        case GPU_VENDOR_INTEL:  return "Intel";
        case GPU_VENDOR_VMWARE: return "VMware";
        default:                return "Unknown";
    }
}

static const char *display_type(uint8_t subclass) {
    switch (subclass) {
        case 0x00: return "VGA compatible";
        case 0x01: return "XGA";
        case 0x02: return "3D (non-VGA)";
        default:   return "Display controller";
    }
}

/* ── PCIe link status ────────────────────────────────────────────────────── */

/*
 * PCIe Link Status register is at PCIe capability offset + 0x12.
 *   Bits [3:0]  — Current Link Speed  (1=Gen1, 2=Gen2, 3=Gen3, 4=Gen4)
 *   Bits [9:4]  — Negotiated Link Width
 */
static void read_pcie_link(uint8_t bus, uint8_t slot, uint8_t cap_off,
                           uint8_t *out_gen, uint8_t *out_width)
{
    /* Link Status is the upper word of the dword at cap_off + 0x10 */
    uint32_t dw   = pci_config_read_dword(bus, slot, 0, cap_off + 0x10);
    uint16_t lsts = (uint16_t)(dw >> 16);

    uint8_t spd = lsts & 0x0F;
    uint8_t wid = (lsts >> 4) & 0x3F;

    /* Speed: 1-4 are defined; clamp anything else to 0 (unknown) */
    *out_gen   = (spd >= 1 && spd <= 4) ? spd : 0;
    *out_width = wid;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int gpu_scan(gpu_info_t *out, int max_count) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256 && found < max_count; bus++) {
        for (uint8_t slot = 0; slot < 32 && found < max_count; slot++) {
            uint16_t vid = pci_get_vendor((uint8_t)bus, slot);
            if (vid == 0xFFFF) continue;

            uint8_t cls = pci_get_class_code((uint8_t)bus, slot);
            if (cls != PCI_CLASS_DISPLAY) continue;

            gpu_info_t *g = &out[found++];
            g->bus        = (uint8_t)bus;
            g->slot       = slot;
            g->vendor_id  = vid;
            g->device_id  = pci_get_device((uint8_t)bus, slot);
            g->subclass   = pci_get_subclass((uint8_t)bus, slot);
            g->vendor_name = vendor_name(vid);
            g->type_name   = display_type(g->subclass);

            /* BAR0 */
            uint32_t bar0 = read_bar((uint8_t)bus, slot, 0);
            if (bar0 & 1) {
                /* I/O BAR — base is bits [31:2] */
                g->bar0_base = bar0 & ~(uint32_t)0x3;
                g->bar0_size = 0;
            } else {
                /* Memory BAR — base is bits [31:4] */
                g->bar0_base = bar0 & ~(uint32_t)0xF;
                /* For 64-bit BARs combine with BAR1 */
                if (((bar0 >> 1) & 0x3) == 2) {
                    uint32_t bar1 = read_bar((uint8_t)bus, slot, 1);
                    g->bar0_base |= (uint64_t)bar1 << 32;
                }
                g->bar0_size = probe_bar_size((uint8_t)bus, slot, 0);
            }

            /* PCIe capability */
            uint8_t cap_off = pci_find_cap((uint8_t)bus, slot, 0, PCI_CAP_ID_PCIE);
            if (cap_off) {
                g->is_pcie = 1;
                read_pcie_link((uint8_t)bus, slot, cap_off,
                               &g->pcie_gen, &g->pcie_width);
            } else {
                g->is_pcie    = 0;
                g->pcie_gen   = 0;
                g->pcie_width = 0;
            }
        }
    }

    return found;
}
