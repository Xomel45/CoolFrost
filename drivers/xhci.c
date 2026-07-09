/* ─────────────────────────────────────────────────────────────────────────
 * xHCI (eXtensible Host Controller Interface) driver
 *
 * Supports:
 *   - PCI discovery (class 0x0C / subclass 0x03 / prog-if 0x30)
 *   - Controller reset + initialisation (command ring, event ring, DCBAA)
 *   - Port reset and device enumeration via control transfers
 *   - Reads USB device descriptor (VID/PID/class) for each connected device
 *
 * Design constraints:
 *   - Polling only (no MSI / MSI-X)
 *   - 32-byte contexts (CSZ=0); bails out if CSZ=1
 *   - Single event ring segment (ERSTSZ=1)
 *   - Control endpoint (EP0) only — no interrupt/bulk endpoint support
 *   - Identity-mapped physical memory (phys == virt for all DMA buffers)
 * ─────────────────────────────────────────────────────────────────────────*/
#include "xhci.h"
#include "usb.h"
#include "pci.h"
#include "../cpu/timer.h"

/* ── Tunable constants ──────────────────────────────────────────────────── */
#define XHCI_RING_SIZE   64u    /* TRBs per ring (last = Link TRB) */
#define XHCI_MAX_SLOTS   16u    /* max device slots we support       */
#define XHCI_POLL_LIMIT  500000u /* iterations before timeout         */

/* ── Static DMA buffers (identity-mapped: phys == virt) ─────────────────── */
static uint64_t      dcbaa[XHCI_MAX_SLOTS + 1] __attribute__((aligned(64)));
static xhci_dev_ctx_t dev_ctx[XHCI_MAX_SLOTS]  __attribute__((aligned(64)));
static xhci_input_ctx_t input_ctx              __attribute__((aligned(64)));
static xhci_trb_t    cmd_ring[XHCI_RING_SIZE]  __attribute__((aligned(64)));
static xhci_trb_t    evt_ring[XHCI_RING_SIZE]  __attribute__((aligned(64)));
static xhci_erst_t   erst[1]                   __attribute__((aligned(64)));
/* One EP0 transfer ring per slot */
static xhci_trb_t    xfer_rings[XHCI_MAX_SLOTS][XHCI_RING_SIZE]
                                                __attribute__((aligned(64)));
/* Shared DMA buffer for control transfer data */
static uint8_t       ctrl_buf[4096]            __attribute__((aligned(64)));

/* ── Controller register base pointers ──────────────────────────────────── */
static volatile uint8_t  *cap_base;  /* capability register base           */
static volatile uint8_t  *op_base;   /* operational register base          */
static volatile uint32_t *db_base;   /* doorbell array base                */
static volatile uint8_t  *rt_base;   /* runtime register base              */

/* ── Command ring producer state ────────────────────────────────────────── */
static uint32_t cmd_enq = 0;
static uint8_t  cmd_cs  = 1;   /* producer cycle state (start with 1)     */

/* ── Event ring consumer state ──────────────────────────────────────────── */
static uint32_t evt_deq = 0;
static uint8_t  evt_cs  = 1;   /* consumer cycle state (start with 1)     */

/* ── Per-slot transfer ring producer state ──────────────────────────────── */
static uint32_t xfer_enq[XHCI_MAX_SLOTS];
static uint8_t  xfer_cs[XHCI_MAX_SLOTS];

/* ── Discovered USB devices ─────────────────────────────────────────────── */
static usb_device_t usb_devs[USB_MAX_DEV];
static int          usb_dev_cnt = 0;

/* ── Max root-hub ports detected at init ────────────────────────────────── */
static uint32_t num_ports = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Convenience register accessors
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CAP_R8(off)  xhci_r8 ((volatile uint8_t  *)(cap_base + (off)))
#define CAP_R32(off) xhci_r32((volatile uint32_t *)(cap_base + (off)))
#define OP_R32(off)  xhci_r32((volatile uint32_t *)(op_base  + (off)))
#define OP_W32(off,v) xhci_w32((volatile uint32_t *)(op_base + (off)), (v))
#define OP_R64(off)  xhci_r64((volatile uint64_t *)(op_base  + (off)))
#define OP_W64(off,v) xhci_w64((volatile uint64_t *)(op_base + (off)), (v))
#define RT_R32(off)  xhci_r32((volatile uint32_t *)(rt_base  + (off)))
#define RT_W32(off,v) xhci_w32((volatile uint32_t *)(rt_base + (off)), (v))
#define RT_R64(off)  xhci_r64((volatile uint64_t *)(rt_base  + (off)))
#define RT_W64(off,v) xhci_w64((volatile uint64_t *)(rt_base + (off)), (v))

