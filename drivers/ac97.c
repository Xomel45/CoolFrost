#include "ac97.h"
#include "pci.h"
#include "../cpu/ports.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

/* ── NAM (Native Audio Mixer) register offsets from BAR0 ─────────────────── */
#define NAM_RESET       0x00u
#define NAM_MASTER_VOL  0x02u   /* master output volume (0=max, mute=bit15) */
#define NAM_PCM_VOL     0x18u   /* PCM out volume                           */
#define NAM_EXT_AUDIO   0x28u   /* extended audio status/control (VRA bit)  */
#define NAM_FRONT_RATE  0x2Cu   /* PCM front DAC sample rate (write Hz)     */

/* ── NABM (Native Audio Bus Master) offsets from BAR1 ────────────────────── */
/* PCM-out channel is at base 0x10 */
#define NABM_PO_BDBAR   0x10u   /* Buffer Descriptor Base Address (32-bit) */
#define NABM_PO_CIV     0x14u   /* Current Index Value (8-bit)             */
#define NABM_PO_LVI     0x15u   /* Last Valid Index   (8-bit)              */
#define NABM_PO_SR      0x16u   /* Status Register    (16-bit)             */
#define NABM_PO_PICB    0x18u   /* Position In Current Buffer (16-bit)     */
#define NABM_PO_CR      0x1Bu   /* Control Register   (8-bit)              */
#define NABM_GLOB_CNT   0x2Cu   /* Global Control     (32-bit)             */
#define NABM_GLOB_STA   0x30u   /* Global Status      (32-bit)             */

/* CR bits */
#define CR_RPBM   0x01u   /* Run/Pause Bus Master (1=run) */
#define CR_RR     0x02u   /* Reset Registers              */

/* SR bits */
#define SR_DCH    0x01u   /* DMA Controller Halted        */
#define SR_CELV   0x02u   /* Current Equals Last Valid    */
#define SR_LVBCI  0x04u   /* Last Valid Buffer Completed  */
#define SR_BCIS   0x08u   /* Buffer Completion Interrupt  */

/* GLOB_CNT bits */
#define GC_CRST   (1u << 1)   /* Cold Reset: 0=reset, 1=normal */
#define GC_WRST   (1u << 2)   /* Warm Reset                    */

/* ── Buffer Descriptor List entry (8 bytes) ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t addr;   /* physical address of PCM buffer (32-bit)    */
    uint16_t len;    /* number of 16-bit samples (L and R counted) */
    uint16_t ctrl;   /* bit 15=IOC, bit 14=BUP                     */
} ac97_bde_t;

/* 32 BDE entries (AC97 spec max), aligned to 8 bytes */
static ac97_bde_t ac97_bdl[32] __attribute__((aligned(8)));

/* PCM output buffer: up to 1 second of 48000Hz stereo =
 * 48000 frames × 2 channels × 2 bytes = 192000 bytes             */
#define AC97_MAX_FRAMES  48000u
static int16_t ac97_pcm[AC97_MAX_FRAMES * 2u] __attribute__((aligned(4096)));

static uint16_t ac97_nam  = 0;   /* BAR0: NAM I/O base  */
static uint16_t ac97_nabm = 0;   /* BAR1: NABM I/O base */
static int      ac97_ready = 0;

/* ── I/O helpers ──────────────────────────────────────────────────────────── */
static uint16_t nam_rd(uint16_t r)          { return port_word_in((uint16_t)(ac97_nam + r)); }
static void     nam_wr(uint16_t r, uint16_t v) { port_word_out((uint16_t)(ac97_nam + r), v); }
static uint8_t  nabm_rb(uint16_t r)         { return port_byte_in((uint16_t)(ac97_nabm + r)); }
static void     nabm_wb(uint16_t r, uint8_t v) { port_byte_out((uint16_t)(ac97_nabm + r), v); }
static uint16_t nabm_rw(uint16_t r)         { return port_word_in((uint16_t)(ac97_nabm + r)); }
static void     nabm_ww(uint16_t r, uint16_t v){ port_word_out((uint16_t)(ac97_nabm + r), v); }
static void     nabm_wl(uint16_t r, uint32_t v){ port_dword_out((uint16_t)(ac97_nabm + r), v); }

