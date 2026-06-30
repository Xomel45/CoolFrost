#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include "../kernel/multiboot.h"

/* Packed 32-bit ARGB color */
#define COLOR_BLACK       0x00000000
#define COLOR_WHITE       0x00FFFFFF
#define COLOR_RED         0x00FF0000
#define COLOR_GREEN       0x0000FF00
#define COLOR_BLUE        0x000000FF
#define COLOR_CYAN        0x0000FFFF
#define COLOR_MAGENTA     0x00FF00FF
#define COLOR_YELLOW      0x00FFFF00
#define COLOR_LIGHTGRAY   0x00AAAAAA
#define COLOR_DARKGRAY    0x00555555

/* Legacy VGA color attribute bytes (for compatibility with existing kprint_attr callers) */
/* Foreground nibble */
enum VGAForegroundColors {
    BLACK_FG       = 0x00,
    BLUE_FG        = 0x01,
    GREEN_FG       = 0x02,
    CYAN_FG        = 0x03,
    RED_FG         = 0x04,
    MAGENTA_FG     = 0x05,
    BROWN_FG       = 0x06,
    LIGHTGRAY_FG   = 0x07,
    DARKGRAY_FG    = 0x08,
    LIGHTBLUE_FG   = 0x09,
    LIGHTGREEN_FG  = 0x0A,
    LIGHTCYAN_FG   = 0x0B,
    LIGHTRED_FG    = 0x0C,
    LIGHTMAGENTA_FG= 0x0D,
    YELLOW_FG      = 0x0E,
    WHITE_FG       = 0x0F,
};

/* Background nibble (high nibble of attr byte) */
enum VGABackgroundColors {
    BLACK_BG       = 0x00,
    BLUE_BG        = 0x10,
    GREEN_BG       = 0x20,
    CYAN_BG        = 0x30,
    RED_BG         = 0x40,
    MAGENTA_BG     = 0x50,
    BROWN_BG       = 0x60,
    LIGHTGRAY_BG   = 0x70,
};

/* Standard attribute bytes */
#define WHITE_ON_BLACK  0x0F
#define RED_ON_WHITE    0xF4
#define RED_ON_BLACK    0x04
#define WHITE_ON_RED    0x4F

/* CP437 box-drawing characters (same codes as VGA ROM) */
enum BoxChars {
    BoxCrnLeftUp      = 0xC9,
    BoxCrnRightUp     = 0xBB,
    BoxCrnLeftDown    = 0xC8,
    BoxCrnRightDown   = 0xBC,
    BoxLineVert       = 0xBA,
    BoxLineHorz       = 0xCD,
    BoxVer2HorzLeft   = 0xB9,
    BoxVer2HorzRight  = 0xCC,
    BoxHorz2VerUp     = 0xCA,
    BoxHorz2VerDown   = 0xCB,
    BoxCross          = 0xCE,
};

/* Font dimensions */
#define FONT_W  8
#define FONT_H  16

/* Must be called before any kprint/clear_screen.
 * Falls back to VGA text mode if type == MB2_FB_TYPE_EGA_TEXT or fb is unavailable. */
void screen_init(fb_info_t *fb);

void clear_screen(void);
void kprint_at(char *message, int col, int row);
void kprint_at_attr(char *message, int col, int row, char attr);
void kprint(char *message);
void kprint_char(char c);
void kprint_attr(char *message, char attr);
void kprint_backspace(void);
int  get_cursor_offset(void);
void set_cursor_offset(int offset);
int  get_offset(int col, int row);

/* Optional serial mirror: if set, every character printed via kprint also
 * goes to this function (e.g. serial_putc for QEMU -serial stdio output). */
void screen_set_serial_hook(void (*fn)(char));

/* VGA text fallback registers (used when framebuffer unavailable) */
#define REG_SCREEN_CTRL 0x3D4
#define REG_SCREEN_DATA 0x3D5

#endif