/* ── Interrupter 0 helpers ────────────────────────────────────────────────  */
#define IR0_OFF(r)  (XHCI_RT_IR0 + (r))
#define IR0_R32(r)  RT_R32(IR0_OFF(r))
#define IR0_W32(r,v)RT_W32(IR0_OFF(r),(v))
#define IR0_R64(r)  RT_R64(IR0_OFF(r))
#define IR0_W64(r,v)RT_W64(IR0_OFF(r),(v))

/* ═══════════════════════════════════════════════════════════════════════════
 *  Ring management
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Zero a ring and install a Link TRB at the last slot.
 * The Link TRB is written with cycle=0; it only becomes "live" when the
 * producer wraps and updates it with the current cycle bit.              */
static void ring_init(xhci_trb_t *ring, uint32_t sz) {
    for (uint32_t i = 0; i < sz; i++) {
        ring[i].param  = 0;
        ring[i].status = 0;
        ring[i].ctrl   = 0;
    }
    /* Install Link TRB at last position, pointing back to ring[0] */
    ring[sz - 1].param = (uint64_t)(uintptr_t)ring;
    ring[sz - 1].ctrl  = TRB_TYPE(TRBTYPE_LINK) | TRB_TC;
    /* cycle bit = 0: hardware won't process this until we update it    */
}

/* Enqueue one TRB into the command ring or a transfer ring.
 * 'cs' / 'enq' are the producer's cycle state and enqueue index.
 * Returns 0 on success, -1 if the ring is full (sanity check).         */
static int ring_enqueue(xhci_trb_t *ring, uint32_t *enq, uint8_t *cs,
                        uint64_t param, uint32_t status, uint32_t ctrl) {
    if (*enq >= XHCI_RING_SIZE - 1) return -1;  /* should not happen */

    xhci_trb_t *trb = &ring[*enq];
    trb->param  = param;
    trb->status = status;
    /* Set cycle bit last (after all other fields) with a store barrier */
    __asm__ volatile("sfence" ::: "memory");
    trb->ctrl = ctrl | (*cs ? TRB_C : 0);

    (*enq)++;

    /* If we've filled up to the Link TRB position, wrap the ring */
    if (*enq == XHCI_RING_SIZE - 1) {
        xhci_trb_t *link = &ring[*enq];
        link->ctrl = TRB_TYPE(TRBTYPE_LINK) | TRB_TC | (*cs ? TRB_C : 0);
        *enq = 0;
        *cs ^= 1;
    }
    return 0;
}

/* ── Command ring wrapper ─────────────────────────────────────────────────  */
static void cmd_enqueue(uint64_t param, uint32_t status, uint32_t ctrl) {
    ring_enqueue(cmd_ring, &cmd_enq, &cmd_cs, param, status, ctrl);
}

/* Ring command doorbell (slot 0) */
static void cmd_doorbell(void) {
    xhci_wmb();
    xhci_w32(&db_base[0], XHCI_DB_HOST_CMD);
}

/* Ring endpoint doorbell for slot/ep */
static void ep_doorbell(uint8_t slot, uint8_t ep) {
    xhci_wmb();
    xhci_w32(&db_base[slot], (uint32_t)ep);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Event ring polling
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Poll the event ring for the next TRB.  Returns 1 if a TRB was
 * available (copied into *out), 0 on timeout.                            */
static int evt_poll(xhci_trb_t *out) {
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        xhci_trb_t *trb = &evt_ring[evt_deq];
        /* Check cycle bit of the TRB against our expected state */
        uint32_t ctrl = xhci_r32((volatile uint32_t *)&trb->ctrl);
        if ((ctrl & TRB_C) == (uint32_t)evt_cs) {
            /* Valid event — copy it */
            out->param  = xhci_r64((volatile uint64_t *)&trb->param);
            out->status = xhci_r32((volatile uint32_t *)&trb->status);
            out->ctrl   = ctrl;

            /* Advance dequeue; wrap and toggle cycle if needed */
            evt_deq++;
            if (evt_deq == XHCI_RING_SIZE) {
                evt_deq = 0;
                evt_cs ^= 1;
            }

            /* Update ERDP register (clear EHB, set new dequeue pointer) */
            uint64_t erdp = (uint64_t)(uintptr_t)&evt_ring[evt_deq];
            IR0_W64(XHCI_IR_ERDP, erdp | (1u << 3)); /* EHB=1 to clear */

            return 1;
        }
        xhci_relax();
    }
    return 0;   /* timeout */
}

