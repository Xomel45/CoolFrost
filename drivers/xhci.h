#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include "usb.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  xHCI register offsets and bit definitions
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Capability register offsets (from cap_base) ───────────────────────── */
#define XHCI_CAP_CAPLENGTH   0x00u   /* 1 byte */
#define XHCI_CAP_HCIVERSION  0x02u   /* 2 bytes */
#define XHCI_CAP_HCSPARAMS1  0x04u
#define XHCI_CAP_HCSPARAMS2  0x08u
#define XHCI_CAP_HCSPARAMS3  0x0Cu
#define XHCI_CAP_HCCPARAMS1  0x10u
#define XHCI_CAP_DBOFF       0x14u   /* doorbell array offset */
#define XHCI_CAP_RTSOFF      0x18u   /* runtime register offset */
#define XHCI_CAP_HCCPARAMS2  0x1Cu

/* ── Operational register offsets (from op_base = cap_base + CAPLENGTH) ── */
#define XHCI_OP_USBCMD   0x00u
#define XHCI_OP_USBSTS   0x04u
#define XHCI_OP_PAGESIZE 0x08u
#define XHCI_OP_DNCTRL   0x14u
#define XHCI_OP_CRCR     0x18u   /* 64-bit Command Ring Control */
#define XHCI_OP_DCBAAP   0x30u   /* 64-bit Device Context BAA Pointer */
#define XHCI_OP_CONFIG   0x38u

/* Port Status and Control for port n (0-based) */
#define XHCI_OP_PORTSC(n)  (0x400u + 0x10u * (uint32_t)(n))

/* ── USBCMD bits ────────────────────────────────────────────────────────── */
#define XHCI_CMD_RUN    (1u <<  0)
#define XHCI_CMD_HCRST  (1u <<  1)
#define XHCI_CMD_INTE   (1u <<  2)
#define XHCI_CMD_HSEE   (1u <<  3)

/* ── USBSTS bits ────────────────────────────────────────────────────────── */
#define XHCI_STS_HCH    (1u <<  0)   /* halted */
#define XHCI_STS_HSE    (1u <<  2)
#define XHCI_STS_EINT   (1u <<  3)
#define XHCI_STS_PCD    (1u <<  4)
#define XHCI_STS_CNR    (1u << 11)   /* controller not ready */

/* ── PORTSC bits ────────────────────────────────────────────────────────── */
#define XHCI_PS_CCS     (1u <<  0)   /* current connect status */
#define XHCI_PS_PED     (1u <<  1)   /* port enabled           */
#define XHCI_PS_PR      (1u <<  4)   /* port reset             */
#define XHCI_PS_PLS(v)  (((v) & 0xFu) << 5)
#define XHCI_PS_PP      (1u <<  9)   /* port power             */
#define XHCI_PS_SPEED(r)(((r) >> 10) & 0xFu)
#define XHCI_PS_CSC     (1u << 17)   /* connect status change  */
#define XHCI_PS_PEC     (1u << 18)   /* enable/disable change  */
#define XHCI_PS_WRC     (1u << 19)
#define XHCI_PS_OCC     (1u << 20)
#define XHCI_PS_PRC     (1u << 21)   /* port reset change      */
#define XHCI_PS_PLC     (1u << 22)
#define XHCI_PS_CEC     (1u << 23)
/* Mask to clear all W1C change bits without disturbing PP/PED */
#define XHCI_PS_W1C_MASK (XHCI_PS_CSC|XHCI_PS_PEC|XHCI_PS_WRC| \
                          XHCI_PS_OCC|XHCI_PS_PRC|XHCI_PS_PLC|XHCI_PS_CEC)

/* ── Runtime register offsets (from rt_base = cap_base + RTSOFF) ───────── */
#define XHCI_RT_MFINDEX  0x00u
#define XHCI_RT_IR0      0x20u   /* Interrupter 0 base */

/* Interrupter register offsets (relative to IR base) */
#define XHCI_IR_IMAN     0x00u
#define XHCI_IR_IMOD     0x04u
#define XHCI_IR_ERSTSZ   0x08u
#define XHCI_IR_ERSTBA   0x10u   /* 64-bit */
#define XHCI_IR_ERDP     0x18u   /* 64-bit */

/* ── CRCR bits ──────────────────────────────────────────────────────────── */
#define XHCI_CRCR_RCS   (1u << 0)   /* Ring Cycle State */
#define XHCI_CRCR_CS    (1u << 1)   /* Command Stop */
#define XHCI_CRCR_CA    (1u << 2)   /* Command Abort */
#define XHCI_CRCR_CRR   (1u << 3)   /* Command Ring Running */

/* ── Doorbell register ──────────────────────────────────────────────────── */
/* DB[0] = command ring, DB[slot] = transfer ring for that slot */
#define XHCI_DB_HOST_CMD  0u         /* value for command ring */
#define XHCI_DB_EP0       1u         /* endpoint ID 1 = EP0   */

