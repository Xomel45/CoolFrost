#include "ugfx.h"
#include "usyscall.h"

#define SECTEXT __attribute__((section(".user_text")))
#define SECBSS  __attribute__((section(".user_bss")))
#define SECDATA __attribute__((section(".user_data")))

/* ── Backbuffer + font cache (filled once by ugfx_init) ─────────────────── */
static uint32_t       backbuf_px[UGFX_MAX_W * UGFX_MAX_H] SECBSS;
static ugfx_surface_t screen_surf SECBSS;
static uint8_t         font_table[256 * UGFX_FONT_H] SECBSS;
static int             ugfx_ready SECBSS;

/* ── Window-surface pool (bump-allocated, mirrors cpu/sched.c's ring3 stack
 * pool — a handful of long-lived windows, never freed) ─────────────────── */
/* GCC always emits __attribute__((section(...))) globals as PROGBITS (real
 * zero bytes in the file), even for a plain uninitialized array — unlike
 * the ordinary .bss GCC picks for unattributed statics. So every byte
 * reserved here adds directly to kernel.elf's size; keep the pool modest. */
#define UGFX_POOL_COUNT 6
#define UGFX_POOL_W     400
#define UGFX_POOL_H     300
static uint32_t       pool_px[UGFX_POOL_COUNT][UGFX_POOL_W * UGFX_POOL_H] SECBSS;
static ugfx_surface_t pool_surf[UGFX_POOL_COUNT] SECBSS;
static int             pool_next SECBSS;

SECTEXT
ugfx_surface_t *ugfx_init(void) {
    if (ugfx_ready) return &screen_surf;

    uint64_t info = usyscall0(SYS_FB_INFO);
    if (!info) return (ugfx_surface_t *)0;

    int32_t w = (int32_t)(info >> 32);
    int32_t h = (int32_t)(info & 0xFFFFFFFFu);
    if (w <= 0 || h <= 0 || w > UGFX_MAX_W || h > UGFX_MAX_H)
        return (ugfx_surface_t *)0;

    usyscall1(SYS_GET_FONT, (uint64_t)(uintptr_t)font_table);

    screen_surf.w  = w;
    screen_surf.h  = h;
    screen_surf.px = backbuf_px;
    ugfx_ready = 1;
    return &screen_surf;
}

SECTEXT
ugfx_surface_t *ugfx_surface_from_pool(int32_t w, int32_t h) {
    if (pool_next >= UGFX_POOL_COUNT) return (ugfx_surface_t *)0;
    if (w > UGFX_POOL_W) w = UGFX_POOL_W;
    if (h > UGFX_POOL_H) h = UGFX_POOL_H;

    ugfx_surface_t *s = &pool_surf[pool_next];
    s->w  = w;
    s->h  = h;
    s->px = pool_px[pool_next];
    pool_next++;
    return s;
}

SECTEXT
static int clip_rect(const ugfx_surface_t *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    if (*w <= 0 || *h <= 0) return 0;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= s->w || *y >= s->h) return 0;
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return (*w > 0 && *h > 0);
}

SECTEXT
void ugfx_fill(ugfx_surface_t *s, uint32_t color) {
    if (!s) return;
    int32_t n = s->w * s->h;
    for (int32_t i = 0; i < n; i++) s->px[i] = color;
}

SECTEXT
void ugfx_fill_rect(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!s || !clip_rect(s, &x, &y, &w, &h)) return;
    for (int32_t j = 0; j < h; j++) {
        uint32_t *row = s->px + (size_t)(y + j) * s->w + x;
        for (int32_t i = 0; i < w; i++) row[i] = color;
    }
}

SECTEXT
void ugfx_hline(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, uint32_t color) {
    ugfx_fill_rect(s, x, y, w, 1, color);
}

SECTEXT
void ugfx_vline(ugfx_surface_t *s, int32_t x, int32_t y, int32_t h, uint32_t color) {
    ugfx_fill_rect(s, x, y, 1, h, color);
}

