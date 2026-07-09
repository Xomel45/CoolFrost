#include "event.h"

#define EVQ_CAP 128   /* power of 2 */

static event_t  evq[EVQ_CAP];
static volatile uint32_t evq_head = 0;   /* read index  */
static volatile uint32_t evq_tail = 0;   /* write index */

/* Last mouse state seen by the queue — used to derive button-edge events */
static int32_t last_mx = -1, last_my = -1;
static uint8_t last_buttons = 0;

void event_init(void) {
    evq_head = evq_tail = 0;
    last_mx = last_my = -1;
    last_buttons = 0;
}

/* Push from IRQ context (interrupts already off, no nesting). Drops on overflow. */
static void evq_push(const event_t *ev) {
    if (evq_tail - evq_head >= EVQ_CAP) return;   /* full — drop */
    evq[evq_tail & (EVQ_CAP - 1)] = *ev;
    evq_tail++;
}

int event_poll(event_t *out) {
    int got = 0;
    __asm__ volatile("cli");
    if (evq_head != evq_tail) {
        *out = evq[evq_head & (EVQ_CAP - 1)];
        evq_head++;
        got = 1;
    }
    __asm__ volatile("sti");
    return got;
}

void event_push_key(uint8_t scancode, int down, char ch) {
    event_t ev;
    ev.type    = down ? EV_KEY_DOWN : EV_KEY_UP;
    ev.code    = scancode;
    ev.ch      = down ? ch : 0;
    ev.buttons = last_buttons;
    ev.x       = last_mx;
    ev.y       = last_my;
    evq_push(&ev);
}

void event_push_mouse(int32_t x, int32_t y, uint8_t buttons) {
    /* Button edges first (position is the new one) */
    uint8_t changed = buttons ^ last_buttons;
    for (uint8_t bit = 1; bit <= 4; bit <<= 1) {
        if (!(changed & bit)) continue;
        event_t ev;
        ev.type    = (buttons & bit) ? EV_MOUSE_DOWN : EV_MOUSE_UP;
        ev.code    = bit;
        ev.ch      = 0;
        ev.buttons = buttons;
        ev.x       = x;
        ev.y       = y;
        evq_push(&ev);
    }

    if (x != last_mx || y != last_my) {
        event_t ev;
        ev.type    = EV_MOUSE_MOVE;
        ev.code    = 0;
        ev.ch      = 0;
        ev.buttons = buttons;
        ev.x       = x;
        ev.y       = y;
        evq_push(&ev);
    }

    last_mx = x;
    last_my = y;
    last_buttons = buttons;
}
