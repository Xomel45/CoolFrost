#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    unsigned int io_space : 1;
    unsigned int memory_space : 1;
    unsigned int bus_master : 1;
    unsigned int special_cycles : 1;
    unsigned int mem_wr_inv_enable : 1; // memory write and invalidate enable
    unsigned int vga_palette_snoop : 1;
    unsigned int parity_err_response : 1;
    unsigned int res1 : 1; // reserved 1
    unsigned int serr_enable : 1;
    unsigned int fast_b2b_enable : 1; // fast back-to-back enable
    unsigned int interrupt_disable : 1;
    unsigned int res2 : 5; // reserved 2
} pci_command_register_t;

typedef struct __attribute__((packed)) {
    unsigned int res1 : 3;
    unsigned int interrupt_status : 1;
    unsigned int capabilities_list : 1;
    unsigned int mhz_capable : 1; // 66 MHz capable
    unsigned int res2 : 1;
    unsigned int fast_b2b_capable : 1;
    unsigned int master_data_parity_err : 1;
    unsigned int devsel_timing : 2;
    unsigned int signaled_target_abort : 1;
    unsigned int received_target_abort : 1;
    unsigned int received_master_abort : 1;
    unsigned int signaled_system_err : 1;
    unsigned int detected_parity_err : 1;
} pci_status_register_t;

typedef struct __attribute__((packed)) {
    uint16_t vendor;
    uint16_t device_id;
    pci_command_register_t command;
    pci_status_register_t status;
    uint8_t revision_id;
    uint8_t progif;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
} pci_base_device_header_t;

typedef struct __attribute__((packed)) {
    pci_base_device_header_t base_header;
    uint32_t bar0;
    uint32_t bar1;
    uint32_t bar2;
    uint32_t bar3;
    uint32_t bar4;
    uint32_t bar5;
    uint32_t cardbus_cis_ptr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rom_base_addr;
    uint8_t capabilities_ptr;
    uint32_t res1 : 24;
    uint32_t res2;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
} pci_standard_device_header_t;

typedef struct __attribute__((packed)) {
    pci_base_device_header_t base_header;
    uint32_t bar0;
    uint32_t bar1;
    uint8_t primary_bus_num;
    uint8_t secondary_bus_num;
    uint8_t subordinate_bus_num;
    uint8_t secondary_latency_timer;
    uint8_t io_base;
    uint8_t io_limit;
    uint16_t secondary_status;
    uint16_t memory_base;
    uint16_t memory_limit;
    uint16_t pref_memory_base;
    uint16_t pref_memory_limit;
    uint32_t pref_base_upper_32;
    uint32_t pref_limit_upper_32;
    uint16_t io_base_upper_16;
    uint16_t io_limit_upper_16;
    uint8_t capability_ptr;
    uint32_t res1 : 24;
    uint32_t expansion_rom_base_addr;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint16_t bridge_control;
} pci_p2p_bridge_header_t;

typedef struct __attribute__((packed)) {
    pci_base_device_header_t base_header;
    uint32_t cardbus_socket;
    uint8_t offset_capabilities_list;
    uint8_t res1;
    uint16_t secondary_status;
    uint8_t pci_bus_num;
    uint8_t cardbus_bus_num;
    uint8_t subordinate_bus_num;
    uint8_t cardbus_latency_timer;
    uint32_t memory_base_addr0;
    uint32_t memory_limit0;
    uint32_t memory_base_addr1;
    uint32_t memory_limit1;
    uint32_t io_base_addr0;
    uint32_t io_limit0;
    uint32_t io_base_addr1;
    uint32_t io_limit1;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint16_t bridge_control;
    uint16_t subsystem_device_id;
    uint16_t subsystem_vendor_id;
    uint32_t /*16*/ bit_pc_card_legacy_mode_base_addr;
} pci_pci2cardbus_bridge_header_t;

uint16_t pci_config_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_get_vendor(uint8_t bus, uint8_t slot);
uint16_t pci_get_device(uint8_t bus, uint8_t slot);
uint8_t pci_get_class_code(uint8_t bus, uint8_t slot);
uint8_t pci_get_subclass(uint8_t bus, uint8_t slot);
uint8_t pci_get_progif(uint8_t bus, uint8_t slot);
uint8_t pci_get_revision(uint8_t bus, uint8_t slot);
pci_base_device_header_t pci_get_base_device_header(uint8_t bus, uint8_t slot);

/* Returns the config-space offset of capability `cap_id`, or 0 if not found.
 * Only works for header type 0 devices that advertise a capabilities list. */
uint8_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id);
#endif
