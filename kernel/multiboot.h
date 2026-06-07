#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Multiboot2 magic value passed in rdi to kernel_main */
#define MB2_MAGIC 0x36D76289

/* Tag types in the Multiboot2 info block */
#define MB2_TAG_END          0
#define MB2_TAG_CMDLINE      1
#define MB2_TAG_BOOTLOADER   2
#define MB2_TAG_BASIC_MEM    4
#define MB2_TAG_MMAP         6
#define MB2_TAG_FRAMEBUFFER  8

/* Framebuffer types reported by GRUB */
#define MB2_FB_TYPE_INDEXED  0
#define MB2_FB_TYPE_RGB      1
#define MB2_FB_TYPE_EGA_TEXT 2

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct __attribute__((packed)) {
    uint32_t type;   /* MB2_TAG_BASIC_MEM = 4 */
    uint32_t size;
    uint32_t mem_lower;   /* KB below 1MB */
    uint32_t mem_upper;   /* KB above 1MB */
} mb2_tag_basic_mem_t;

typedef struct __attribute__((packed)) {
    uint32_t type;   /* MB2_TAG_BOOTLOADER = 2 */
    uint32_t size;
    char     string[0];
} mb2_tag_string_t;

typedef struct __attribute__((packed)) {
    uint32_t type;   /* MB2_TAG_FRAMEBUFFER = 8 */
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} mb2_tag_framebuffer_t;

/* Multiboot2 info block header (first 8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

/* Convenience type stored in globals */
typedef struct {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type;   /* MB2_FB_TYPE_* */
} fb_info_t;

/* Walk tags and find one by type; returns NULL if not found */
mb2_tag_t *mb2_find_tag(mb2_info_t *info, uint32_t tag_type);

uint64_t   mb_get_lower_mem(mb2_info_t *info);
uint64_t   mb_get_upper_mem(mb2_info_t *info);
char      *mb_get_bootloader(mb2_info_t *info);
int        mb_get_framebuffer(mb2_info_t *info, fb_info_t *out);

#endif
