#ifndef GPU_H
#define GPU_H

#include <stdint.h>

/* PCI class for display controllers */
#define PCI_CLASS_DISPLAY   0x03

/* Known GPU vendors */
#define GPU_VENDOR_NVIDIA   0x10DE
#define GPU_VENDOR_AMD      0x1002
#define GPU_VENDOR_INTEL    0x8086
#define GPU_VENDOR_VMWARE   0x15AD

/* PCIe capability ID */
#define PCI_CAP_ID_PCIE     0x10

#define MAX_GPUS 4

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  subclass;   /* 0x00=VGA, 0x02=3D, 0x80=other */
    const char *vendor_name;
    const char *type_name;
    uint64_t bar0_base;  /* BAR0 physical base address (memory) */
    uint64_t bar0_size;  /* BAR0 region size in bytes, 0 = unknown/IO */
    int      is_pcie;    /* 1 if PCIe capability found */
    uint8_t  pcie_gen;   /* link speed: 1-4 (0 = unknown) */
    uint8_t  pcie_width; /* negotiated lane width: 1,2,4,8,16 (0 = unknown) */
} gpu_info_t;

/* Scan all PCI buses for display-class devices.
 * Fills `out[0..count-1]` and returns the number found (up to max_count). */
int gpu_scan(gpu_info_t *out, int max_count);

#endif