/* ═══════════════════════════════════════════════════════════════════════════
 *  TRB (Transfer Request Block) — 16 bytes
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct __attribute__((aligned(16))) {
    uint64_t param;   /* offset  0 */
    uint32_t status;  /* offset  8 */
    uint32_t ctrl;    /* offset 12 */
} xhci_trb_t;

/* TRB ctrl bit helpers */
#define TRB_C            (1u <<  0)   /* cycle bit */
#define TRB_TC           (1u <<  1)   /* toggle cycle (Link TRB) */
#define TRB_ENT          (1u <<  1)   /* evaluate next TRB */
#define TRB_ISP          (1u <<  2)   /* interrupt on short packet */
#define TRB_CH           (1u <<  4)   /* chain */
#define TRB_IOC          (1u <<  5)   /* interrupt on completion */
#define TRB_IDT          (1u <<  6)   /* immediate data (Setup Stage) */
#define TRB_BSR          (1u <<  9)   /* block set address request */
#define TRB_DIR          (1u << 16)   /* data direction: 1=IN (Data/Status) */
#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_TRT(t)       ((uint32_t)(t) << 16)  /* Transfer Type (Setup) */
#define TRB_SLOT(s)      ((uint32_t)(s) << 24)
#define TRB_EP(e)        ((uint32_t)(e) << 16)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3Fu)
#define TRB_GET_SLOT(c)  (((c) >> 24) & 0xFFu)
#define TRB_GET_CC(s)    (((s) >> 24) & 0xFFu)

/* TRB types */
#define TRBTYPE_NORMAL      1u
#define TRBTYPE_SETUP       2u
#define TRBTYPE_DATA        3u
#define TRBTYPE_STATUS      4u
#define TRBTYPE_LINK        6u
#define TRBTYPE_EN_SLOT     9u
#define TRBTYPE_DIS_SLOT   10u
#define TRBTYPE_ADDR_DEV   11u
#define TRBTYPE_CFG_EP     12u
#define TRBTYPE_EVAL_CTX   13u
#define TRBTYPE_RST_EP     14u
#define TRBTYPE_NOOP_CMD   23u
#define TRBTYPE_EVT_XFER   32u
#define TRBTYPE_EVT_CMD    33u
#define TRBTYPE_EVT_PORT   34u

/* TRT (Transfer Type) for Setup Stage TRB */
#define TRT_NO_DATA  0u
#define TRT_OUT_DATA 2u
#define TRT_IN_DATA  3u

/* Completion codes */
#define CC_INVALID          0u
#define CC_SUCCESS          1u
#define CC_DATA_BUF         2u
#define CC_BABBLE           3u
#define CC_USB_XACT         4u
#define CC_TRB_ERR          5u
#define CC_STALL            6u
#define CC_RESOURCE         7u
#define CC_BANDWIDTH        8u
#define CC_NO_SLOTS         9u
#define CC_SHORT_PKT       13u
#define CC_RING_UNDERRUN   14u
#define CC_RING_OVERRUN    15u
#define CC_VF_RING_FULL    16u
#define CC_PARAMETER_ERR   17u
#define CC_STOPPED         26u
#define CC_STOPPED_LEN_INV 27u

/* ── Event Ring Segment Table entry (16 bytes, naturally laid out) ──────── */
typedef struct __attribute__((aligned(64))) {
    uint64_t base;
    uint16_t size;
    uint16_t _res[3];
} xhci_erst_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Context structures (32-byte variant, CSZ=0)
 *  No 'packed' needed — natural layout matches the required byte sizes.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define XHCI_CTX_SIZE   32u    /* per context entry, CSZ=0 */

typedef struct {
    uint32_t dw[8];
} xhci_ctx_t;   /* 32 bytes, naturally 4-byte aligned */

/* Device Context: Slot ctx + 31 EP contexts.
 * No alignment on the typedef — variable declarations add aligned(64).  */
typedef struct {
    xhci_ctx_t slot;
    xhci_ctx_t ep[31];   /* ep[0]=EP0, ep[1]=EP1-Out, ep[2]=EP1-In, … */
} xhci_dev_ctx_t;

/* Input Context: Input Control ctx (32 B) followed by Device Context.
 * The whole struct is 64-byte aligned at declaration; icc occupies bytes
 * 0-31, dev starts at byte 32 as the xHCI spec requires.               */
typedef struct __attribute__((aligned(64))) {
    xhci_ctx_t   icc;
    xhci_dev_ctx_t dev;
} xhci_input_ctx_t;

