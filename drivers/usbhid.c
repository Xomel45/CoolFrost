/* ─────────────────────────────────────────────────────────────────────────
 * USB HID Boot Protocol driver  (keyboard + mouse)
 *
 * Relies on xhci.c having already enumerated devices (xhci_scan_devices).
 * For each device with bDeviceClass=0 or 3 / HID boot-protocol interface:
 *   1. Reads full configuration descriptor
 *   2. Finds Interrupt IN endpoint
 *   3. Sends SET_PROTOCOL(Boot) + SET_IDLE
 *   4. Configures the endpoint via CONFIGURE_ENDPOINT xHCI command
 *   5. Enqueues the first transfer TRB
 *
 * usbhid_poll() is called from the timer ISR every tick.  It checks the
 * xHCI event ring for completed transfer TRBs and either:
 *   - Keyboard: translates HID key codes → PS/2 scancodes via
 *               kbd_inject_scancode() so getline() picks them up.
 *   - Mouse: calls mouse_inject_delta() to update shared mouse state.
 * ─────────────────────────────────────────────────────────────────────────*/
#include "usbhid.h"
#include "xhci.h"
#include "usb.h"
#include "keyboard.h"
#include "mouse.h"

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define USBHID_MAX_DEV   4u
#define HID_RING_SZ      16u  /* small ring: we only ever have 1 TRB in flight */

/* ── Device type tags ───────────────────────────────────────────────────── */
#define HID_TYPE_KBD  1u
#define HID_TYPE_MOUSE 2u

/* ── Per-device state ───────────────────────────────────────────────────── */
typedef struct {
    uint8_t  valid;
    uint8_t  slot;
    uint8_t  epid;        /* xHCI endpoint ID for interrupt IN (odd number) */
    uint8_t  report_len;
    uint8_t  type;        /* HID_TYPE_KBD or HID_TYPE_MOUSE */
    uint8_t  pending;     /* 1 = TRB is in flight */
    uint8_t  prev[8];     /* previous report (keyboard: detect new keys) */
} hid_dev_t;

static hid_dev_t hid_devs[USBHID_MAX_DEV];
static uint32_t  hid_count  = 0;
static uint32_t  hid_kbd_n  = 0;  /* number of keyboards found */
static uint32_t  hid_mse_n  = 0;  /* number of mice found      */

/* Interrupt endpoint transfer rings (one per HID device) */
static xhci_trb_t hid_rings[USBHID_MAX_DEV][HID_RING_SZ] __attribute__((aligned(64)));
static uint32_t   hid_enq[USBHID_MAX_DEV];
static uint8_t    hid_cs [USBHID_MAX_DEV];

/* DMA-aligned report buffers */
static uint8_t hid_buf[USBHID_MAX_DEV][8] __attribute__((aligned(64)));

/* Scratch buffer for config descriptor */
static uint8_t cfg_buf[512] __attribute__((aligned(64)));

/* ── HID modifier bits ──────────────────────────────────────────────────── */
#define HID_MOD_LSHIFT  0x02u
#define HID_MOD_RSHIFT  0x20u
#define HID_MOD_SHIFT   (HID_MOD_LSHIFT | HID_MOD_RSHIFT)

/* ── HID usage ID → PS/2 scancode table (boot-protocol key codes) ───────
 * Entries not listed are 0 (no mapping).  Values > 0x5F are the virtual
 * scancodes from keyboard.h (KEY_UP/DOWN/LEFT/RIGHT = 0x60-0x63).       */
