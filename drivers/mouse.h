#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Button bitmask (same bits as PS/2 packet byte 0) */
#define MOUSE_BTN_LEFT   (1u << 0)
#define MOUSE_BTN_RIGHT  (1u << 1)
#define MOUSE_BTN_MIDDLE (1u << 2)

/* Initialise PS/2 auxiliary port and install IRQ12 handler.
 * Call once, after irq_install().
 * Returns 1 on success, 0 if no PS/2 mouse was detected.      */
int     mouse_init(void);

/* Set screen bounds for position clamping (default 1920×1080). */
void    mouse_set_bounds(int32_t w, int32_t h);

/* Current absolute position (pixels from top-left). */
int32_t mouse_get_x(void);
int32_t mouse_get_y(void);

/* Relative movement since last call to mouse_consume_delta(). */
void    mouse_consume_delta(int32_t *dx, int32_t *dy);

/* Button state — bitmask of MOUSE_BTN_* flags. */
uint8_t mouse_get_buttons(void);

/* 1 if mouse is present and operational. */
int     mouse_present(void);

/* Inject a relative motion event (dx right, dy down) with button mask.
 * Called from USB HID driver (timer ISR context — no CLI/STI needed). */
void    mouse_inject_delta(int dx, int dy, uint8_t buttons);

#endif