/* Slot Context dw0 helpers */
#define SLOT_CTX_SPEED(s)   ((uint32_t)(s) << 20)
#define SLOT_CTX_CTX_ENT(n) ((uint32_t)(n) << 27)
#define SLOT_CTX_ROOTPORT(p)((uint32_t)(p) << 16)  /* dw1 */

/* EP Context dw0 helpers */
#define EP_CTX_EP_STATE(s)  ((s) & 0x7u)
#define EP_CTX_INTERVAL(i)  ((uint32_t)(i) << 16)
/* EP Context dw1 */
#define EP_CTX_CERR(n)      ((uint32_t)(n) << 1)
#define EP_CTX_EP_TYPE(t)   ((uint32_t)(t) << 3)
#define EP_CTX_MAX_BURST(b) ((uint32_t)(b) << 8)
#define EP_CTX_MAX_PKT(p)   ((uint32_t)(p) << 16)
/* EP types */
#define EP_TYPE_ISOC_OUT    1u
#define EP_TYPE_BULK_OUT    2u
#define EP_TYPE_INT_OUT     3u
#define EP_TYPE_CONTROL     4u
#define EP_TYPE_ISOC_IN     5u
#define EP_TYPE_BULK_IN     6u
#define EP_TYPE_INT_IN      7u
/* Input Control Context dw1 bits */
#define ICC_ADD_SLOT        (1u << 0)
#define ICC_ADD_EP0         (1u << 1)

/* ═══════════════════════════════════════════════════════════════════════════
 *  MMIO accessors — inline ASM for correct MMIO ordering
 *  x86 store ordering (TSO) is strong, but mfence after writes ensures
 *  the doorbell ring is never reordered with preceding ring enqueues.
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t xhci_r8(volatile uint8_t *a) {
    uint8_t v;
    __asm__ volatile("movb (%[a]), %[v]" : [v]"=r"(v) : [a]"r"(a) : "memory");
    return v;
}

static inline uint32_t xhci_r32(volatile uint32_t *a) {
    uint32_t v;
    __asm__ volatile("movl (%[a]), %[v]" : [v]"=r"(v) : [a]"r"(a) : "memory");
    return v;
}

static inline void xhci_w32(volatile uint32_t *a, uint32_t v) {
    __asm__ volatile("movl %[v], (%[a])\n\t"
                     "mfence"
                     :: [v]"r"(v), [a]"r"(a) : "memory");
}

static inline uint64_t xhci_r64(volatile uint64_t *a) {
    uint64_t v;
    __asm__ volatile("movq (%[a]), %[v]" : [v]"=r"(v) : [a]"r"(a) : "memory");
    return v;
}

static inline void xhci_w64(volatile uint64_t *a, uint64_t v) {
    __asm__ volatile("movq %[v], (%[a])\n\t"
                     "mfence"
                     :: [v]"r"(v), [a]"r"(a) : "memory");
}

/* Full memory barrier — before handing a DMA buffer to the controller */
static inline void xhci_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* Store barrier — after filling a TRB ring before ringing doorbell */
static inline void xhci_wmb(void) {
    __asm__ volatile("sfence" ::: "memory");
}

/* Spinloop yield — reduces memory bus contention while polling */
static inline void xhci_relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

/* ── Primary entry points ────────────────────────────────────────────────── */
int  xhci_init(void);
void xhci_scan_devices(void);

/* ── Low-level primitives used by usbhid.c ──────────────────────────────── */

/* Initialise a TRB ring of `sz` entries (last = Link TRB pointing back). */
void    xhci_ring_init(xhci_trb_t *ring, uint32_t sz);

/* Enqueue one TRB into a ring managed by the caller's enq/cs state.
 * Returns 0 on success, -1 if the ring would overflow.              */
int     xhci_ring_enqueue(xhci_trb_t *ring, uint32_t *enq, uint8_t *cs,
                           uint64_t param, uint32_t status, uint32_t ctrl);

/* Ring the doorbell for slot `slot` / endpoint-id `epid`.
 * epid = ep_num*2 + 1 for IN endpoints (1 = EP0).                  */
void    xhci_ep_doorbell(uint8_t slot, uint8_t epid);

/* Non-blocking single-shot event ring check.
 * Returns 1 and fills *out if an event TRB was ready; 0 if ring empty. */
int     xhci_evt_try(xhci_trb_t *out);

/* Control transfer on EP0 of `slot`.  Same as the internal helper.  */
uint8_t xhci_ctrl_xfer(uint8_t slot, const usb_setup_t *setup,
                        void *buf, uint16_t len);

/* CONFIGURE_ENDPOINT command — adds/modifies endpoint contexts.
 * The caller populates `ictx` before calling.                       */
uint8_t xhci_cfg_ep(uint8_t slot, xhci_input_ctx_t *ictx);

/* Return pointer to the shared input context scratch buffer.        */
xhci_input_ctx_t *xhci_get_input_ctx(void);

#endif
