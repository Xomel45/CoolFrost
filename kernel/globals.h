#include <stdint.h>
#include "multiboot.h"
#include "../drivers/clock.h"

typedef struct {
    mb2_info_t        *multiboot_addr;
    fb_info_t          fb_info;
    datetime_t         datetime;
    uint8_t            cpuid_supported;
    char               cpu_manufacturer[13];
    char               cpu_full_name[49];
    char              *kernel_name;
    char              *kernel_version;
    char              *kernel_codename;
} kernel_globals;
