#include "window.h"

#define SECTEXT __attribute__((section(".user_text")))
#define SECBSS  __attribute__((section(".user_bss")))

static window_t win_pool[WM_MAX_WIN] SECBSS;
static int      win_pool_next SECBSS;

SECTEXT
window_t *window_create(int32_t x, int32_t y, int32_t w, int32_t h, const char *title,
                        win_paint_fn on_paint, win_event_fn on_event, void *ctx) {
    if (win_pool_next >= WM_MAX_WIN) return (window_t *)0;

    ugfx_surface_t *surf = ugfx_surface_from_pool(w, h - WIN_TITLE_H);
    if (!surf) return (window_t *)0;

    window_t *win = &win_pool[win_pool_next++];
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = WIN_TITLE_H + surf->h;   /* clamped by the pool -> reflect actual size */
    win->surface  = surf;
    win->in_use   = 1;
    win->on_paint = on_paint;
    win->on_event = on_event;
    win->ctx      = ctx;
    win->next = win->prev = (window_t *)0;

    int i = 0;
    while (i < 23 && title[i]) { win->title[i] = title[i]; i++; }
    win->title[i] = '\0';

    return win;
}

SECTEXT
win_hit_t window_hit_test(const window_t *win, int32_t x, int32_t y) {
    if (x < win->x || x >= win->x + win->w || y < win->y || y >= win->y + win->h)
        return HIT_NONE;

    int32_t rx = x - win->x, ry = y - win->y;   /* window-relative */

    if (ry < WIN_TITLE_H) {
        if (rx >= win->w - WIN_CLOSE_SZ - 4 && rx < win->w - 4 &&
            ry >= 4 && ry < 4 + WIN_CLOSE_SZ)
            return HIT_CLOSE;
        return HIT_TITLE;
    }

    int on_right  = rx >= win->w - WIN_BORDER;
    int on_bottom = ry >= win->h - WIN_BORDER;
    if (on_right && on_bottom) return HIT_RESIZE_BR;
    if (on_right)  return HIT_RESIZE_R;
    if (on_bottom) return HIT_RESIZE_B;

    return HIT_CLIENT;
}