/* Wait for a specific event TRB type.
 * Discards other events until the expected type is seen or timeout.
 * Returns completion code (0 = CC_INVALID on timeout).                  */
static uint8_t wait_event(uint8_t expected_type) {
    xhci_trb_t ev;
    while (evt_poll(&ev)) {
        uint8_t type = (uint8_t)TRB_GET_TYPE(ev.ctrl);
        if (type == expected_type)
            return (uint8_t)TRB_GET_CC(ev.status);
    }
    return CC_INVALID;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Command helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t cmd_enable_slot(uint8_t *slot_out) {
    cmd_enqueue(0, 0, TRB_TYPE(TRBTYPE_EN_SLOT));
    cmd_doorbell();

    xhci_trb_t ev;
    while (evt_poll(&ev)) {
        if (TRB_GET_TYPE(ev.ctrl) == TRBTYPE_EVT_CMD) {
            *slot_out = (uint8_t)TRB_GET_SLOT(ev.ctrl);
            return (uint8_t)TRB_GET_CC(ev.status);
        }
    }
    *slot_out = 0;
    return CC_INVALID;
}

static uint8_t cmd_address_device(uint8_t slot, int bsr) {
    uint64_t ictx_phys = (uint64_t)(uintptr_t)&input_ctx;
    uint32_t ctrl = TRB_TYPE(TRBTYPE_ADDR_DEV) | TRB_SLOT(slot) |
                    (bsr ? TRB_BSR : 0);
    cmd_enqueue(ictx_phys, 0, ctrl);
    cmd_doorbell();
    return wait_event(TRBTYPE_EVT_CMD);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Control transfer helper
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Perform a USB control transfer on EP0 of 'slot'.
 * setup  — the 8-byte setup packet
 * buf    — data buffer (NULL if no data phase)
 * len    — transfer length (0 if no data phase)
 * Returns CC_SUCCESS on success.                                          */
static uint8_t control_xfer(uint8_t slot,
                             const usb_setup_t *setup,
                             void *buf, uint16_t len) {
    xhci_trb_t *ring = xfer_rings[slot - 1];
    uint32_t   *enq  = &xfer_enq[slot - 1];
    uint8_t    *cs   = &xfer_cs [slot - 1];

    /* Determine TRT (Transfer Request Type) for Setup Stage TRB */
    uint8_t trt;
    if (len == 0)                        trt = TRT_NO_DATA;
    else if (setup->bmRequestType & 0x80) trt = TRT_IN_DATA;
    else                                  trt = TRT_OUT_DATA;

    /* Setup Stage TRB: 8-byte setup packet embedded as immediate data */
    uint64_t sp_lo = ((uint32_t)setup->bmRequestType)
                   | ((uint32_t)setup->bRequest  <<  8)
                   | ((uint32_t)setup->wValue    << 16);
    uint64_t sp_hi = ((uint32_t)setup->wIndex)
                   | ((uint32_t)setup->wLength   << 16);
    uint64_t sp_param = sp_lo | (sp_hi << 32);

    ring_enqueue(ring, enq, cs, sp_param, 8,
                 TRB_TYPE(TRBTYPE_SETUP) | TRB_IDT | TRB_TRT(trt));

    /* Data Stage TRB (only if data phase exists) */
    if (len > 0 && buf) {
        uint32_t dctrl = TRB_TYPE(TRBTYPE_DATA) | TRB_IOC |
                         ((setup->bmRequestType & 0x80) ? TRB_DIR : 0);
        ring_enqueue(ring, enq, cs,
                     (uint64_t)(uintptr_t)buf, (uint32_t)len, dctrl);
    }

    /* Status Stage TRB: direction = opposite of data, or IN if no data */
    uint32_t stat_dir = (len > 0 && (setup->bmRequestType & 0x80)) ? 0u : TRB_DIR;
    ring_enqueue(ring, enq, cs, 0, 0,
                 TRB_TYPE(TRBTYPE_STATUS) | TRB_IOC | stat_dir);

    ep_doorbell(slot, XHCI_DB_EP0);

    /* Wait for the Transfer Event on the Status Stage TRB */
    return wait_event(TRBTYPE_EVT_XFER);
}

/* ── GET_DESCRIPTOR helper ────────────────────────────────────────────────  */
static uint8_t get_descriptor(uint8_t slot, uint8_t type, uint8_t idx,
                               void *buf, uint16_t len) {
    usb_setup_t s = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_STD | USB_RECIP_DEV,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16_t)((uint16_t)type << 8) | idx,
        .wIndex        = 0,
        .wLength       = len,
    };
    return control_xfer(slot, &s, buf, len);
}

