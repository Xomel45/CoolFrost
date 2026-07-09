#ifndef USER_WINDOW_H
#define USER_WINDOW_H

#include "ugfx.h"
#include "../ui/event.h"

#define WIN_TITLE_H  24
#define WIN_CLOSE_SZ 16
#define WIN_BORDER   6      /* resize grab margin along the right/bottom edge */
#define WM_MAX_WIN   6      /* matches UGFX_POOL_COUNT (user/ugfx.c) */

typedef struct window window_t;

typedef void (*win_paint_fn)(window_t *w, void *ctx);
typedef void (*win_event_fn)(window_t *w, void *ctx, const event_t *ev);

/* A window's (x, y, w, h) covers the whole chrome including the titlebar;
 * the client area is (w, h - WIN_TITLE_H) starting at (x, y + WIN_TITLE_H),
 * backed by `surface` (from ugfx_surface_from_pool — fixed max size, see
 * user/ugfx.c, so w/h are clamped to UGFX_POOL_W/H at creation). */
struct window {
    int32_t         x, y, w, h;
    char            title[24];
    ugfx_surface_t *surface;
    int             in_use;

    win_paint_fn    on_paint;   /* called each frame before compositing, draws into `surface` */
    win_event_fn    on_event;   /* client-area clicks/keys forwarded here when focused/hit */
    void           *ctx;

    window_t       *next, *prev;   /* z-order list; head (see wm.c) = topmost */
};

typedef enum {
    HIT_NONE = 0,
    HIT_TITLE,
    HIT_CLOSE,
    HIT_CLIENT,
    HIT_RESIZE_R,
    HIT_RESIZE_B,
    HIT_RESIZE_BR,
} win_hit_t;

/* Allocates from a static pool (WM_MAX_WIN slots, never freed — matches the
 * rest of this ring3 arena's no-malloc design). NULL if the pool or the
 * backing ugfx surface pool is exhausted. */
window_t *window_create(int32_t x, int32_t y, int32_t w, int32_t h, const char *title,
                        win_paint_fn on_paint, win_event_fn on_event, void *ctx);

win_hit_t window_hit_test(const window_t *win, int32_t x, int32_t y);

#endif
