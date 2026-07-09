#ifndef USBHID_H
#define USBHID_H

#include <stdint.h>

/* Detect and configure all USB HID (boot-protocol) devices found by
 * xhci_scan_devices().  Call once after xhci_scan_devices().           */
void usbhid_init(void);

/* Poll all HID interrupt endpoints for new reports.
 * Called from the timer ISR every tick.  Must be fast when idle.       */
void usbhid_poll(void);

/* Returns 1 if at least one USB keyboard was found and configured.     */
int usbhid_kbd_present(void);

/* Returns 1 if at least one USB mouse was found and configured.        */
int usbhid_mouse_present(void);

/* USB mouse absolute position (uses the same mouse_get_x/y interface via
 * mouse_inject_delta — no separate accessors needed).                  */

#endif
