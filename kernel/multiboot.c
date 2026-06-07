#include <stdint.h>
#include "multiboot.h"

mb2_tag_t *mb2_find_tag(mb2_info_t *info, uint32_t tag_type) {
    uint8_t *p = (uint8_t *)info + 8;  /* skip 8-byte header */
    uint8_t *end = (uint8_t *)info + info->total_size;

    while (p < end) {
        mb2_tag_t *tag = (mb2_tag_t *)p;
        if (tag->type == MB2_TAG_END)
            break;
        if (tag->type == tag_type)
            return tag;
        /* tags are 8-byte aligned */
        p += (tag->size + 7) & ~7u;
    }
    return (mb2_tag_t *)0;
}

uint64_t mb_get_lower_mem(mb2_info_t *info) {
    mb2_tag_basic_mem_t *t = (mb2_tag_basic_mem_t *)mb2_find_tag(info, MB2_TAG_BASIC_MEM);
    if (!t) return 0;
    return (uint64_t)t->mem_lower * 1024;
}

uint64_t mb_get_upper_mem(mb2_info_t *info) {
    mb2_tag_basic_mem_t *t = (mb2_tag_basic_mem_t *)mb2_find_tag(info, MB2_TAG_BASIC_MEM);
    if (!t) return 0;
    return (uint64_t)t->mem_upper * 1024 + 0x100000ULL;
}

char *mb_get_bootloader(mb2_info_t *info) {
    mb2_tag_string_t *t = (mb2_tag_string_t *)mb2_find_tag(info, MB2_TAG_BOOTLOADER);
    if (!t) return (char *)0;
    return t->string;
}

int mb_get_framebuffer(mb2_info_t *info, fb_info_t *out) {
    mb2_tag_framebuffer_t *t =
        (mb2_tag_framebuffer_t *)mb2_find_tag(info, MB2_TAG_FRAMEBUFFER);
    if (!t) return -1;
    out->addr   = t->framebuffer_addr;
    out->pitch  = t->framebuffer_pitch;
    out->width  = t->framebuffer_width;
    out->height = t->framebuffer_height;
    out->bpp    = t->framebuffer_bpp;
    out->type   = t->framebuffer_type;
    return 0;
}
