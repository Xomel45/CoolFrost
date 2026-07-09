#ifndef UI_EVENT_H
#define UI_EVENT_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  Unified input event queue for the future DE/WM.
 *
 *  Producers: keyboard IRQ1, PS/2 mouse IRQ12, USB HID poll (timer ISR).
 *  Consumer:  the WM/compositor main loop via event_poll().
 *
 *  IRQ handlers run with interrupts disabled (interrupt gates) and never
 *  nest, so single-producer-at-a-time push is safe without extra locking
 *  as long as the consumer runs on the BSP with IRQs briefly masked in
 *  event_poll().
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    EV_NONE = 0,
    EV_KEY_DOWN,     /* code = scancode (incl. KEY_UP/DOWN/... virtuals), ch = ASCII or 0 */
    EV_KEY_UP,       /* code = scancode */
    EV_MOUSE_MOVE,   /* x, y = absolute position */
    EV_MOUSE_DOWN,   /* code = MOUSE_BTN_* bit that went down; x, y position */
    EV_MOUSE_UP,     /* code = MOUSE_BTN_* bit that went up               */
} event_type_t;

typedef struct {
    uint8_t  type;      /* event_type_t */
    uint8_t  code;      /* scancode or button bit */
    char     ch;        /* translated character for EV_KEY_DOWN, else 0 */
    uint8_t  buttons;   /* full button mask at event time */
    int32_t  x, y;      /* mouse position (mouse events) */
} event_t;

/* Reset the queue (call once at boot; safe to call again to flush). */
void event_init(void);

/* Pop one event.  Returns 1 and fills *out, or 0 if the queue is empty. */
int  event_poll(event_t *out);

/* Producers (called from IRQ/ISR context) */
void event_push_key(uint8_t scancode, int down, char ch);
void event_push_mouse(int32_t x, int32_t y, uint8_t buttons);

#endif
