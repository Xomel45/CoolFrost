#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define COM1 0x3F8u
#define COM2 0x2F8u

void serial_init(uint16_t port, uint32_t baud);   /* call once; also hooks kprint */
void serial_putc(char c);
void serial_puts(const char *s);

#endif