SECTEXT
void ugfx_draw_rect(ugfx_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    ugfx_hline(s, x, y,         w, color);
    ugfx_hline(s, x, y + h - 1, w, color);
    ugfx_vline(s, x,         y, h, color);
    ugfx_vline(s, x + w - 1, y, h, color);
}

SECTEXT
void ugfx_blit(ugfx_surface_t *dst, int32_t dx, int32_t dy, const ugfx_surface_t *src) {
    if (!dst || !src) return;
    int32_t sx = 0, sy = 0, w = src->w, h = src->h;

    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;

    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++)
            dst->px[(size_t)(dy + j) * dst->w + dx + i] =
                src->px[(size_t)(sy + j) * src->w + sx + i];
}

SECTEXT
void ugfx_blit_rect(ugfx_surface_t *dst, int32_t dx, int32_t dy,
                    const ugfx_surface_t *src, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
    if (!dst || !src) return;

    /* Clip the source rect to the source surface first. */
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx >= src->w || sy >= src->h || sw <= 0 || sh <= 0) return;
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;

    /* Then clip the destination placement, shrinking the source rect in step. */
    if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;
    if (sw <= 0 || sh <= 0) return;

    for (int32_t j = 0; j < sh; j++)
        for (int32_t i = 0; i < sw; i++)
            dst->px[(size_t)(dy + j) * dst->w + dx + i] =
                src->px[(size_t)(sy + j) * src->w + sx + i];
}

SECTEXT
void ugfx_draw_char(ugfx_surface_t *s, int32_t x, int32_t y, unsigned char ch, uint32_t fg, uint32_t bg) {
    if (!s) return;
    const uint8_t *glyph = &font_table[(size_t)ch * UGFX_FONT_H];
    for (int32_t j = 0; j < UGFX_FONT_H; j++) {
        int32_t py = y + j;
        if (py < 0 || py >= s->h) continue;
        uint8_t bits = glyph[j];
        for (int32_t i = 0; i < UGFX_FONT_W; i++) {
            int32_t px = x + i;
            if (px < 0 || px >= s->w) continue;
            if (bits & (0x80u >> i))
                s->px[(size_t)py * s->w + px] = fg;
            else if (bg != UGFX_TRANSPARENT)
                s->px[(size_t)py * s->w + px] = bg;
        }
    }
}

SECTEXT
void ugfx_draw_text(ugfx_surface_t *s, int32_t x, int32_t y, const char *text, uint32_t fg, uint32_t bg) {
    for (; *text; text++) {
        ugfx_draw_char(s, x, y, (unsigned char)*text, fg, bg);
        x += UGFX_FONT_W;
    }
}

/* 12x19 arrow, 0=skip 1=black 2=white — same sprite as gfx/gfx.c's cursor,
 * duplicated here since ring3 can't call into kernel .text to share it. */
static const uint8_t cursor_map[19][12] SECDATA = {
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

SECTEXT
void ugfx_draw_cursor(ugfx_surface_t *s, int32_t x, int32_t y) {
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

SECTEXT
void ugfx_present(void) {
    if (!ugfx_ready) return;
    usyscall1(SYS_GFX_PRESENT, (uint64_t)(uintptr_t)backbuf_px);
}

SECTEXT
void ugfx_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!ugfx_ready) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x > 0xFFFF || y > 0xFFFF || w > 0xFFFF || h > 0xFFFF) { ugfx_present(); return; }

    uint64_t packed = ((uint64_t)(uint16_t)x << 48) | ((uint64_t)(uint16_t)y << 32) |
                      ((uint64_t)(uint16_t)w << 16) | (uint64_t)(uint16_t)h;
    usyscall2(SYS_GFX_PRESENT_RECT, (uint64_t)(uintptr_t)backbuf_px, packed);
}
