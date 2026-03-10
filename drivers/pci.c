#include <stdint.h>
#include "../cpu/ports.h"
#include "pci.h"

// Took from OSDEV: https://wiki.osdev.org/PCI#Configuration_Space_Access_Mechanism_#1
// offset is in bytes - just a note for me ;)
uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    uint16_t tmp = 0;
  
    // Create configuration address as per Figure 1
    address = (uint32_t)((lbus << 16) | (lslot << 11) |
              (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
  
    // Write out the address
    port_dword_out(0xCF8, address);
    // Read in the data
    // (offset & 2) * 8) = 0 will choose the first word of the 32-bit register
    tmp = (uint16_t)((port_dword_in(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
    return tmp;
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func <<  8)
                     | (offset & 0xFC);

    port_dword_out(0xCF8, address);
    return port_dword_in(0xCFC);
}

// 0xFFFF - invalid vendor - non-existent device
uint16_t pci_get_vendor(uint8_t bus, uint8_t slot) {
    return pci_config_read_word(bus, slot, 0, 0);
}

uint16_t pci_get_device(uint8_t bus, uint8_t slot) {
    uint32_t vendor_device = pci_config_read_dword(bus, slot, 0, 0);
    uint16_t vendor = (uint16_t)(vendor_device & 0xFFFF);
    if (vendor != 0xFFFF) {
       return (uint16_t)((vendor_device >> 16) & 0xFFFF);
    }
    return vendor;
}

// Doesn't check for invalid vendor!
uint8_t pci_get_class_code(uint8_t bus, uint8_t slot) {
    return (uint8_t)((pci_config_read_word(bus, slot, 0, 10) >> 8) & 0xFF);
}

// Doesn't check for invalid vendor!
uint8_t pci_get_subclass(uint8_t bus, uint8_t slot) {
    return (uint8_t)(pci_config_read_word(bus, slot, 0, 10) & 0xFF);
}

// Doesn't check for invalid vendor!
uint8_t pci_get_progif(uint8_t bus, uint8_t slot) {
    return (uint8_t)((pci_config_read_word(bus, slot, 0, 8) >> 8) & 0xFF);
}

// Doesn't check for invalid vendor!
uint8_t pci_get_revision(uint8_t bus, uint8_t slot) {
    return (uint8_t)(pci_config_read_word(bus, slot, 0, 8) & 0xFF);
}

void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func <<  8)
                     | (offset & 0xFC);
    port_dword_out(0xCF8, address);
    port_dword_out(0xCFC, value);
}

uint8_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id) {
    /* Check capabilities list bit in status register (offset 0x06, bit 4) */
    uint16_t status = pci_config_read_word(bus, slot, func, 0x06);
    if (!(status & (1 << 4))) return 0;

    /* Capabilities pointer is at offset 0x34 (bits [7:0]) */
    uint8_t cap_ptr = (uint8_t)(pci_config_read_dword(bus, slot, func, 0x34) & 0xFF);
    cap_ptr &= 0xFC; /* align to dword */

    uint8_t visited = 0;
    while (cap_ptr && visited < 48) {
        uint32_t cap_dw = pci_config_read_dword(bus, slot, func, cap_ptr);
        uint8_t  id     = (uint8_t)(cap_dw & 0xFF);
        uint8_t  next   = (uint8_t)((cap_dw >> 8) & 0xFF);
        if (id == cap_id) return cap_ptr;
        cap_ptr = next & 0xFC;
        visited++;
    }
    return 0;
}

pci_base_device_header_t pci_get_base_device_header(uint8_t bus, uint8_t slot) {
    if (pci_get_vendor(bus, slot) == 0xFFFF)
        return (pci_base_device_header_t){.vendor = 0xFFFF};

    uint32_t result[4];
    result[0] = pci_config_read_dword(bus, slot, 0, 0x0);
    result[1] = pci_config_read_dword(bus, slot, 0, 0x4);
    result[2] = pci_config_read_dword(bus, slot, 0, 0x8);
    result[3] = pci_config_read_dword(bus, slot, 0, 0xC);

    return *(pci_base_device_header_t *)result;
}
