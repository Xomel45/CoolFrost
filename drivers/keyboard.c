#include "../cpu/ports.h"
#include "../cpu/isr.h"
#include <stdint.h>
#include "screen.h"

static uint8_t cur_scancode = 0;
static uint8_t shift_pressed = 0;
static uint8_t e0_prefix = 0;

/* Virtual scancodes for extended keys (above 0x58, below 0x80 — no conflict) */
#define KEY_UP    0x60
#define KEY_DOWN  0x61
#define KEY_LEFT  0x62
#define KEY_RIGHT 0x63

uint8_t get_cur_scancode() {
    uint8_t temp = cur_scancode;
    cur_scancode = 0;
    return temp;
}

// Non-Shifted keys
static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*',0,' ',
};

// Shifted keys
static const char keymap_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*',0,' ',
};

char keyboard_receive_key(char halt)
{
    // Halt everything until we receive a key
    while (halt && cur_scancode == 0) asm volatile("hlt");

    // Key release (break code)
    if (cur_scancode & 0x80) {
        unsigned char release_code = cur_scancode & 0x7F;
        if (release_code == 0x2A || release_code == 0x36) // Left/Right Shift
            shift_pressed = 0;
        cur_scancode = 0;
        return 0;
    }

    // Shift pressed
    if (cur_scancode == 0x2A || cur_scancode == 0x36) {
        shift_pressed = 1;
        cur_scancode = 0;
        return 0;
    }

    char c = shift_pressed ? keymap_shift[cur_scancode] : keymap[cur_scancode];
    cur_scancode = 0;
    return c;
}

/*
 * getline — read a line into `to`, with optional echo and cursor navigation.
 *
 * Supports:
 *   - Left/Right arrow keys to move within the buffer
 *   - Insert-mode typing (shifts chars right)
 *   - Backspace from any position (shifts chars left, redraws suffix)
 */
void getline(char *to, char echo, uint32_t max_len) {
    char *start = to;
    uint32_t len = 0;        /* current number of chars in buffer */
    uint32_t cursor_pos = 0; /* logical position of the cursor    */
    int base_offset = 0;     /* VGA byte-offset where input starts */

    if (echo) base_offset = get_cursor_offset();

    while (1) {
        /* Wait for a key */
        while (cur_scancode == 0) asm volatile("hlt");
        uint8_t sc = cur_scancode;
        cur_scancode = 0;

        /* Key release — only care about Shift */
        if (sc & 0x80) {
            if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36)
                shift_pressed = 0;
            continue;
        }

        /* Shift make */
        if (sc == 0x2A || sc == 0x36) {
            shift_pressed = 1;
            continue;
        }

        /* Arrow key: move cursor left */
        if (sc == KEY_LEFT) {
            if (cursor_pos > 0) {
                cursor_pos--;
                if (echo) set_cursor_offset(base_offset + (int)cursor_pos * 2);
            }
            continue;
        }

        /* Arrow key: move cursor right */
        if (sc == KEY_RIGHT) {
            if (cursor_pos < len) {
                cursor_pos++;
                if (echo) set_cursor_offset(base_offset + (int)cursor_pos * 2);
            }
            continue;
        }

        char c = shift_pressed ? keymap_shift[sc] : keymap[sc];
        if (c == 0) continue;

        /* Enter */
        if (c == '\n') break;

        /* Backspace — delete char before cursor, redraw suffix */
        if (c == '\b') {
            if (cursor_pos > 0) {
                cursor_pos--;
                len--;
                /* Shift buffer left by one */
                for (uint32_t i = cursor_pos; i < len; i++)
                    start[i] = start[i + 1];
                if (echo) {
                    int save = base_offset + (int)cursor_pos * 2;
                    set_cursor_offset(save);
                    char tmp[2] = {0, 0};
                    for (uint32_t i = cursor_pos; i < len; i++) {
                        tmp[0] = start[i];
                        kprint(tmp);
                    }
                    kprint(" "); /* erase the now-extra last char */
                    set_cursor_offset(save);
                }
            }
            continue;
        }

        /* Regular character — insert at cursor_pos */
        if (len < max_len - 1) {
            /* Shift buffer right by one */
            for (uint32_t i = len; i > cursor_pos; i--)
                start[i] = start[i - 1];
            start[cursor_pos] = c;
            cursor_pos++;
            len++;
            if (echo) {
                int save = base_offset + (int)cursor_pos * 2;
                set_cursor_offset(base_offset + (int)(cursor_pos - 1) * 2);
                char tmp[2] = {0, 0};
                for (uint32_t i = cursor_pos - 1; i < len; i++) {
                    tmp[0] = start[i];
                    kprint(tmp);
                }
                set_cursor_offset(save);
            }
        }
    }

    start[len] = '\0';
}

void kb_callback(registers_t *regs) {
    (void)regs;
    uint8_t sc = port_byte_in(0x60);
    if (sc == 0xE0) {
        e0_prefix = 1;
    } else if (e0_prefix) {
        e0_prefix = 0;
        if (!(sc & 0x80)) { /* only make codes, ignore extended releases */
            switch (sc) {
                case 0x48: cur_scancode = KEY_UP;    break;
                case 0x50: cur_scancode = KEY_DOWN;  break;
                case 0x4B: cur_scancode = KEY_LEFT;  break;
                case 0x4D: cur_scancode = KEY_RIGHT; break;
            }
        }
    } else {
        cur_scancode = sc;
    }
    port_byte_out(0x20, 0x20);  /* send EOI to master PIC */
}

void init_keyboard() {
    register_interrupt_handler(IRQ1, &kb_callback);
}
