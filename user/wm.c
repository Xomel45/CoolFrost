#include "wm.h"
#include "window.h"
#include "ugfx.h"
#include "usyscall.h"
#include "../drivers/mouse.h"

#define SECTEXT __attribute__((section(".user_text")))
#define SECBSS  __attribute__((section(".user_bss")))
#define SECDATA __attribute__((section(".user_data")))

/* String literals default to the kernel's .rodata (U=0, unreachable from
 * ring3) — anything a ring3 function dereferences at runtime must be a
 * named object explicitly placed in .user_data instead. */
static const char str_window_title[] SECDATA = "window";
static const char str_launch_label[] SECDATA  = "+ win";
static const char str_close_label[]  SECDATA  = "x";

#define TASKBAR_H   32
#define WIN_MIN_W   120
#define WIN_MIN_H   (WIN_TITLE_H + 60)
#define FRAME_MS    16

/* ── Z-order list: intrusive, head = topmost ─────────────────────────────── */
static window_t *z_head SECBSS;

SECTEXT
static void z_unlink(window_t *w) {
    if (w->prev) w->prev->next = w->next; else z_head = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = w->prev = (window_t *)0;
}

SECTEXT
static void z_push_front(window_t *w) {
    w->prev = (window_t *)0;
    w->next = z_head;
    if (z_head) z_head->prev = w;
    z_head = w;
}

SECTEXT
static void z_raise(window_t *w) {
    if (z_head == w) return;
    z_unlink(w);
    z_push_front(w);
}

SECTEXT
static int z_gather(window_t **out, int max) {
    int n = 0;
    for (window_t *w = z_head; w && n < max; w = w->next) out[n++] = w;
    return n;
}

/* ── Demo window content: a bouncing bar, proves live per-frame redraw
 * without needing a number->string formatter (not available in .user_text —
 * itoa lives in kernel libc, U=0). ─────────────────────────────────────── */
typedef struct { uint32_t color; uint64_t frames; } demo_ctx_t;
static demo_ctx_t demo_ctx_pool[WM_MAX_WIN] SECBSS;
static int         demo_ctx_next SECBSS;

static const uint32_t demo_palette[WM_MAX_WIN] __attribute__((section(".user_data"))) = {
    0x00E0A030, 0x00305090, 0x0030A050, 0x00A03050, 0x00A0A030, 0x0030A0A0,
};

SECTEXT
static void demo_on_paint(window_t *w, void *vctx) {
    demo_ctx_t *dc = (demo_ctx_t *)vctx;
    ugfx_fill(w->surface, 0x00E8E8E8);
    ugfx_draw_text(w->surface, 8, 8, w->title, 0x00202020, UGFX_TRANSPARENT);

    int32_t bar_w = 20, bar_h = 20;
    int32_t max_x = w->surface->w - bar_w - 8;
    if (max_x < 8) max_x = 8;
    int32_t period = (max_x > 8) ? (max_x - 8) * 2 : 2;
    int32_t phase  = (int32_t)(dc->frames % (uint64_t)period);
    int32_t bx     = (phase <= period / 2) ? (8 + phase) : (8 + period - phase);
    ugfx_fill_rect(w->surface, bx, 32, bar_w, bar_h, dc->color);

    dc->frames++;
}

SECTEXT
static window_t *spawn_demo_window(int32_t x, int32_t y) {
    if (demo_ctx_next >= WM_MAX_WIN) return (window_t *)0;
    demo_ctx_t *dc = &demo_ctx_pool[demo_ctx_next];
    dc->color  = demo_palette[demo_ctx_next];
    dc->frames = 0;
    demo_ctx_next++;

    window_t *w = window_create(x, y, 300, WIN_TITLE_H + 200, str_window_title, demo_on_paint, (win_event_fn)0, dc);
    if (!w) return (window_t *)0;
    z_push_front(w);
    return w;
}

/* ── Taskbar layout ───────────────────────────────────────────────────────
 * A launcher button on the left, then one button per open window. */
#define LAUNCH_BTN_W 64
#define WIN_BTN_W    110
#define BTN_GAP      4

