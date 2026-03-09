#include "../cpu/ports.h"
#include "../cpu/isr.h"
#include <stdint.h>
#include "screen.h"

static uint8_t cur_scancode = 0;
static uint8_t shift_pressed = 0;

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
        return 0; // Ignore key releases
    }

    // Shift pressed
    if (cur_scancode == 0x2A || cur_scancode == 0x36) { // Left/Right Shift
        shift_pressed = 1;
        return 0;
    }

    char c = shift_pressed ? keymap_shift[cur_scancode] : keymap[cur_scancode];
    cur_scancode = 0;
    return c;
}

void getline(char *to, char echo, uint32_t max_len) {
    char key = 0;
    char temp[2];
    char *start = to;

    while (1) {
        key = keyboard_receive_key(1);
        if (key == '\n') break;
        if (key == '\0') continue;

        if (key == '\b') {
            /* Only allow backspace if there's something to erase */
            if (to > start) {
                to--;
                if (echo)
                    kprint_backspace();
            }
            continue;
        }

        if ((uint32_t)(to - start) < max_len - 1) {
            *to++ = key;
            if (echo) {
                temp[0] = key;
                temp[1] = '\0';
                kprint(temp);
            }
        }
    }

    *to = '\0';
}

void kb_callback(registers_t *regs) {
    (void)regs;
    cur_scancode = port_byte_in(0x60);
    port_byte_out(0x20, 0x20);  // send EOI to master PIC
}

void init_keyboard() {
    register_interrupt_handler(IRQ1, &kb_callback);
}