/* ── SET_CONFIGURATION helper ─────────────────────────────────────────────  */
static uint8_t set_configuration(uint8_t slot, uint8_t cfg_val) {
    usb_setup_t s = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STD | USB_RECIP_DEV,
        .bRequest      = USB_REQ_SET_CONFIG,
        .wValue        = cfg_val,
        .wIndex        = 0,
        .wLength       = 0,
    };
    return control_xfer(slot, &s, (void *)0, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Input context setup helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_slot_ctx(xhci_ctx_t *sc, uint8_t speed, uint8_t port) {
    /* dw0: speed, CTX_ENTRIES=1 (only EP0 context follows) */
    sc->dw[0] = SLOT_CTX_SPEED(speed) | SLOT_CTX_CTX_ENT(1);
    /* dw1: root hub port number (1-based) */
    sc->dw[1] = SLOT_CTX_ROOTPORT((uint32_t)port + 1u);
    sc->dw[2] = sc->dw[3] = sc->dw[4] = sc->dw[5] = 0;
    sc->dw[6] = sc->dw[7] = 0;
}

static void build_ep0_ctx(xhci_ctx_t *ec, uint8_t slot, uint16_t max_pkt) {
    /* dw1: EP type = Control (4), CERR=3, MaxPacketSize */
    ec->dw[0] = 0;
    ec->dw[1] = EP_CTX_EP_TYPE(EP_TYPE_CONTROL) | EP_CTX_CERR(3) |
                EP_CTX_MAX_PKT(max_pkt);
    /* dw2/3: TR Dequeue Pointer + DCS=1 */
    uint64_t tr_ptr = (uint64_t)(uintptr_t)xfer_rings[slot - 1] | 1u;
    ec->dw[2] = (uint32_t)(tr_ptr & 0xFFFFFFFFu);
    ec->dw[3] = (uint32_t)(tr_ptr >> 32);
    /* dw4: Average TRB Length = 8 (control endpoint) */
    ec->dw[4] = 8u;
    ec->dw[5] = ec->dw[6] = ec->dw[7] = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Port probing and device enumeration
 * ═══════════════════════════════════════════════════════════════════════════ */

static void probe_port(uint32_t port) {
    /* 1. Read PORTSC — check if device is connected */
    uint32_t ps = OP_R32(XHCI_OP_PORTSC(port));
    if (!(ps & XHCI_PS_CCS)) return;   /* no device */

    /* 2. Port reset: preserve PP, set PR, clear all change bits */
    uint32_t rst = XHCI_PS_PP | XHCI_PS_PR | XHCI_PS_W1C_MASK;
    OP_W32(XHCI_OP_PORTSC(port), rst);

    /* 3. Wait for PRC (port reset change) to indicate reset complete */
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        ps = OP_R32(XHCI_OP_PORTSC(port));
        if (ps & XHCI_PS_PRC) break;
        xhci_relax();
    }
    /* Clear PRC */
    OP_W32(XHCI_OP_PORTSC(port), XHCI_PS_PP | XHCI_PS_PRC);

    /* 4. Re-read PORTSC to get actual speed */
    ps = OP_R32(XHCI_OP_PORTSC(port));
    if (!(ps & XHCI_PS_CCS)) return;   /* device disconnected during reset */
    uint8_t speed = (uint8_t)XHCI_PS_SPEED(ps);

    /* 5. Enable Slot */
    uint8_t slot = 0;
    if (cmd_enable_slot(&slot) != CC_SUCCESS || !slot) return;

    /* 6. Map device context in DCBAA */
    xhci_dev_ctx_t *dc = &dev_ctx[slot - 1];
    for (uint32_t i = 0; i < sizeof(*dc) / 4; i++)
        ((volatile uint32_t *)dc)[i] = 0;
    xhci_mb();
    dcbaa[slot] = (uint64_t)(uintptr_t)dc;

    /* 7. Init transfer ring for this slot */
    ring_init(xfer_rings[slot - 1], XHCI_RING_SIZE);
    xfer_enq[slot - 1] = 0;
    xfer_cs [slot - 1] = 1;
    xhci_mb();

    /* 8. Build Input Context for Address Device */
    for (uint32_t i = 0; i < sizeof(input_ctx) / 4; i++)
        ((volatile uint32_t *)&input_ctx)[i] = 0;

    input_ctx.icc.dw[1] = ICC_ADD_SLOT | ICC_ADD_EP0;
    build_slot_ctx(&input_ctx.dev.slot, speed, port);

    uint16_t max_pkt = usb_ep0_maxpkt(speed);
    build_ep0_ctx(&input_ctx.dev.ep[0], slot, max_pkt);
    xhci_mb();

    /* 9. Address Device (BSR=0 → controller assigns USB address) */
    if (cmd_address_device(slot, 0) != CC_SUCCESS) return;

    /* 10. GET_DESCRIPTOR (first 8 bytes) to find bMaxPacketSize0 */
    uint8_t cc = get_descriptor(slot, USB_DESC_DEVICE, 0, ctrl_buf, 8);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return;

    usb_dev_desc_t *hdr = (usb_dev_desc_t *)ctrl_buf;
    uint8_t real_pkt = hdr->bMaxPacketSize0;

    /* 11. If max packet size changed, update EP0 context (Evaluate Context) */
    if (real_pkt != (uint8_t)max_pkt) {
        for (uint32_t i = 0; i < sizeof(input_ctx) / 4; i++)
            ((volatile uint32_t *)&input_ctx)[i] = 0;
        input_ctx.icc.dw[1] = ICC_ADD_EP0;   /* only update EP0 */
        build_ep0_ctx(&input_ctx.dev.ep[0], slot, (uint16_t)real_pkt);
        xhci_mb();

        uint64_t ictx = (uint64_t)(uintptr_t)&input_ctx;
        cmd_enqueue(ictx, 0,
                    TRB_TYPE(TRBTYPE_EVAL_CTX) | TRB_SLOT(slot));
        cmd_doorbell();
        wait_event(TRBTYPE_EVT_CMD);  /* ignore result for now */
    }

    /* 12. GET_DESCRIPTOR — full 18-byte device descriptor */
    cc = get_descriptor(slot, USB_DESC_DEVICE, 0, ctrl_buf, 18);
    if (cc != CC_SUCCESS) return;

    usb_dev_desc_t *dd = (usb_dev_desc_t *)ctrl_buf;

    /* 13. Store device record */
    if (usb_dev_cnt < USB_MAX_DEV) {
        usb_device_t *d   = &usb_devs[usb_dev_cnt++];
        d->valid    = 1;
        d->slot     = slot;
        d->port     = (uint8_t)port;
        d->speed    = speed;
        d->vid      = dd->idVendor;
        d->pid      = dd->idProduct;
        d->dev_class = dd->bDeviceClass;
        d->subclass = dd->bDeviceSubClass;
        d->proto    = dd->bDeviceProtocol;
        d->max_pkt0 = dd->bMaxPacketSize0;
        d->bcd_usb  = dd->bcdUSB;

        /* 14. Set configuration 1 (best-effort; ignore errors) */
        set_configuration(slot, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  xhci_init — find controller, reset, and fully initialise
 * ═══════════════════════════════════════════════════════════════════════════ */
int xhci_init(void) {
    /* ── 1. PCI scan for xHCI controller (class=0x0C, sub=0x03, pi=0x30) ── */
    uint8_t  xhci_bus  = 0, xhci_slot = 0, xhci_func = 0;
    int      found     = 0;

    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t dev = 0; dev < 32 && !found; dev++) {
            uint8_t nfunc = pci_is_multifunction((uint8_t)bus, dev) ? 8 : 1;
            for (uint8_t func = 0; func < nfunc && !found; func++) {
                if (pci_get_vendor((uint8_t)bus, dev, func) == 0xFFFFu) continue;
                if (pci_get_class_code((uint8_t)bus, dev, func) != 0x0Cu) continue;
                if (pci_get_subclass  ((uint8_t)bus, dev, func) != 0x03u) continue;
                if (pci_get_progif    ((uint8_t)bus, dev, func) != 0x30u) continue;
                xhci_bus  = (uint8_t)bus;
                xhci_slot = dev;
                xhci_func = func;
                found = 1;
            }
        }
    }
    if (!found) return 0;

    /* ── 2. Enable bus mastering + memory space ─────────────────────────── */
    uint32_t cmd_reg = pci_config_read_dword(xhci_bus, xhci_slot, xhci_func, 0x04u);
    cmd_reg |= 0x06u;   /* bus master + memory space enable */
    pci_config_write_dword(xhci_bus, xhci_slot, xhci_func, 0x04u, cmd_reg);

    /* ── 3. Read BAR0 (supports both 32-bit and 64-bit BARs) ─────────────  */
    uint32_t bar0_lo = pci_config_read_dword(xhci_bus, xhci_slot, xhci_func, 0x10u);
    uint64_t mmio;
    if ((bar0_lo & 0x6u) == 0x4u) {
        /* 64-bit BAR: combine low + high dwords */
        uint32_t bar0_hi = pci_config_read_dword(xhci_bus, xhci_slot, xhci_func, 0x14u);
        mmio = ((uint64_t)bar0_hi << 32) | (bar0_lo & ~0xFu);
    } else {
        mmio = (uint64_t)(bar0_lo & ~0xFu);
    }
    if (!mmio) return 0;

    cap_base = (volatile uint8_t *)(uintptr_t)mmio;

    /* ── 4. Derive operational, doorbell, and runtime base addresses ──────  */
    uint8_t  caplength = CAP_R8(XHCI_CAP_CAPLENGTH);
    uint32_t dboff     = CAP_R32(XHCI_CAP_DBOFF)  & ~0x3u;
    uint32_t rtsoff    = CAP_R32(XHCI_CAP_RTSOFF) & ~0x1Fu;

    op_base = cap_base + caplength;
    db_base = (volatile uint32_t *)(cap_base + dboff);
    rt_base = (volatile uint8_t  *)(cap_base + rtsoff);

    /* ── 5. Check CSZ: must be 0 (32-byte contexts) ─────────────────────── */
    uint32_t hccparams1 = CAP_R32(XHCI_CAP_HCCPARAMS1);
    if (hccparams1 & (1u << 2)) return 0;   /* CSZ=1 not supported */

    /* ── 6. Wait for Controller Not Ready to clear ──────────────────────── */
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (!(OP_R32(XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
        xhci_relax();
    }

    /* ── 7. Halt the controller if running ──────────────────────────────── */
    if (!(OP_R32(XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        OP_W32(XHCI_OP_USBCMD, OP_R32(XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);
        for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
            if (OP_R32(XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
            xhci_relax();
        }
    }

    /* ── 8. Reset ─────────────────────────────────────────────────────────  */
    OP_W32(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (!(OP_R32(XHCI_OP_USBCMD) & XHCI_CMD_HCRST) &&
            !(OP_R32(XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
        xhci_relax();
    }

    /* ── 9. Read topology parameters ────────────────────────────────────── */
    uint32_t hcsparams1 = CAP_R32(XHCI_CAP_HCSPARAMS1);
    uint32_t max_slots  = (hcsparams1 >>  0) & 0xFFu;
    num_ports           = (hcsparams1 >> 24) & 0xFFu;

    if (max_slots > XHCI_MAX_SLOTS) max_slots = XHCI_MAX_SLOTS;

    /* ── 10. Set max slots ───────────────────────────────────────────────── */
    OP_W32(XHCI_OP_CONFIG, max_slots);

    /* ── 11. DCBAA ───────────────────────────────────────────────────────── */
    for (uint32_t i = 0; i <= XHCI_MAX_SLOTS; i++) dcbaa[i] = 0;
    xhci_mb();
    OP_W64(XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)dcbaa);

    /* ── 12. Command Ring ────────────────────────────────────────────────── */
    ring_init(cmd_ring, XHCI_RING_SIZE);
    cmd_enq = 0; cmd_cs = 1;
    xhci_mb();
    /* CRCR: base address | RCS=1 (initial ring cycle state = 1) */
    OP_W64(XHCI_OP_CRCR, (uint64_t)(uintptr_t)cmd_ring | XHCI_CRCR_RCS);

    /* ── 13. Primary Event Ring ──────────────────────────────────────────── */
    ring_init(evt_ring, XHCI_RING_SIZE);
    evt_deq = 0; evt_cs = 1;

    erst[0].base = (uint64_t)(uintptr_t)evt_ring;
    erst[0].size = XHCI_RING_SIZE;
    xhci_mb();

    /* Interrupter 0: disable interrupt line (polling mode) */
    IR0_W32(XHCI_IR_IMAN,   0u);
    IR0_W32(XHCI_IR_ERSTSZ, 1u);
    /* ERDP: initial dequeue pointer = &evt_ring[0] */
    IR0_W64(XHCI_IR_ERDP, (uint64_t)(uintptr_t)evt_ring);
    /* ERSTBA: event ring segment table base address */
    IR0_W64(XHCI_IR_ERSTBA, (uint64_t)(uintptr_t)erst);

    /* ── 14. Start the controller ────────────────────────────────────────── */
    OP_W32(XHCI_OP_USBCMD, XHCI_CMD_RUN);
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (!(OP_R32(XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        xhci_relax();
    }
    if (OP_R32(XHCI_OP_USBSTS) & XHCI_STS_HCH) return 0;   /* still halted */

    /* ── 15. Power on all ports ──────────────────────────────────────────── */
    for (uint32_t p = 0; p < num_ports; p++) {
        uint32_t ps = OP_R32(XHCI_OP_PORTSC(p));
        if (!(ps & XHCI_PS_PP))
            OP_W32(XHCI_OP_PORTSC(p), ps | XHCI_PS_PP);
    }

    /* Short settle delay: some controllers need ~20 ms after power-on */
    uint64_t t_end = get_tick() + 50u;
    while (get_tick() < t_end) xhci_relax();

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  xhci_scan_devices — enumerate all root-hub ports
 * ═══════════════════════════════════════════════════════════════════════════ */
void xhci_scan_devices(void) {
    usb_dev_cnt = 0;
    for (uint32_t p = 0; p < num_ports; p++)
        probe_port(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */
int           usb_device_count(void)    { return usb_dev_cnt; }
usb_device_t *usb_get_device(int idx)  {
    if (idx < 0 || idx >= usb_dev_cnt) return (usb_device_t *)0;
    return &usb_devs[idx];
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Primitives exported for usbhid.c
 * ═══════════════════════════════════════════════════════════════════════════ */

void xhci_ring_init(xhci_trb_t *ring, uint32_t sz) {
    ring_init(ring, sz);
}

int xhci_ring_enqueue(xhci_trb_t *ring, uint32_t *enq, uint8_t *cs,
                       uint64_t param, uint32_t status, uint32_t ctrl) {
    return ring_enqueue(ring, enq, cs, param, status, ctrl);
}

void xhci_ep_doorbell(uint8_t slot, uint8_t epid) {
    ep_doorbell(slot, epid);
}

/* Non-blocking single-shot event ring check */
int xhci_evt_try(xhci_trb_t *out) {
    xhci_trb_t *trb = &evt_ring[evt_deq];
    uint32_t ctrl = xhci_r32((volatile uint32_t *)&trb->ctrl);
    if ((ctrl & TRB_C) != (uint32_t)evt_cs) return 0;

    out->param  = xhci_r64((volatile uint64_t *)&trb->param);
    out->status = xhci_r32((volatile uint32_t *)&trb->status);
    out->ctrl   = ctrl;

    evt_deq++;
    if (evt_deq == XHCI_RING_SIZE) { evt_deq = 0; evt_cs ^= 1; }
    IR0_W64(XHCI_IR_ERDP,
            (uint64_t)(uintptr_t)&evt_ring[evt_deq] | (1u << 3));
    return 1;
}

uint8_t xhci_ctrl_xfer(uint8_t slot, const usb_setup_t *setup,
                        void *buf, uint16_t len) {
    return control_xfer(slot, setup, buf, len);
}

xhci_input_ctx_t *xhci_get_input_ctx(void) { return &input_ctx; }

uint8_t xhci_cfg_ep(uint8_t slot, xhci_input_ctx_t *ictx) {
    cmd_enqueue((uint64_t)(uintptr_t)ictx, 0,
                TRB_TYPE(TRBTYPE_CFG_EP) | TRB_SLOT(slot));
    cmd_doorbell();
    return wait_event(TRBTYPE_EVT_CMD);
}
