#include "gfx.h"
#include "../drivers/screen.h"
#include "../libc/mem.h"

/* ── State ──────────────────────────────────────────────────────────────── */
static screen_fb_params_t fbp;
static gfx_surface_t      backbuf;      /* pixels on the kernel heap */
static int                gfx_ready = 0;

int gfx_available(void) { return gfx_ready; }

gfx_surface_t *gfx_screen(void) { return gfx_ready ? &backbuf : 0; }

int gfx_init(void) {
    if (gfx_ready) return 0;
    if (screen_get_fb(&fbp) != 0) return -1;

    size_t bytes = (size_t)fbp.width * fbp.height * 4u;
    uint32_t *px = (uint32_t *)kmalloc(bytes, 0, 0);
    if (!px) return -1;

    backbuf.w  = (int32_t)fbp.width;
    backbuf.h  = (int32_t)fbp.height;
    backbuf.px = px;
    memset(px, 0, bytes);
    gfx_ready = 1;
    return 0;
}

/* ── Surfaces ───────────────────────────────────────────────────────────── */

gfx_surface_t *gfx_surface_create(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return 0;
    gfx_surface_t *s = (gfx_surface_t *)kmalloc(sizeof(gfx_surface_t), 0, 0);
    if (!s) return 0;
    s->px = (uint32_t *)kmalloc((size_t)w * h * 4u, 0, 0);
    if (!s->px) { kfree(s); return 0; }
    s->w = w;
    s->h = h;
    return s;
}

void gfx_surface_destroy(gfx_surface_t *s) {
    if (!s) return;
    kfree(s->px);
    kfree(s);
}

/* ── Clipping helper ────────────────────────────────────────────────────── *
 * Clamps a rect to surface bounds; returns 0 if nothing remains.           */
static int clip_rect(const gfx_surface_t *s, int32_t *x, int32_t *y,
                     int32_t *w, int32_t *h) {
    if (*w <= 0 || *h <= 0) return 0;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= s->w || *y >= s->h) return 0;
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return (*w > 0 && *h > 0);
}

/* ── Primitives ─────────────────────────────────────────────────────────── */

void gfx_fill(gfx_surface_t *s, uint32_t color) {
    if (!s) return;
    int32_t n = s->w * s->h;
    for (int32_t i = 0; i < n; i++) s->px[i] = color;
}

void gfx_fill_rect(gfx_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color) {
    if (!s || !clip_rect(s, &x, &y, &w, &h)) return;
    for (int32_t j = 0; j < h; j++) {
        uint32_t *row = s->px + (size_t)(y + j) * s->w + x;
        for (int32_t i = 0; i < w; i++) row[i] = color;
    }
}

void gfx_hline(gfx_surface_t *s, int32_t x, int32_t y, int32_t w, uint32_t color) {
    gfx_fill_rect(s, x, y, w, 1, color);
}

void gfx_vline(gfx_surface_t *s, int32_t x, int32_t y, int32_t h, uint32_t color) {
    gfx_fill_rect(s, x, y, 1, h, color);
}

void gfx_draw_rect(gfx_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color) {
    gfx_hline(s, x, y,         w, color);
    gfx_hline(s, x, y + h - 1, w, color);
    gfx_vline(s, x,         y, h, color);
    gfx_vline(s, x + w - 1, y, h, color);
}

void gfx_blit(gfx_surface_t *dst, int32_t dx, int32_t dy,
              const gfx_surface_t *src) {
    if (!dst || !src) return;
    int32_t sx = 0, sy = 0, w = src->w, h = src->h;

    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;

    for (int32_t j = 0; j < h; j++)
        memcpy(dst->px + (size_t)(dy + j) * dst->w + dx,
               src->px + (size_t)(sy + j) * src->w + sx,
               (size_t)w * 4u);
}

/* ── Text ───────────────────────────────────────────────────────────────── */

void gfx_draw_char(gfx_surface_t *s, int32_t x, int32_t y,
                   unsigned char ch, uint32_t fg, uint32_t bg) {
    if (!s) return;
    const uint8_t *glyph = screen_glyph8x16(ch);
    for (int32_t j = 0; j < FONT_H; j++) {
        int32_t py = y + j;
        if (py < 0 || py >= s->h) continue;
        uint8_t bits = glyph[j];
        for (int32_t i = 0; i < FONT_W; i++) {
            int32_t px = x + i;
            if (px < 0 || px >= s->w) continue;
            if (bits & (0x80u >> i))
                s->px[(size_t)py * s->w + px] = fg;
            else if (bg != GFX_TRANSPARENT)
                s->px[(size_t)py * s->w + px] = bg;
        }
    }
}

void gfx_draw_text(gfx_surface_t *s, int32_t x, int32_t y,
                   const char *text, uint32_t fg, uint32_t bg) {
    for (; *text; text++) {
        gfx_draw_char(s, x, y, (unsigned char)*text, fg, bg);
        x += FONT_W;
    }
}

/* ── Cursor sprite (12×19 arrow, 0=skip 1=black 2=white) ────────────────── */

static const uint8_t cursor_map[19][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

void gfx_draw_cursor(gfx_surface_t *s, int32_t x, int32_t y) {
    if (!s) return;
    for (int32_t j = 0; j < 19; j++) {
        int32_t py = y + j;
        if (py < 0 || py >= s->h) continue;
        for (int32_t i = 0; i < 12; i++) {
            int32_t px = x + i;
            if (px < 0 || px >= s->w) continue;
            uint8_t c = cursor_map[j][i];
            if (c == 1) s->px[(size_t)py * s->w + px] = 0x00000000;
            else if (c == 2) s->px[(size_t)py * s->w + px] = 0x00FFFFFF;
        }
    }
}

/* ── Presenting ─────────────────────────────────────────────────────────── */

void gfx_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!gfx_ready || !clip_rect(&backbuf, &x, &y, &w, &h)) return;
    for (int32_t j = 0; j < h; j++) {
        uint8_t *dst = fbp.base + (size_t)(y + j) * fbp.pitch + (size_t)x * 4u;
        const uint32_t *src = backbuf.px + (size_t)(y + j) * backbuf.w + x;
        memcpy(dst, src, (size_t)w * 4u);
    }
}

void gfx_present(void) {
    gfx_present_rect(0, 0, backbuf.w, backbuf.h);
}