SECTEXT
static void compose(ugfx_surface_t *scr, window_t **order, int n, int32_t mx, int32_t my) {
    ugfx_fill(scr, 0x001A3A52);

    int32_t tb_y = scr->h - TASKBAR_H;
    ugfx_fill_rect(scr, 0, tb_y, scr->w, TASKBAR_H, 0x00202020);
    ugfx_fill_rect(scr, 8, tb_y + 4, LAUNCH_BTN_W, TASKBAR_H - 8, 0x00305830);
    ugfx_draw_text(scr, 8 + 8, tb_y + 8, str_launch_label, 0x00FFFFFF, UGFX_TRANSPARENT);

    int32_t bx = 8 + LAUNCH_BTN_W + BTN_GAP;
    for (int i = n - 1; i >= 0; i--) {   /* taskbar order: creation order looks nicer than z-order */
        window_t *w = order[i];
        uint32_t bg = (w == z_head) ? 0x00405878 : 0x00303030;
        if (bx + WIN_BTN_W > scr->w) break;
        ugfx_fill_rect(scr, bx, tb_y + 4, WIN_BTN_W, TASKBAR_H - 8, bg);
        ugfx_draw_text(scr, bx + 4, tb_y + 8, w->title, 0x00E0E0E0, UGFX_TRANSPARENT);
        bx += WIN_BTN_W + BTN_GAP;
    }

    for (int k = n - 1; k >= 0; k--) {   /* paint back-to-front */
        window_t *w = order[k];
        if (w->on_paint) w->on_paint(w, w->ctx);

        uint32_t tcol = (w == z_head) ? 0x004070C0 : 0x00505860;
        ugfx_fill_rect(scr, w->x, w->y, w->w, WIN_TITLE_H, tcol);
        ugfx_draw_text(scr, w->x + 6, w->y + 4, w->title, 0x00FFFFFF, UGFX_TRANSPARENT);

        int32_t cx = w->x + w->w - WIN_CLOSE_SZ - 4, cy = w->y + 4;
        ugfx_fill_rect(scr, cx, cy, WIN_CLOSE_SZ, WIN_CLOSE_SZ, 0x00C04040);
        ugfx_draw_text(scr, cx + 4, cy - 2, str_close_label, 0x00FFFFFF, UGFX_TRANSPARENT);

        ugfx_blit_rect(scr, w->x, w->y + WIN_TITLE_H, w->surface,
                       0, 0, w->w, w->h - WIN_TITLE_H);
        ugfx_draw_rect(scr, w->x, w->y, w->w, w->h, 0x00101010);
    }

    ugfx_draw_cursor(scr, mx, my);
}

typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE_R, DRAG_RESIZE_B, DRAG_RESIZE_BR } drag_mode_t;