static const uint8_t hid_to_ps2[0x80] = {
    [0x04] = 0x1E, /* a  */  [0x05] = 0x30, /* b  */
    [0x06] = 0x2E, /* c  */  [0x07] = 0x20, /* d  */
    [0x08] = 0x12, /* e  */  [0x09] = 0x21, /* f  */
    [0x0A] = 0x22, /* g  */  [0x0B] = 0x23, /* h  */
    [0x0C] = 0x17, /* i  */  [0x0D] = 0x24, /* j  */
    [0x0E] = 0x25, /* k  */  [0x0F] = 0x26, /* l  */
    [0x10] = 0x32, /* m  */  [0x11] = 0x31, /* n  */
    [0x12] = 0x18, /* o  */  [0x13] = 0x19, /* p  */
    [0x14] = 0x10, /* q  */  [0x15] = 0x13, /* r  */
    [0x16] = 0x1F, /* s  */  [0x17] = 0x14, /* t  */
    [0x18] = 0x16, /* u  */  [0x19] = 0x2F, /* v  */
    [0x1A] = 0x11, /* w  */  [0x1B] = 0x2D, /* x  */
    [0x1C] = 0x15, /* y  */  [0x1D] = 0x2C, /* z  */
    [0x1E] = 0x02, /* 1  */  [0x1F] = 0x03, /* 2  */
    [0x20] = 0x04, /* 3  */  [0x21] = 0x05, /* 4  */
    [0x22] = 0x06, /* 5  */  [0x23] = 0x07, /* 6  */
    [0x24] = 0x08, /* 7  */  [0x25] = 0x09, /* 8  */
    [0x26] = 0x0A, /* 9  */  [0x27] = 0x0B, /* 0  */
    [0x28] = 0x1C, /* Enter     */
    [0x29] = 0x01, /* Escape    */
    [0x2A] = 0x0E, /* Backspace */
    [0x2B] = 0x0F, /* Tab       */
    [0x2C] = 0x39, /* Space     */
    [0x2D] = 0x0C, /* -         */  [0x2E] = 0x0D, /* =  */
    [0x2F] = 0x1A, /* [         */  [0x30] = 0x1B, /* ]  */
    [0x31] = 0x2B, /* \         */
    [0x33] = 0x27, /* ;         */  [0x34] = 0x28, /* '  */
    [0x35] = 0x29, /* `         */
    [0x36] = 0x33, /* ,         */  [0x37] = 0x34, /* .  */
    [0x38] = 0x35, /* /         */
    /* Arrow keys (virtual scancodes, must match keyboard.h defines) */
    [0x4F] = KEY_RIGHT,
    [0x50] = KEY_LEFT,
    [0x51] = KEY_DOWN,
    [0x52] = KEY_UP,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configuration descriptor parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Scan the raw config descriptor bytes in buf[0..len) for a HID boot-
 * protocol interface with an Interrupt IN endpoint.
 * On success: fills *iface_num, *ep_addr, *ep_maxpkt, *ep_interval, *hid_type
 * Returns 1 if found, 0 otherwise.                                        */
static int find_hid_boot_ep(uint8_t *buf, uint16_t len,
                              uint8_t *iface_num,
                              uint8_t *ep_addr,
                              uint16_t *ep_maxpkt,
                              uint8_t *ep_interval,
                              uint8_t *hid_type) {
    uint8_t *p   = buf;
    uint8_t *end = buf + len;
    int      found_hid  = 0;
    uint8_t  cur_iface  = 0;
    uint8_t  cur_proto  = 0;

    while (p < end) {
        if (p + 2 > end) break;
        uint8_t dlen  = p[0];
        uint8_t dtype = p[1];
        if (dlen == 0) break;

        if (dtype == USB_DESC_INTERFACE) {
            usb_iface_desc_t *id = (usb_iface_desc_t *)p;
            cur_iface = id->bInterfaceNumber;
            /* HID boot-protocol: class=HID(3), subclass=Boot(1), proto=1(kbd)/2(mse) */
            found_hid = (id->bInterfaceClass    == USB_CLASS_HID &&
                         id->bInterfaceSubClass == USB_HID_SC_BOOT);
            cur_proto = id->bInterfaceProtocol;
        } else if (dtype == USB_DESC_ENDPOINT && found_hid) {
            usb_ep_desc_t *ed = (usb_ep_desc_t *)p;
            /* Interrupt IN: direction bit set, transfer type = 0x03 */
            if ((ed->bEndpointAddress & 0x80u) &&
                (ed->bmAttributes  & 0x03u) == 0x03u) {
                *iface_num  = cur_iface;
                *ep_addr    = ed->bEndpointAddress;
                *ep_maxpkt  = (uint16_t)(ed->wMaxPacketSize & 0x7FFu);
                *ep_interval = ed->bInterval;
                *hid_type   = (cur_proto == USB_HID_P_MOUSE) ? HID_TYPE_MOUSE
                                                              : HID_TYPE_KBD;
                return 1;
            }
        }
        p += dlen;
    }
    return 0;
}

/* ── Convert bInterval to xHCI Interval field (in 125 μs units) ─────────  */
static uint8_t ep_interval_field(uint8_t speed, uint8_t bInterval) {
    if (speed >= USB_SPEED_HIGH) {
        /* SS/HS: 2^(bInterval-1) * 125 μs → Interval = bInterval - 1   */
        return (bInterval > 0u) ? bInterval - 1u : 0u;
    }
    /* FS/LS: bInterval is in ms.  Nearest Interval = ceil(log2(bInterval*8))
     * For HID boot devices bInterval is typically 10 → use 7 (≈16 ms).  */
    if (bInterval == 0u) return 7u;
    uint8_t log2v = 0;
    uint16_t v = (uint16_t)bInterval * 8u;
    while (v > 1u) { v >>= 1; log2v++; }
    return log2v;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HID class control requests
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t hid_set_protocol(uint8_t slot, uint8_t iface) {
    /* SET_PROTOCOL(Boot = 0) */
    usb_setup_t s = {
        .bmRequestType = 0x21u,  /* Class | Interface | H2D */
        .bRequest      = 0x0Bu,  /* SET_PROTOCOL */
        .wValue        = 0u,     /* Boot Protocol */
        .wIndex        = iface,
        .wLength       = 0u,
    };
    return xhci_ctrl_xfer(slot, &s, (void *)0, 0u);
}

static uint8_t hid_set_idle(uint8_t slot, uint8_t iface) {
    /* SET_IDLE(duration=0 → send only on change, report_id=0 → all) */
    usb_setup_t s = {
        .bmRequestType = 0x21u,
        .bRequest      = 0x0Au,  /* SET_IDLE */
        .wValue        = 0u,
        .wIndex        = iface,
        .wLength       = 0u,
    };
    return xhci_ctrl_xfer(slot, &s, (void *)0, 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Build CONFIGURE_ENDPOINT Input Context for an interrupt IN endpoint
 * ═══════════════════════════════════════════════════════════════════════════ */
static void build_intr_ep_input_ctx(xhci_input_ctx_t *ictx,
                                     uint8_t  epid,
                                     uint8_t  speed,
                                     uint8_t  port,
                                     uint16_t maxpkt,
                                     uint8_t  interval,
                                     xhci_trb_t *ring) {
    /* Zero entire context */
    uint32_t *p = (uint32_t *)ictx;
    for (uint32_t i = 0; i < sizeof(*ictx) / 4u; i++) p[i] = 0;

    /* Input Control Context: add Slot + this endpoint */
    ictx->icc.dw[1] = (1u << 0) | (1u << epid);   /* A0 (slot) + Aepid */

    /* Slot Context: CTX_ENTRIES = epid (must be >= highest configured EP) */
    ictx->dev.slot.dw[0] = SLOT_CTX_SPEED(speed) | SLOT_CTX_CTX_ENT(epid);
    ictx->dev.slot.dw[1] = SLOT_CTX_ROOTPORT((uint32_t)port + 1u);

    /* EP Context at index (epid - 1) */
    xhci_ctx_t *ec = &ictx->dev.ep[epid - 1u];
    ec->dw[0] = EP_CTX_INTERVAL(ep_interval_field(speed, interval));
    ec->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_INT_IN) |
                EP_CTX_CERR(3u) |
                EP_CTX_MAX_PKT(maxpkt);
    uint64_t tr_ptr = (uint64_t)(uintptr_t)ring | 1u;  /* DCS = 1 */
    ec->dw[2] = (uint32_t)(tr_ptr & 0xFFFFFFFFu);
    ec->dw[3] = (uint32_t)(tr_ptr >> 32);
    ec->dw[4] = 1u;   /* Average TRB Length — HID reports are tiny */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Keyboard report processing
 * ═══════════════════════════════════════════════════════════════════════════ */
static void process_kbd_report(hid_dev_t *hd, uint8_t *curr) {
    uint8_t *prev = hd->prev;

    /* ── Modifier changes (shift keys only) ── */
    uint8_t prev_shift = !!(prev[0] & HID_MOD_SHIFT);
    uint8_t curr_shift = !!(curr[0] & HID_MOD_SHIFT);
    if (!prev_shift && curr_shift)  kbd_inject_scancode(0x2Au); /* LShift make  */
    if (prev_shift  && !curr_shift) kbd_inject_scancode(0xAAu); /* LShift break */

    /* ── Newly pressed keys (in curr[2..7] but not in prev[2..7]) ── */
    for (int i = 2; i < 8; i++) {
        uint8_t k = curr[i];
        if (k == 0x00u || k == 0x01u) continue;  /* no key / rollover */
        int held = 0;
        for (int j = 2; j < 8; j++)
            if (prev[j] == k) { held = 1; break; }
        if (held) continue;

        uint8_t sc = (k < 0x80u) ? hid_to_ps2[k] : 0u;
        if (sc) {
            kbd_inject_scancode(sc);
            break;   /* one key at a time into cur_scancode */
        }
    }

    /* ── Released keys (in prev[2..7] but not in curr[2..7]) ── */
    for (int i = 2; i < 8; i++) {
        uint8_t k = prev[i];
        if (k == 0x00u || k == 0x01u) continue;
        int still_held = 0;
        for (int j = 2; j < 8; j++)
            if (curr[j] == k) { still_held = 1; break; }
        if (still_held) continue;

        uint8_t sc = (k < 0x80u) ? hid_to_ps2[k] : 0u;
        if (sc && sc < 0x60u)        /* don't send break for virtual codes */
            kbd_inject_scancode(sc | 0x80u);
    }

    /* Save current as previous */
    for (int i = 0; i < 8; i++) prev[i] = curr[i];
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mouse report processing
 * ═══════════════════════════════════════════════════════════════════════════ */
static void process_mouse_report(uint8_t *rpt, uint8_t len) {
    if (len < 3u) return;
    uint8_t  btns = rpt[0] & 0x07u;
    int      dx   = (int8_t)rpt[1];
    int      dy   = (int8_t)rpt[2];
    mouse_inject_delta(dx, dy, btns);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Enqueue one Normal TRB into a HID device's interrupt ring
 * ═══════════════════════════════════════════════════════════════════════════ */
static void hid_enqueue_trb(uint32_t idx) {
    hid_dev_t *hd = &hid_devs[idx];
    uint8_t  *buf = hid_buf[idx];
    xhci_ring_enqueue(hid_rings[idx], &hid_enq[idx], &hid_cs[idx],
                      (uint64_t)(uintptr_t)buf,
                      (uint32_t)hd->report_len,
                      TRB_TYPE(TRBTYPE_NORMAL) | TRB_IOC);
    xhci_mb();
    xhci_ep_doorbell(hd->slot, hd->epid);
    hd->pending = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  usbhid_init
 * ═══════════════════════════════════════════════════════════════════════════ */
void usbhid_init(void) {
    int total = usb_device_count();

    for (int i = 0; i < total && (int)hid_count < (int)USBHID_MAX_DEV; i++) {
        usb_device_t *ud = usb_get_device(i);
        if (!ud || !ud->valid) continue;

        /* ── 1. Fetch configuration descriptor (first 9 bytes for wTotalLength) */
        usb_setup_t s_cfg = {
            .bmRequestType = USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
            .bRequest      = USB_REQ_GET_DESCRIPTOR,
            .wValue        = (uint16_t)(USB_DESC_CONFIG << 8),
            .wIndex        = 0,
            .wLength       = 9,
        };
        uint8_t cc = xhci_ctrl_xfer(ud->slot, &s_cfg, cfg_buf, 9);
        if (cc != CC_SUCCESS) continue;

        uint16_t total_len = (uint16_t)(cfg_buf[2] | ((uint16_t)cfg_buf[3] << 8));
        if (total_len > (uint16_t)sizeof(cfg_buf)) total_len = (uint16_t)sizeof(cfg_buf);

        /* ── 2. Fetch full configuration descriptor ─────────────────────── */
        s_cfg.wLength = total_len;
        cc = xhci_ctrl_xfer(ud->slot, &s_cfg, cfg_buf, total_len);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) continue;

        /* ── 3. Parse for HID boot-protocol interrupt IN endpoint ───────── */
        uint8_t  iface_num  = 0;
        uint8_t  ep_addr    = 0;
        uint16_t ep_maxpkt  = 0;
        uint8_t  ep_interval = 0;
        uint8_t  hid_type   = 0;

        if (!find_hid_boot_ep(cfg_buf, total_len,
                              &iface_num, &ep_addr,
                              &ep_maxpkt, &ep_interval, &hid_type))
            continue;

        /* ── 4. HID class setup ─────────────────────────────────────────── */
        hid_set_protocol(ud->slot, iface_num);
        hid_set_idle(ud->slot, iface_num);

        /* ── 5. xHCI endpoint ID: ep_num*2 + 1 for IN ───────────────────── */
        uint8_t ep_num = ep_addr & 0x0Fu;
        uint8_t epid   = (uint8_t)(ep_num * 2u + 1u);

        /* ── 6. Clamp max packet size (HID reports ≤ 8 bytes) ───────────── */
        if (ep_maxpkt > 8u) ep_maxpkt = 8u;

        /* ── 7. Initialise interrupt transfer ring ───────────────────────── */
        uint32_t idx = hid_count;
        xhci_ring_init(hid_rings[idx], HID_RING_SZ);
        hid_enq[idx] = 0;
        hid_cs[idx]  = 1;
        xhci_mb();

        /* ── 8. Build and send CONFIGURE_ENDPOINT input context ──────────── */
        xhci_input_ctx_t *ictx = xhci_get_input_ctx();
        build_intr_ep_input_ctx(ictx, epid, ud->speed, ud->port,
                                ep_maxpkt, ep_interval, hid_rings[idx]);
        xhci_mb();

        cc = xhci_cfg_ep(ud->slot, ictx);
        if (cc != CC_SUCCESS) continue;

        /* ── 9. Store device record ──────────────────────────────────────── */
        hid_devs[idx].valid      = 1;
        hid_devs[idx].slot       = ud->slot;
        hid_devs[idx].epid       = epid;
        hid_devs[idx].report_len = (uint8_t)ep_maxpkt;
        hid_devs[idx].type       = hid_type;
        hid_devs[idx].pending    = 0;
        for (int j = 0; j < 8; j++) hid_devs[idx].prev[j] = 0;

        if (hid_type == HID_TYPE_KBD)   hid_kbd_n++;
        else                             hid_mse_n++;
        hid_count++;

        /* ── 10. Enqueue first TRB so the device can send its first report ─ */
        hid_enqueue_trb(idx);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  usbhid_poll  — called from timer ISR every tick
 * ═══════════════════════════════════════════════════════════════════════════ */
void usbhid_poll(void) {
    if (hid_count == 0) return;

    /* Check at most one event per call — the timer fires often enough */
    xhci_trb_t ev;
    if (!xhci_evt_try(&ev)) return;

    /* Only handle Transfer Events */
    if (TRB_GET_TYPE(ev.ctrl) != TRBTYPE_EVT_XFER) return;

    uint8_t slot  = (uint8_t)TRB_GET_SLOT(ev.ctrl);
    uint8_t epid  = (uint8_t)((ev.ctrl >> 16) & 0x1Fu);
    uint8_t code  = (uint8_t)TRB_GET_CC(ev.status);

    /* Match the event to a registered HID device */
    for (uint32_t i = 0; i < hid_count; i++) {
        hid_dev_t *hd = &hid_devs[i];
        if (!hd->valid || !hd->pending) continue;
        if (hd->slot != slot || hd->epid != epid) continue;

        hd->pending = 0;

        if (code == CC_SUCCESS || code == CC_SHORT_PKT) {
            uint8_t *buf = hid_buf[i];
            if (hd->type == HID_TYPE_KBD)
                process_kbd_report(hd, buf);
            else
                process_mouse_report(buf, hd->report_len);
        }

        /* Re-arm: enqueue next TRB for continuous polling */
        hid_enqueue_trb(i);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public predicates
 * ═══════════════════════════════════════════════════════════════════════════ */
int usbhid_kbd_present(void)   { return hid_kbd_n  > 0u; }
int usbhid_mouse_present(void) { return hid_mse_n  > 0u; }
