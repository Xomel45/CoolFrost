#include "syscall.h"
#include "idt.h"
#include "sched.h"
#include "paging.h"
#include "timer.h"
#include "../ui/event.h"
#include "../gfx/gfx.h"
#include "../drivers/screen.h"
#include "../libc/mem.h"

#define SYSCALL_VECTOR 0x80

extern void syscall_stub(void);

void syscall_install(void) {
    set_idt_gate_dpl(SYSCALL_VECTOR, (uint64_t)syscall_stub, 3);
}

void syscall_dispatch(registers_t *r) {
    switch (r->rax) {
    case SYS_EXIT:
        sched_kill_current();
        r->rax = 0;
        break;

    case SYS_YIELD:
        /* syscall_stub's own epilogue calls sched_irq_end right after we
         * return — just flag the reschedule, don't nest another interrupt. */
        sched_want_reschedule();
        r->rax = 0;
        break;

    case SYS_GET_TICKS:
        r->rax = get_tick();
        break;

    case SYS_EVENT_POLL: {
        event_t *out = (event_t *)(uintptr_t)r->rdi;
        if (!is_user_range(out, sizeof(event_t))) {
            r->rax = (uint64_t)-1;
            break;
        }
        event_t ev;
        r->rax = event_poll(&ev) ? 1 : 0;
        if (r->rax) *out = ev;
        break;
    }

    case SYS_GFX_PRESENT: {
        gfx_surface_t *scr = gfx_screen();
        if (!scr) { r->rax = (uint64_t)-1; break; }
        size_t bytes = (size_t)scr->w * scr->h * 4u;
        const void *src = (const void *)(uintptr_t)r->rdi;
        if (!is_user_range(src, bytes)) { r->rax = (uint64_t)-1; break; }
        memcpy(scr->px, src, bytes);
        gfx_present();
        r->rax = 0;
        break;
    }

    case SYS_FB_INFO: {
        gfx_surface_t *scr = gfx_screen();
        r->rax = scr ? (((uint64_t)(uint32_t)scr->w << 32) | (uint32_t)scr->h) : 0;
        break;
    }

    case SYS_GET_FONT: {
        uint8_t *dst = (uint8_t *)(uintptr_t)r->rdi;
        if (!is_user_range(dst, 256 * 16)) { r->rax = (uint64_t)-1; break; }
        for (int ch = 0; ch < 256; ch++)
            memcpy(dst + ch * 16, screen_glyph8x16((unsigned char)ch), 16);
        r->rax = 0;
        break;
    }

    case SYS_WRITE: {
        /* kprint(), not kprint_char() — only kprint()/kprint_at_attr() mirror
         * to serial (drivers/screen.c: g_serial_hook), and that mirror is
         * the only way to observe output from a headless QEMU test run. */
        const char *buf = (const char *)(uintptr_t)r->rdi;
        uint64_t len = r->rsi;
        if (len > 256) len = 256;
        if (!is_user_range(buf, len)) { r->rax = (uint64_t)-1; break; }
        char local[257];
        memcpy(local, buf, len);
        local[len] = '\0';
        kprint(local);
        r->rax = len;
        break;
    }

    default:
        r->rax = (uint64_t)-1;
        break;
    }
}
