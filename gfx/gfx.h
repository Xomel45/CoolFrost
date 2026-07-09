#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  gfx — software 2D layer for the future DE/WM.
 *
 *  All drawing happens on gfx_surface_t (32-bit XRGB pixels).  The screen
 *  is represented by a heap-allocated backbuffer surface; gfx_present()
 *  copies it (or a dirty rectangle) to the real framebuffer.  Windows can
 *  be off-screen surfaces blitted onto the backbuffer by the compositor.
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t   w, h;
    uint32_t *px;      /* w*h packed XRGB, row-major */
} gfx_surface_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Allocates the backbuffer.  Returns 0 on success,
 * -1 if no 32bpp linear framebuffer is active (VGA text fallback). */
int  gfx_init(void);

/* 1 after successful gfx_init() */
int  gfx_available(void);

/* The screen backbuffer surface (NULL before gfx_init). */
gfx_surface_t *gfx_screen(void);

/* Off-screen surface on the kernel heap (e.g. window contents). */
gfx_surface_t *gfx_surface_create(int32_t w, int32_t h);
void           gfx_surface_destroy(gfx_surface_t *s);

/* ── Primitives (all clip against the target surface) ──────────────────── */

void gfx_fill(gfx_surface_t *s, uint32_t color);
void gfx_fill_rect(gfx_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color);
/* 1px outline */
void gfx_draw_rect(gfx_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color);
void gfx_hline(gfx_surface_t *s, int32_t x, int32_t y, int32_t w, uint32_t color);
void gfx_vline(gfx_surface_t *s, int32_t x, int32_t y, int32_t h, uint32_t color);

/* Copy src (whole) onto dst at (dx, dy), clipped to dst bounds. */
void gfx_blit(gfx_surface_t *dst, int32_t dx, int32_t dy,
              const gfx_surface_t *src);

/* 8×16 CP437 text.  bg = GFX_TRANSPARENT skips background pixels. */
#define GFX_TRANSPARENT 0x01000000u   /* impossible XRGB value (bit 24) */
void gfx_draw_char(gfx_surface_t *s, int32_t x, int32_t y,
                   unsigned char ch, uint32_t fg, uint32_t bg);
void gfx_draw_text(gfx_surface_t *s, int32_t x, int32_t y,
                   const char *text, uint32_t fg, uint32_t bg);

/* Arrow cursor sprite (drawn last, on the backbuffer, by the compositor). */
void gfx_draw_cursor(gfx_surface_t *s, int32_t x, int32_t y);

/* ── Presenting ─────────────────────────────────────────────────────────── */

/* Copy backbuffer → framebuffer: whole screen or a clipped rectangle. */
void gfx_present(void);
void gfx_present_rect(int32_t x, int32_t y, int32_t w, int32_t h);

#endif