/* ── Public: ac97_init ────────────────────────────────────────────────────── */
int ac97_init(void) {
    /* Known Intel AC97 device IDs */
    static const uint16_t known[] = { 0x2415, 0x2425, 0x2445, 0x2485, 0 };

    for (uint16_t bus = 0; bus < 256u; bus++) {
        for (uint8_t slot = 0; slot < 32u; slot++) {
          uint8_t nfunc = pci_is_multifunction((uint8_t)bus, slot) ? 8 : 1;
          for (uint8_t func = 0; func < nfunc; func++) {
            if (pci_get_vendor(bus, slot, func) != 0x8086u) continue;
            uint16_t dev = (uint16_t)(pci_config_read_dword(bus, slot, func, 0x00u) >> 16);
            int match = 0;
            for (int k = 0; known[k]; k++) if (known[k] == dev) { match = 1; break; }
            if (!match) continue;

            /* Enable I/O space + bus master */
            uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04u);
            cmd |= 0x05u;
            pci_config_write_dword(bus, slot, func, 0x04u, cmd);

            /* Read BARs (I/O space) */
            ac97_nam  = (uint16_t)(pci_config_read_dword(bus, slot, func, 0x10u) & 0xFFFEu);
            ac97_nabm = (uint16_t)(pci_config_read_dword(bus, slot, func, 0x14u) & 0xFFFEu);

            /* Global cold reset: first clear CR field, then set CRST */
            nabm_wl(NABM_GLOB_CNT, 0u);
            /* Spin until reset clears */
            for (int i = 0; i < 100000; i++) {
                if (!(port_dword_in((uint16_t)(ac97_nabm + NABM_GLOB_CNT)) & GC_CRST))
                    break;
            }
            nabm_wl(NABM_GLOB_CNT, GC_CRST);   /* release reset */

            /* Wait for codec ready: NAM register accessible when not 0xFFFF */
            for (int i = 0; i < 100000; i++) {
                if (nam_rd(NAM_RESET) != 0xFFFFu) break;
            }
            /* Codec cold reset via NAM */
            nam_wr(NAM_RESET, 0u);

            /* Set volumes (0x0000 = max, no mute) */
            nam_wr(NAM_MASTER_VOL, 0x0000u);
            nam_wr(NAM_PCM_VOL,    0x0000u);

            /* Enable VRA (Variable Rate Audio) if supported, then set 48000 Hz */
            uint16_t ea = nam_rd(0x26u);   /* Extended Audio ID */
            if (ea & 0x0001u) {
                nam_wr(NAM_EXT_AUDIO, nam_rd(NAM_EXT_AUDIO) | 0x0001u);
            }
            nam_wr(NAM_FRONT_RATE, 48000u);

            /* Reset PCM-out channel */
            nabm_wb(NABM_PO_CR, CR_RR);
            for (int i = 0; i < 100000; i++) {
                if (!(nabm_rb(NABM_PO_CR) & CR_RR)) break;
            }

            ac97_ready = 1;
            printf("ac97: found at NAM=0x%x NABM=0x%x, 48000Hz 16-bit stereo\n",
                   ac97_nam, ac97_nabm);
            return 0;
          }
        }
    }
    return -1;
}

/* ── Public: ac97_play_pcm ────────────────────────────────────────────────── */
void ac97_play_pcm(const int16_t *frames, uint32_t n_frames) {
    if (!ac97_ready || !n_frames) return;
    if (n_frames > AC97_MAX_FRAMES) n_frames = AC97_MAX_FRAMES;

    uint32_t n_samples = n_frames * 2u;   /* L + R per frame */
    memcpy((uint8_t *)ac97_pcm, (uint8_t *)frames, n_samples * 2u);

    /* Build BDL: split into ≤65535-sample chunks */
    uint32_t remaining = n_samples;
    uint8_t  nentries  = 0u;
    uint32_t offset    = 0u;

    memset(ac97_bdl, 0, sizeof(ac97_bdl));
    while (remaining > 0u && nentries < 32u) {
        uint16_t chunk = (remaining > 65535u) ? 65535u : (uint16_t)remaining;
        ac97_bdl[nentries].addr = (uint32_t)((uintptr_t)ac97_pcm + offset * 2u);
        ac97_bdl[nentries].len  = chunk;
        ac97_bdl[nentries].ctrl = 0u;   /* no IOC, no BUP */
        offset    += chunk;
        remaining -= chunk;
        nentries++;
    }
    /* Mark last entry */
    ac97_bdl[nentries - 1u].ctrl = (1u << 15);   /* IOC */

    /* Stop any running DMA, clear status */
    nabm_wb(NABM_PO_CR, 0u);
    nabm_ww(NABM_PO_SR, 0x001Cu);   /* clear interrupt bits */

    /* Set BDL base address and LVI */
    nabm_wl(NABM_PO_BDBAR, (uint32_t)(uintptr_t)ac97_bdl);
    nabm_wb(NABM_PO_LVI,   nentries - 1u);

    /* Start DMA */
    nabm_wb(NABM_PO_CR, CR_RPBM);

    /* Poll until DMA halted (DCH=1) or CELV */
    for (uint32_t i = 0u; i < 200000000u; i++) {
        uint16_t sr = nabm_rw(NABM_PO_SR);
        if (sr & (SR_DCH | SR_CELV | SR_LVBCI)) break;
    }

    nabm_wb(NABM_PO_CR, 0u);   /* stop */
}

/* ── Public: ac97_beep ────────────────────────────────────────────────────── */
void ac97_beep(uint32_t freq, uint32_t ms) {
    if (!ac97_ready) return;

    uint32_t n_frames = ms * 48000u / 1000u;
    if (n_frames > AC97_MAX_FRAMES) n_frames = AC97_MAX_FRAMES;
    if (!freq) freq = 440u;

    uint32_t half_period = 48000u / freq / 2u;
    if (!half_period) half_period = 1u;

    static int16_t beep_buf[AC97_MAX_FRAMES * 2u];
    int16_t val = 16000;
    for (uint32_t i = 0u; i < n_frames; i++) {
        if ((i / half_period) & 1u) val = -16000;
        else                        val =  16000;
        beep_buf[i * 2u]     = val;   /* left  */
        beep_buf[i * 2u + 1u] = val;  /* right */
    }
    ac97_play_pcm(beep_buf, n_frames);
}
