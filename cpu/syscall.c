#include "syscall.h"
#include "idt.h"
#include "sched.h"
#include "vmm.h"
#include "paging.h"
#include "timer.h"
#include "../ui/event.h"
#include "../gfx/gfx.h"
#include "../drivers/screen.h"
#include "../libc/mem.h"
#include "../fs/vfs.h"

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

    case SYS_GFX_PRESENT_RECT: {
        gfx_surface_t *scr = gfx_screen();
        if (!scr) { r->rax = (uint64_t)-1; break; }

        uint64_t packed = r->rsi;
        int32_t x = (int32_t)(uint16_t)(packed >> 48);
        int32_t y = (int32_t)(uint16_t)(packed >> 32);
        int32_t w = (int32_t)(uint16_t)(packed >> 16);
        int32_t h = (int32_t)(uint16_t)packed;

        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x >= scr->w || y >= scr->h || w <= 0 || h <= 0) { r->rax = 0; break; }
        if (x + w > scr->w) w = scr->w - x;
        if (y + h > scr->h) h = scr->h - y;

        /* Validate against the caller's *whole* backbuffer (rows below are
         * indexed by scr->w, the full stride, not just the rect) — same
         * bound SYS_GFX_PRESENT uses. */
        size_t full_bytes = (size_t)scr->w * scr->h * 4u;
        const uint8_t *src = (const uint8_t *)(uintptr_t)r->rdi;
        if (!is_user_range(src, full_bytes)) { r->rax = (uint64_t)-1; break; }

        for (int32_t row = 0; row < h; row++) {
            size_t off = ((size_t)(y + row) * scr->w + x) * 4u;
            memcpy((uint8_t *)scr->px + off, src + off, (size_t)w * 4u);
        }
        gfx_present_rect(x, y, w, h);
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

    case SYS_OPEN: {
        uint64_t len = r->rsi;
        if (len > MAX_PATH - 1) len = MAX_PATH - 1;
        const char *upath = (const char *)(uintptr_t)r->rdi;
        if (!is_user_range(upath, len)) { r->rax = (uint64_t)-1; break; }
        char local[MAX_PATH];
        memcpy(local, upath, len);
        local[len] = '\0';
        r->rax = (uint64_t)(int64_t)vfs_open(local, (uint8_t)r->rdx);
        break;
    }

    case SYS_CLOSE:
        r->rax = (uint64_t)(int64_t)vfs_close((int)r->rdi);
        break;

    case SYS_READ: {
        int fd = (int)r->rdi;
        void *buf = (void *)(uintptr_t)r->rsi;
        uint64_t size = r->rdx;
        if (!is_user_range(buf, size)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_read(fd, buf, (size_t)size);
        break;
    }

    case SYS_SEEK:
        r->rax = (uint64_t)(int64_t)vfs_seek((int)r->rdi, r->rsi);
        break;

    case SYS_FWRITE: {
        int fd = (int)r->rdi;
        const void *buf = (const void *)(uintptr_t)r->rsi;
        uint64_t size = r->rdx;
        if (!is_user_range(buf, size)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_write(fd, buf, (size_t)size);
        break;
    }

    case SYS_STAT: {
        uint64_t len = r->rsi;
        if (len > MAX_PATH - 1) len = MAX_PATH - 1;
        const char *upath = (const char *)(uintptr_t)r->rdi;
        void *ubuf = (void *)(uintptr_t)r->rdx;
        if (!is_user_range(upath, len) || !is_user_range(ubuf, sizeof(vfs_stat_t))) {
            r->rax = (uint64_t)-1;
            break;
        }
        char local[MAX_PATH];
        memcpy(local, upath, len);
        local[len] = '\0';

        vfs_stat_t st;
        if (vfs_stat(local, &st) != 0) { r->rax = (uint64_t)-1; break; }
        memcpy(ubuf, &st, sizeof(st));
        r->rax = 0;
        break;
    }

    case SYS_FSTAT: {
        int fd = (int)r->rdi;
        void *ubuf = (void *)(uintptr_t)r->rsi;
        if (!is_user_range(ubuf, sizeof(vfs_stat_t))) { r->rax = (uint64_t)-1; break; }

        vfs_stat_t st;
        if (vfs_fstat(fd, &st) != 0) { r->rax = (uint64_t)-1; break; }
        memcpy(ubuf, &st, sizeof(st));
        r->rax = 0;
        break;
    }

    case SYS_READDIR: {
        int fd = (int)r->rdi;
        uint32_t index = (uint32_t)r->rsi;
        void *ubuf = (void *)(uintptr_t)r->rdx;
        if (!is_user_range(ubuf, sizeof(dirent_t))) { r->rax = (uint64_t)-1; break; }

        dirent_t *de = vfs_readdir(fd, index);
        if (!de) { r->rax = (uint64_t)-1; break; }
        memcpy(ubuf, de, sizeof(dirent_t));
        r->rax = 0;
        break;
    }

    case SYS_UNLINK: {
        uint64_t len = r->rsi;
        if (len > MAX_PATH - 1) len = MAX_PATH - 1;
        const char *upath = (const char *)(uintptr_t)r->rdi;
        if (!is_user_range(upath, len)) { r->rax = (uint64_t)-1; break; }
        char local[MAX_PATH];
        memcpy(local, upath, len);
        local[len] = '\0';
        r->rax = (vfs_unlink(local) == 0) ? 0 : (uint64_t)-1;
        break;
    }

    case SYS_SBRK: {
        task_t *cur = sched_current_task();
        if (!cur->as || cur->heap_end == 0) { r->rax = (uint64_t)-1; break; }

        int64_t increment = (int64_t)r->rdi;
        uint64_t old_brk = cur->heap_brk;

        if (increment == 0) { r->rax = old_brk; break; }
        if (increment < 0) { r->rax = (uint64_t)-1; break; }   /* shrink: unsupported */

        uint64_t new_brk = old_brk + (uint64_t)increment;
        if (new_brk < old_brk || new_brk > cur->heap_end) { r->rax = (uint64_t)-1; break; }

        /* Pages already mapped cover [heap_start, mapped_end); map whatever
         * new whole pages are needed to reach new_brk. round-up-to-page of
         * (old_brk - heap_start) naturally comes out to 0 on the very first
         * call (old_brk == heap_start), so this needs no special-casing for
         * "nothing mapped yet". */
        uint64_t mapped_end = cur->heap_start +
            ((old_brk - cur->heap_start + 0xFFFULL) & ~0xFFFULL);
        uint64_t needed_end = cur->heap_start +
            ((new_brk - cur->heap_start + 0xFFFULL) & ~0xFFFULL);

        int ok = 1;
        for (uint64_t va = mapped_end; va < needed_end; va += 0x1000ULL) {
            uint64_t pa = vmm_alloc_page(cur->as);
            if (!pa || vmm_map_page(cur->as, va, pa, VMM_PAGE_RW_U) != 0) { ok = 0; break; }
        }
        if (!ok) { r->rax = (uint64_t)-1; break; }

        cur->heap_brk = new_brk;
        r->rax = old_brk;
        break;
    }

    case SYS_RENAME: {
        uint64_t old_len = r->rsi;
        uint64_t new_len = r->r10;
        if (old_len > MAX_PATH - 1) old_len = MAX_PATH - 1;
        if (new_len > MAX_PATH - 1) new_len = MAX_PATH - 1;

        const char *uold = (const char *)(uintptr_t)r->rdi;
        const char *unew = (const char *)(uintptr_t)r->rdx;
        if (!is_user_range(uold, old_len) || !is_user_range(unew, new_len)) {
            r->rax = (uint64_t)-1;
            break;
        }

        char old_path[MAX_PATH];
        char new_path[MAX_PATH];
        memcpy(old_path, uold, old_len);
        old_path[old_len] = '\0';
        memcpy(new_path, unew, new_len);
        new_path[new_len] = '\0';

        r->rax = (vfs_rename(old_path, new_path) == 0) ? 0 : (uint64_t)-1;
        break;
    }

    case SYS_GFX_FILL_RECT: {
        gfx_surface_t *scr = gfx_screen();
        if (!scr) { r->rax = (uint64_t)-1; break; }

        uint64_t packed = r->rdi;
        int32_t x = (int32_t)(uint16_t)(packed >> 48);
        int32_t y = (int32_t)(uint16_t)(packed >> 32);
        int32_t w = (int32_t)(uint16_t)(packed >> 16);
        int32_t h = (int32_t)(uint16_t)packed;
        uint32_t color = (uint32_t)r->rsi;

        gfx_fill_rect(scr, x, y, w, h, color);
        r->rax = 0;
        break;
    }

    case SYS_GFX_DRAW_TEXT: {
        gfx_surface_t *scr = gfx_screen();
        if (!scr) { r->rax = (uint64_t)-1; break; }

        uint64_t xy = r->rdi;
        int32_t x = (int32_t)(uint16_t)(xy >> 16);
        int32_t y = (int32_t)(uint16_t)xy;

        uint64_t len = r->rdx;
        if (len > 256) len = 256;
        const char *utext = (const char *)(uintptr_t)r->rsi;
        if (!is_user_range(utext, len)) { r->rax = (uint64_t)-1; break; }

        char local[257];
        memcpy(local, utext, len);
        local[len] = '\0';

        uint64_t fgbg = r->r10;
        uint32_t fg = (uint32_t)(fgbg >> 32);
        uint32_t bg = (uint32_t)fgbg;

        gfx_draw_text(scr, x, y, local, fg, bg);
        r->rax = 0;
        break;
    }

    case SYS_GFX_FLUSH_RECT: {
        gfx_surface_t *scr = gfx_screen();
        if (!scr) { r->rax = (uint64_t)-1; break; }

        uint64_t packed = r->rdi;
        int32_t x = (int32_t)(uint16_t)(packed >> 48);
        int32_t y = (int32_t)(uint16_t)(packed >> 32);
        int32_t w = (int32_t)(uint16_t)(packed >> 16);
        int32_t h = (int32_t)(uint16_t)packed;

        gfx_present_rect(x, y, w, h);
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
