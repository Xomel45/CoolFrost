#include "mouse.h"
#include "../cpu/isr.h"
#include "../cpu/ports.h"
#include "../ui/event.h"

/* ── 8042 PS/2 controller ports ────────────────────────────────────────── */
#define PS2_DATA    0x60u
#define PS2_STATUS  0x64u
#define PS2_CMD     0x64u

#define PS2_OBF     (1u << 0)   /* output buffer full — safe to read 0x60 */
#define PS2_IBF     (1u << 1)   /* input  buffer full — wait before write  */
#define PS2_MOUSE   (1u << 5)   /* OBF is from auxiliary (mouse) port      */

/* Mouse commands sent via 0xD4 prefix */
#define MCMD_RESET     0xFFu
#define MCMD_DEFAULTS  0xF6u
#define MCMD_ENABLE    0xF4u
#define MCMD_SETRATE   0xF3u
#define MCMD_GETID     0xF2u
#define MOUSE_ACK      0xFAu

/* ── Internal state ─────────────────────────────────────────────────────── */
static volatile int32_t  state_x       = 0;
static volatile int32_t  state_y       = 0;
static volatile int32_t  state_dx      = 0;
static volatile int32_t  state_dy      = 0;
static volatile uint8_t  state_buttons = 0;
static volatile int      state_present = 0;

static int32_t bound_w = 1920;
static int32_t bound_h = 1080;

/* ── PS/2 I/O helpers ───────────────────────────────────────────────────── */

static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(port_byte_in(PS2_STATUS) & PS2_IBF)) return 1;
    return 0;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (port_byte_in(PS2_STATUS) & PS2_OBF) return 1;
    return 0;
}

static void ps2_flush(void) {
    for (int i = 0; i < 16; i++) {
        if (!(port_byte_in(PS2_STATUS) & PS2_OBF)) break;
        port_byte_in(PS2_DATA);
    }
}

/* Send a byte to the mouse (via 8042 auxiliary port prefix 0xD4). */
static int mouse_send(uint8_t cmd) {
    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_CMD,  0xD4u);
    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_DATA, cmd);
    return 1;
}

/* Read one byte from the controller data port (with timeout). */
static int mouse_recv(uint8_t *out) {
    if (!ps2_wait_read()) return 0;
    *out = port_byte_in(PS2_DATA);
    return 1;
}

/* ── IRQ12 handler ──────────────────────────────────────────────────────── *
 * The 8042 fires IRQ12 whenever the mouse sends a byte.  We reassemble     *
 * 3-byte standard PS/2 packets and update the position/button state.       */
static uint8_t pkt[3];
static int     pkt_pos = 0;

static void mouse_irq_handler(registers_t *regs) {
    (void)regs;

    uint8_t status = port_byte_in(PS2_STATUS);
    if (!(status & PS2_OBF)) return;

    uint8_t byte = port_byte_in(PS2_DATA);

    /* Byte 0 must always have bit 3 set (always-one bit in standard PS/2
     * mouse packet).  Drop stale bytes to re-sync if needed.              */
    if (pkt_pos == 0 && !(byte & 0x08u)) return;

    pkt[pkt_pos++] = byte;
    if (pkt_pos < 3) return;
    pkt_pos = 0;

    /* ── Decode packet ── */
    uint8_t flags = pkt[0];

    /* X/Y are 9-bit two's-complement: sign bit is in flags byte 0.       */
    int dx = (int)(uint8_t)pkt[1] - ((flags & 0x10u) ? 256 : 0);
    int dy = (int)(uint8_t)pkt[2] - ((flags & 0x20u) ? 256 : 0);

    /* Discard if overflow bits set (values > 255 px in one packet). */
    if (flags & 0x40u) dx = 0;
    if (flags & 0x80u) dy = 0;

    state_buttons = flags & 0x07u;

    /* Accumulate relative delta for mouse_consume_delta() */
    state_dx += dx;
    state_dy -= dy;   /* PS/2 Y axis is inverted relative to screen */

    /* Update absolute position with clamping */
    state_x += dx;
    state_y -= dy;
    if (state_x < 0)         state_x = 0;
    if (state_y < 0)         state_y = 0;
    if (state_x >= bound_w)  state_x = bound_w - 1;
    if (state_y >= bound_h)  state_y = bound_h - 1;

    event_push_mouse(state_x, state_y, state_buttons);
}

/* ── mouse_init ─────────────────────────────────────────────────────────── *
 * The whole negotiation runs with interrupts masked: IRQ1 is already live
 * at this point, and 8042 command responses raise IRQ1, so kb_callback
 * would steal the bytes our polling loop is waiting for.                   */
static int mouse_init_polled(void) {
    uint8_t ack, id;

    ps2_flush();

    /* 1. Enable auxiliary PS/2 port on the 8042 */
    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_CMD, 0xA8u);

    /* 2. Read controller command byte, enable IRQ12 (bit 1), clear
     *    "disable mouse" bit (bit 5).                                    */
    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_CMD, 0x20u);      /* read command byte */
    if (!ps2_wait_read())  return 0;
    uint8_t cb = port_byte_in(PS2_DATA);
    cb |= 0x02u;    /* enable auxiliary interrupt (IRQ12) */
    cb &= ~0x20u;   /* clear "mouse disabled" bit */

    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_CMD,  0x60u);     /* write command byte */
    if (!ps2_wait_write()) return 0;
    port_byte_out(PS2_DATA, cb);

    /* 3. Reset mouse: expect ACK → 0xAA → device_id */
    if (!mouse_send(MCMD_RESET)) return 0;
    if (!mouse_recv(&ack) || ack != MOUSE_ACK) return 0;
    if (!mouse_recv(&ack) || ack != 0xAAu) return 0;   /* BAT passed */
    if (!mouse_recv(&id))  return 0;                   /* device ID */
    (void)id;

    /* 4. Enable data reporting */
    if (!mouse_send(MCMD_ENABLE)) return 0;
    if (!mouse_recv(&ack) || ack != MOUSE_ACK) return 0;

    return 1;
}

int mouse_init(void) {
    __asm__ volatile("cli");
    int ok = mouse_init_polled();
    if (ok)
        register_interrupt_handler(IRQ12, mouse_irq_handler);
    __asm__ volatile("sti");

    if (ok) state_present = 1;
    return ok;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void mouse_set_bounds(int32_t w, int32_t h) {
    bound_w = (w > 0) ? w : 1;
    bound_h = (h > 0) ? h : 1;
}

int32_t mouse_get_x(void) { return state_x; }
int32_t mouse_get_y(void) { return state_y; }

void mouse_consume_delta(int32_t *dx, int32_t *dy) {
    /* Disable IRQs briefly to read atomically */
    __asm__ volatile("cli");
    *dx = state_dx; state_dx = 0;
    *dy = state_dy; state_dy = 0;
    __asm__ volatile("sti");
}

uint8_t mouse_get_buttons(void) { return state_buttons; }
int     mouse_present(void)     { return state_present; }

void mouse_inject_delta(int dx, int dy, uint8_t buttons) {
    /* Called from timer ISR (interrupts already disabled) — no CLI/STI. */
    state_x += dx;
    state_y -= dy;   /* screen Y increases downward, PS/2/HID Y increases upward */
    if (state_x < 0)        state_x = 0;
    if (state_y < 0)        state_y = 0;
    if (state_x >= bound_w) state_x = bound_w - 1;
    if (state_y >= bound_h) state_y = bound_h - 1;
    state_buttons = buttons;
    state_present = 1;   /* mark present so mouse_present() returns true */

    event_push_mouse(state_x, state_y, state_buttons);
}
