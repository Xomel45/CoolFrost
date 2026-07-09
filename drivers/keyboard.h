#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Virtual scancodes for extended/arrow keys (exported for USB HID use) */
#define KEY_UP    0x60u
#define KEY_DOWN  0x61u
#define KEY_LEFT  0x62u
#define KEY_RIGHT 0x63u

uint8_t get_cur_scancode(void);
char    keyboard_receive_key(char halt);
void    getline(char *to, char echo, uint32_t max_len);
void    init_keyboard(void);

/* Inject a PS/2-style scancode as if the keyboard interrupt fired.
 * Silently dropped if another key is already pending.               */
void    kbd_inject_scancode(uint8_t sc);

#endif