SECTEXT
void wm_run(void *arg) {
    (void)arg;

    ugfx_surface_t *scr = ugfx_init();
    if (!scr) return;   /* no 32bpp linear FB — nothing to do, SYS_EXIT via trampoline */

    int32_t mx = scr->w / 2, my = scr->h / 2;

    spawn_demo_window(scr->w / 2 - 320, 96);
    spawn_demo_window(scr->w / 2 + 16,  160);

    drag_mode_t drag = DRAG_NONE;
    window_t   *drag_win = (window_t *)0;
    int32_t     drag_ox = 0, drag_oy = 0;   /* MOVE: cursor - win pos, at drag start */
    int32_t     drag_sw = 0, drag_sh = 0;   /* RESIZE: win size at drag start */
    int32_t     drag_sx = 0, drag_sy = 0;   /* RESIZE: cursor pos at drag start */

    uint64_t last_tick = usyscall0(SYS_GET_TICKS);
    int running = 1;

    while (running) {
        event_t ev;
        while (usyscall1(SYS_EVENT_POLL, (uint64_t)(uintptr_t)&ev)) {
            switch (ev.type) {
            case EV_KEY_DOWN:
                if (ev.code == 0x01) running = 0;   /* ESC */
                break;

            case EV_MOUSE_MOVE:
                mx = ev.x; my = ev.y;
                if (drag == DRAG_MOVE) {
                    drag_win->x = mx - drag_ox;
                    drag_win->y = my - drag_oy;
                } else if (drag != DRAG_NONE) {
                    int32_t dw = mx - drag_sx, dh = my - drag_sy;
                    int32_t max_w = drag_win->surface->w;
                    int32_t max_h = WIN_TITLE_H + drag_win->surface->h;
                    if (drag == DRAG_RESIZE_R || drag == DRAG_RESIZE_BR) {
                        int32_t nw = drag_sw + dw;
                        if (nw < WIN_MIN_W) nw = WIN_MIN_W;
                        if (nw > max_w) nw = max_w;
                        drag_win->w = nw;
                    }
                    if (drag == DRAG_RESIZE_B || drag == DRAG_RESIZE_BR) {
                        int32_t nh = drag_sh + dh;
                        if (nh < WIN_MIN_H) nh = WIN_MIN_H;
                        if (nh > max_h) nh = max_h;
                        drag_win->h = nh;
                    }
                }
                break;

            case EV_MOUSE_DOWN:
                if (ev.code != MOUSE_BTN_LEFT) break;

                if (ev.y >= scr->h - TASKBAR_H) {
                    if (ev.x >= 8 && ev.x < 8 + LAUNCH_BTN_W) {
                        window_t *order[WM_MAX_WIN];
                        int n = z_gather(order, WM_MAX_WIN);
                        spawn_demo_window(40 + 24 * n, 80 + 20 * n);
                        break;
                    }
                    window_t *order[WM_MAX_WIN];
                    int n = z_gather(order, WM_MAX_WIN);
                    int32_t bx = 8 + LAUNCH_BTN_W + BTN_GAP;
                    for (int i = n - 1; i >= 0; i--) {
                        if (ev.x >= bx && ev.x < bx + WIN_BTN_W) { z_raise(order[i]); break; }
                        bx += WIN_BTN_W + BTN_GAP;
                    }
                    break;
                }

                {
                    window_t *order[WM_MAX_WIN];
                    int n = z_gather(order, WM_MAX_WIN);
                    for (int i = 0; i < n; i++) {
                        window_t *w = order[i];
                        win_hit_t hit = window_hit_test(w, ev.x, ev.y);
                        if (hit == HIT_NONE) continue;

                        z_raise(w);
                        switch (hit) {
                        case HIT_CLOSE:
                            z_unlink(w);
                            w->in_use = 0;
                            break;
                        case HIT_TITLE:
                            drag = DRAG_MOVE; drag_win = w;
                            drag_ox = ev.x - w->x; drag_oy = ev.y - w->y;
                            break;
                        case HIT_RESIZE_R: case HIT_RESIZE_B: case HIT_RESIZE_BR:
                            drag = (hit == HIT_RESIZE_R) ? DRAG_RESIZE_R :
                                  (hit == HIT_RESIZE_B) ? DRAG_RESIZE_B : DRAG_RESIZE_BR;
                            drag_win = w;
                            drag_sw = w->w; drag_sh = w->h;
                            drag_sx = ev.x; drag_sy = ev.y;
                            break;
                        case HIT_CLIENT:
                            if (w->on_event) w->on_event(w, w->ctx, &ev);
                            break;
                        default: break;
                        }
                        break;   /* topmost hit wins */
                    }
                }
                break;

            case EV_MOUSE_UP:
                if (ev.code == MOUSE_BTN_LEFT) { drag = DRAG_NONE; drag_win = (window_t *)0; }
                break;

            default: break;
            }
        }

        window_t *order[WM_MAX_WIN];
        int n = z_gather(order, WM_MAX_WIN);
        compose(scr, order, n, mx, my);
        ugfx_present();

        uint64_t now;
        do {
            usyscall0(SYS_YIELD);
            now = usyscall0(SYS_GET_TICKS);
        } while (now - last_tick < FRAME_MS);
        last_tick = now;
    }
}
