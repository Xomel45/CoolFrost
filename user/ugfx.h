#ifndef UGFX_H
#define UGFX_H

#include <stdint.h>
#include <stddef.h>

/* Ring3 counterpart of gfx/gfx.h — same API shape, but every primitive
 * lives in .user_text and only ever touches .user_bss memory, so the WM
 * compositor (user/wm.c) can redraw a whole frame's worth of primitives
 * without a syscall per fill/blit/glyph. The only syscall per frame is
 * SYS_GFX_PRESENT, which copies the finished backbuffer to the real
 * framebuffer (cpu/syscall.c) — that's the one piece of memory ring3 isn't
 * allowed to touch directly (kept U=0 on purpose). */

typedef struct {
    int32_t   w, h;
    uint32_t *px;
} ugfx_surface_t;

#define UGFX_TRANSPARENT 0x01000000u
#define UGFX_FONT_W 8
#define UGFX_FONT_H 16

/* Max backbuffer size — matches the Multiboot2 header's preferred mode
 * (boot/multiboot_entry.asm requests 1024x768). If the negotiated mode
 * ends up bigger, ugfx_init() fails rather than overrunning this buffer. */
#define UGFX_MAX_W 1024
#define UGFX_MAX_H 768

/* Fetches the real resolution (SYS_FB_INFO) and the font table (SYS_GET_FONT,
 * copied once), and sizes the static backbuffer to it. Returns the backbuffer
 * surface, or NULL if gfx isn't available or the resolution exceeds
 * UGFX_MAX_W/H. */
ugfx_surface_t *ugfx_init(void);

ugfx_surface_t *ugfx_surface_from_pool(int32_t w, int32_t h);

void ugfx_fill(ugfx_surface_t *s, uint32_t color);
void ugfx_fill_rect(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void ugfx_draw_rect(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void ugfx_hline(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, uint32_t color);
void ugfx_vline(ugfx_surface_t *s, int32_t x, int32_t y, int32_t h, uint32_t color);
void ugfx_blit(ugfx_surface_t *dst, int32_t dx, int32_t dy, const ugfx_surface_t *src);
/* Same as ugfx_blit, but copies only the (sx, sy, sw, sh) sub-rect of src —
 * e.g. a window shrunk below its (fixed, pool-allocated) surface size still
 * needs to blit just its current chrome size, not the whole backing buffer. */
void ugfx_blit_rect(ugfx_surface_t *dst, int32_t dx, int32_t dy,
                    const ugfx_surface_t *src, int32_t sx, int32_t sy, int32_t sw, int32_t sh);
void ugfx_draw_char(ugfx_surface_t *s, int32_t x, int32_t y, unsigned char ch, uint32_t fg, uint32_t bg);
void ugfx_draw_text(ugfx_surface_t *s, int32_t x, int32_t y, const char *text, uint32_t fg, uint32_t bg);
void ugfx_draw_cursor(ugfx_surface_t *s, int32_t x, int32_t y);

/* SYS_GFX_PRESENT: pushes the backbuffer returned by ugfx_init() to the FB. */
void ugfx_present(void);

#endif
