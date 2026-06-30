#include "serial.h"
#include "screen.h"
#include "../cpu/ports.h"

static uint16_t serial_port = 0;

#define REG_DATA(b)  (b)
#define REG_IER(b)   ((b)+1u)
#define REG_FCR(b)   ((b)+2u)
#define REG_LCR(b)   ((b)+3u)
#define REG_MCR(b)   ((b)+4u)
#define REG_LSR(b)   ((b)+5u)

#define LCR_DLAB  0x80u
#define LCR_8N1   0x03u
#define LSR_THRE  0x20u   /* Transmit Holding Register Empty */

void serial_init(uint16_t port, uint32_t baud) {
    serial_port = port;

    uint16_t divisor = (uint16_t)(115200u / baud);

    port_byte_out(REG_IER(port), 0x00u);       /* disable interrupts  */
    port_byte_out(REG_LCR(port), LCR_DLAB);    /* enable DLAB         */
    port_byte_out(REG_DATA(port), (uint8_t)(divisor & 0xFF));
    port_byte_out(REG_IER(port),  (uint8_t)(divisor >> 8));
    port_byte_out(REG_LCR(port), LCR_8N1);     /* 8N1, clear DLAB     */
    port_byte_out(REG_FCR(port), 0xC7u);        /* FIFO on, clear, 14B */
    port_byte_out(REG_MCR(port), 0x03u);        /* DTR + RTS           */

    screen_set_serial_hook(serial_putc);
}

void serial_putc(char c) {
    if (!serial_port) return;
    if (c == '\n')
        serial_putc('\r');
    while (!(port_byte_in(REG_LSR(serial_port)) & LSR_THRE));
    port_byte_out(REG_DATA(serial_port), (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}
