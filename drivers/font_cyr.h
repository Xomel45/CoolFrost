#ifndef FONT_CYR_H
#define FONT_CYR_H

#include <stdint.h>

/* Returns pointer to 16-byte 8x16 glyph for a Cyrillic codepoint, or NULL. */
const uint8_t *cyr_get_glyph(uint32_t cp);

#endif
