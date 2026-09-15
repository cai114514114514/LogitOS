#include "../core/io_lock.h"
/* Command rings and stream register RMW belong to this controller. The ISR
 * takes this short gate but never an audio worker/lifecycle owner. */
static io_lock_t hda_gate = IO_LOCK_INIT;
/* Intel High Definition Audio controller + codec.
 *
 * HDA is the right target because it is what real machines have: every PC
 * built since about 2004 has an HDA controller, the register map is a published
 * spec rather than a per-vendor secret, and QEMU emulates it faithfully enough
 * that the same code drives both. AC'97 would have been a third of the work and
 * is a dead end -- see ac97.c, which exists as the fallback for machines that
 * really only have that.
 *
 * WHAT AN HDA CONTROLLER ACTUALLY IS. Two completely separate things share one
 * PCI function, and conflating them is the usual way this driver goes wrong:
 *
 *   1. A COMMAND BUS to the codecs. Codecs are little processors on a serial
 *      link; you talk to them with 32-bit "verbs" through a pair of DMA rings
 *      (CORB out, RIRB back). Setting the volume, choosing which pin the sound
 *      leaves by, and asking what the hardware even IS all happen here.
 *   2. A DMA ENGINE ("stream descriptor") that walks a scatter list (the BDL)
 *      through a cyclic buffer and hands the bytes to a codec's converter.
 *      This is the part that plays.
 *
 * Nothing works until BOTH are set up AND connected: the DAC must be told which
 * stream number to listen to, or the DMA engine runs happily, LPIB advances,
 * every register reads back correct -- and there is silence, because the bytes
 * are going nowhere. That failure looks exactly like success from the
 * controller's side, which is why this driver reports the codec graph it found
 * rather than just "ok".
 *
 * DMA now uses coherent allocations: CPU pointers always name the physmap,
 * device addresses come from the DMA handle, with the GCAP 64OK capability
 * selecting the mask. Historical description (superseded):
 * DMA: the same pattern virtio.c uses -- pmm_alloc()/pmm_alloc_contig() return
 * identity-mapped low physical pages, so the address the CPU writes is the
 * address the device reads, with no IOMMU and no translation. The BDL must be
 * 128-byte aligned and the ring 128-byte aligned; a page from the PMM is 4 KiB
 * aligned, which satisfies both.
 *
 * Interrupts: dev_irq_request() picks MSI-X, then MSI, then INTx. The handler
 * clears the stream's status bits and calls snd_period_elapsed() -- a counter
 * and a semaphore post. It does no mixing: driver.h forbids blocking and
 * floating point in an ISR, and mixing wants both.
 */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "pci.h"
#include "snd.h"
#include "dma.h"
#include "kprintf.h"
#include "pit.h"
#ifdef HDA_DMA_CAPTURE_CANARY
#include "sched.h"
#include "kernel/sync/wait.h"
void hda_capture_stop_check(void);
#endif

/* The monotonic ns clock is what this driver WANTS for its reset and codec
 * timeouts -- a duration expressed in spins is a different duration on every
 * host, which is exactly how a driver that works on one machine times out on
 * another. But c/kernel/core/ktime.h belongs to another line and may not have
 * landed in the tree being built, and a committed file must build from a clean
 * clone of itself rather than from whatever happens to be in a shared working
 * tree. So: use it when it is there, fall back to the 100 Hz tick when it is
 * not, and never depend on either being precise -- every wait below is ALSO
 * bounded by an iteration count, so a frozen or missing clock degrades to a
 * failed probe instead of a hung boot. */
#if defined(__has_include)
#  if __has_include("ktime.h")
#    define HDA_HAVE_KTIME 1
#  endif
#endif
#ifdef HDA_HAVE_KTIME
#  include "ktime.h"
#endif

void *memset(void *, int, size_t);

/* ---------------------------------------------------------- registers --- */
#define GCAP        0x00    /* 16: capabilities -- how many streams, of which kind */
#define VMIN        0x02
#define VMAJ        0x03
#define GCTL        0x08    /* 32: bit0 CRST (0 = in reset) */
#define WAKEEN      0x0C
#define STATESTS    0x0E    /* 16: one bit per codec address that responded */
#define INTCTL      0x20    /* 32: bit31 GIE, bit30 CIE, bits 0..29 per stream */
#define INTSTS      0x24
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48    /* 16 */
#define CORBRP      0x4A    /* 16: bit15 = reset */
#define CORBCTL     0x4C    /* 8:  bit1 RUN */
#define CORBSTS     0x4D
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58    /* 16: bit15 = reset */
#define RINTCNT     0x5A    /* 16 */
#define RIRBCTL     0x5C    /* 8:  bit1 RUN, bit0 int-on-response */
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define DPLBASE     0x70
#define DPUBASE     0x74

/* Stream descriptors start here, 0x20 bytes each, input streams first. */
#define SD_BASE     0x80
#define SD_CTL      0x00    /* 24-bit: b0 SRST, b1 RUN, b2 IOCE, b3 FEIE, b4 DEIE,
                             * b20..23 stream number */
#define SD_STS      0x03    /* 8: b2 BCIS, b3 FIFOE, b4 DESE */
#define SD_LPIB     0x04
#define SD_CBL      0x08    /* cyclic buffer length, bytes */
#define SD_LVI      0x0C    /* 16: last valid BDL index */
#define SD_FIFOS    0x10
#define SD_FMT      0x12    /* 16 */
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

/* Codec verbs (12-bit) and parameters. */
#define VERB_GET_PARAM        0xF00
#define VERB_GET_CONN_LIST    0xF02
#define VERB_SET_STREAM_FMT   0x200
#define VERB_SET_AMP          0x300
#define VERB_SET_STREAM_CHAN  0x706
#define VERB_SET_PIN_CTL      0x707
#define VERB_GET_PIN_CTL      0xF07
#define VERB_SET_EAPD         0x70C
#define VERB_GET_CONFIG_DEF   0xF1C
#define VERB_SET_POWER        0x705
#define VERB_SET_CONN_SELECT  0x701    /* choose which connection-list entry a
                                        * multi-source widget (an ADC, a mixer,
                                        * an input selector) actually listens
                                        * to right now */
#define VERB_GET_PIN_SENSE    0xF09    /* bit31 = a jack is physically present */

#define PARAM_VENDOR          0x00
#define PARAM_SUBNODE_COUNT   0x04
#define PARAM_FG_TYPE         0x05
#define PARAM_AUDIO_WIDGET_CAP 0x09
#define PARAM_PIN_CAP         0x0C
#define PARAM_CONN_LIST_LEN   0x0E

#define WIDGET_DAC   0x0
#define WIDGET_ADC   0x1
#define WIDGET_MIXER 0x2
#define WIDGET_SEL   0x3
#define WIDGET_PIN   0x4

/* --------------------------------------------------------- our geometry -- */
/* 1024 frames at 48 kHz is 21.3 ms per period. Chosen against TCG rather than
 * against a datasheet: a shorter period is lower latency and QEMU's emulated
 * guest is slow enough that a 5 ms deadline is a coin flip, which would make
 * the underrun path fire constantly and hide real faults. Eight of them is
 * 170 ms of buffer -- generous, and the right trade while the scheduler has no
 * priorities to give kaudio. */
#define HDA_RATE      48000
#define HDA_CHANNELS  2
#define HDA_PERIOD_FRAMES 1024
#define HDA_PERIOD_BYTES  (HDA_PERIOD_FRAMES * HDA_CHANNELS * 2)
#define HDA_PERIODS   8
#define HDA_RING_BYTES (HDA_PERIOD_BYTES * HDA_PERIODS)
#define HDA_STREAM_TAG 1
/* A separate tag, not because the two directions could collide on the wire --
 * input and output streams are independent DMA rings and independent
 * converter buses -- but because a stream tag is also how a codec verb
 * (SET_STREAM_CHAN) says "listen to THIS one", and using the same number for
 * both would make a trace of the verb log ambiguous about which engine a
 * given SET_STREAM_CHAN was routing. Same geometry as playback (1024-frame
 * periods x8): a period boundary is a period boundary regardless of which
 * way the bytes are moving, and there is no QEMU-vs-TCG reason for capture to
 * need a different size than the one already tuned for it. */
#define HDA_CAPTURE_STREAM_TAG 2

struct bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;      /* bit0 = interrupt on completion */
};

struct hda {
    volatile uint8_t *mmio;
    struct device    *dev;
    int removing;
    struct dma_device dma;
    struct dma_buffer *corb_dma, *rirb_dma;
    struct dma_buffer *ring_dma, *bdl_dma, *cap_ring_dma, *cap_bdl_dma;

    uint32_t         *corb;       /* one page; active entry count is negotiated */
    uint64_t         *rirb;       /* one page; active entry count is negotiated */
    unsigned          corb_entries;
    unsigned          rirb_entries;
    unsigned          corb_wp;
    unsigned          rirb_rp;

    unsigned          out_base;   /* register offset of the first output SD */
    unsigned          codec_addr;
    unsigned          dac_nid;
    unsigned          pin_nid;

    uint8_t          *ring;       /* the PCM the DMA engine plays */
    struct bdl_entry *bdl;

    struct snd_device snd;
    uint64_t          irqs;

    /* -------------------------------------------------------- capture -- */
    unsigned          in_base;    /* register offset of the first input SD;
                                    * input descriptors are always first, so
                                    * this is SD_BASE whenever iss > 0 -- kept
                                    * as a field rather than a bare constant
                                    * because hda_isr and hda_cap_start read it
                                    * without a second copy of that fact. */
    unsigned          adc_nid;    /* 0 = no capture path found/usable */
    unsigned          cap_pin_nid;
    unsigned          cap_sel_nid;    /* which widget's connection-select to
                                        * program, 0 = none needed (the source
                                        * is this ADC/selector's only input) */
    unsigned          cap_sel_index;  /* the index to select on cap_sel_nid */

    uint8_t          *cap_ring;   /* the PCM the DMA engine WRITES into */
    struct bdl_entry *cap_bdl;

    struct snd_capdevice cap;
    uint64_t             cap_irqs;
    int                  has_capture;
};

static void hda_remove(struct device *dev);
static struct hda g_hda;
/* An unacknowledged controller reset means its last CORB/RIRB addresses may
 * still be live.  The active instance must nevertheless be reusable when a
 * display controller's HDA function probes before the motherboard codec.  A
 * separate, stable ledger retains those pages for their lifetime instead
 * of either freeing device-owned memory or leaving g_hda.mmio poisoned. */
static struct dma_device g_hda_quarantined_dma = {
    .name = "hda-quarantine", .mask = DMA_MASK_64, .blocked = 1,
};
static int g_hda_dbg;   /* bounds the codec-timeout trace to its first 8 lines */

/* ------------------------------------------------------------- accessors -- */
static inline uint8_t  r8 (struct hda *h, unsigned o) { return *(volatile uint8_t  *)(h->mmio + o); }
static inline uint16_t r16(struct hda *h, unsigned o) { return *(volatile uint16_t *)(h->mmio + o); }
static inline uint32_t r32(struct hda *h, unsigned o) { return *(volatile uint32_t *)(h->mmio + o); }
static inline void w8 (struct hda *h, unsigned o, uint8_t v)  { *(volatile uint8_t  *)(h->mmio + o) = v; }
static inline void w16(struct hda *h, unsigned o, uint16_t v) { *(volatile uint16_t *)(h->mmio + o) = v; }
static inline void w32(struct hda *h, unsigned o, uint32_t v) { *(volatile uint32_t *)(h->mmio + o) = v; }

/* Milliseconds since boot, from the best clock this tree has. */
static uint64_t hda_ms(void)
{
#ifdef HDA_HAVE_KTIME
    return time_mono_ns() / 1000000ull;
#else
    /* 10 ms granularity, and it does not advance with IF=0 -- which is why
     * every caller pairs it with an iteration cap. */
    return timer_ticks() * (timer_ns_per_tick() / 1000000ull);
#endif
}

/* Three characters of string formatting, done by hand.
 *
 * kprintf.h's ksnprintf would be the obvious tool and is deliberately not used:
 * it is part of another line's in-flight work and was not committed when this
 * driver was, which is exactly the dependency that broke HEAD once already. The
 * codec identity line is the single most useful thing this driver prints on
 * unfamiliar hardware, so it is not worth making it wait on someone else's
 * commit. Each of these clamps at `end` and never writes the NUL -- the caller
 * does that once. */
static char *put_str(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *put_dec(char *p, char *end, unsigned v)
{
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 12);
    while (n-- && p < end) *p++ = tmp[n];
    return p;
}

static char *put_hex4(char *p, char *end, unsigned v)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 3; i >= 0; i--)
        if (p < end) *p++ = hx[(v >> (i * 4)) & 0xF];
    return p;
}

static void udelay(unsigned us)
{
#ifdef HDA_HAVE_KTIME
    uint64_t end = time_mono_ns() + (uint64_t)us * 1000ull;
    while (time_mono_ns() < end) __asm__ volatile ("pause");
#else
    /* Coarse on purpose: the tick cannot resolve microseconds, so this is a
     * bounded spin. It is only used for reset settling at probe time, where
     * "at least this long" is the requirement and overshooting costs nothing. */
    volatile unsigned long n = (unsigned long)us * 300ul;
    while (n--) __asm__ volatile ("pause");
#endif
}

/* --------------------------------------------------------- CORB / RIRB --- */

/* Send one verb and wait for its response.
 *
 * CORB/RIRB rather than the immediate-command registers (ICW/IRR/ICS at 0x60):
 * the immediate interface is optional, was deprecated by the spec, and is
 * missing on plenty of real controllers, so a driver built on it works in QEMU
 * and stops at the first laptop. The rings are perhaps thirty lines more.
 *
 * Returns 0 on success and fills *resp; -1 on timeout. */
static int codec_cmd(struct hda *h, unsigned cad, unsigned nid,
                     unsigned verb, unsigned payload, uint32_t *resp)
{
    IO_GUARD(&hda_gate);
    uint32_t val;
    unsigned wp;
    uint64_t deadline;

    /* A 12-bit verb carries an 8-bit payload; a 4-bit verb carries 16 bits.
     * Getting this split wrong silently sends a different command. */
    if ((verb & 0xF00) == verb && payload > 0xFF)
        val = (cad << 28) | (nid << 20) | ((verb & 0xF) << 16) | (payload & 0xFFFF);
    else
        val = (cad << 28) | (nid << 20) | ((verb & 0xFFF) << 8) | (payload & 0xFF);

    /* Ack any pending response status BEFORE kicking the CORB, not only after
     * a reply arrives.
     *
     * This is the bug that cost the most time here, and it is invisible from
     * the driver's own state: the controller counts responses and STOPS
     * FETCHING COMMANDS once that count reaches RINTCNT, until the driver
     * clears the response-interrupt bit in RIRBSTS. So exactly one verb would
     * succeed and every one after it would time out -- with CORBCTL and
     * RIRBCTL both still reading "running", CORBWP correctly advanced, and
     * CORBRP frozen one behind it. The registers all say the bus is healthy;
     * it is simply waiting for an acknowledgement nobody sent.
     *
     * Two belts here. The ack below makes the gate impossible to be closed when
     * we kick, and RINTCNT is programmed to 0xFF in probe() rather than 1, so
     * the count cannot reach it during the short probe conversation at all. */
    w8(h, RIRBSTS, 0x05);

    wp = (h->corb_wp + 1) % h->corb_entries;
    h->corb[wp] = val;
    h->corb_wp = wp;
    w16(h, CORBWP, (uint16_t)wp);

    /* TWO bounds, not one. The clock bound is the meaningful one; the spin
     * bound is what keeps a missing, frozen or IF=0-stalled clock from turning
     * an unresponsive codec into a hung boot. A driver that can hang the
     * machine when its hardware is absent is worse than no driver. */
    deadline = hda_ms() + 50;
    for (unsigned long spins = 0; spins < 200000000ul; spins++) {
        unsigned rwp = (r16(h, RIRBWP) & 0xFF) % h->rirb_entries;
        if (rwp != h->rirb_rp) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_entries;
            if (resp) *resp = (uint32_t)(h->rirb[h->rirb_rp] & 0xFFFFFFFFu);
            w8(h, RIRBSTS, 0x05);          /* ack response + overrun */
            /* The success path used to trace its first eight commands. That
             * was scaffolding for bringing the codec up and it has done its
             * job: the enumeration it was printing is now summarised by the
             * [hda] widget lines and the [snd] boot report. A verbose driver
             * makes the ONE line that matters harder to find. The failure path
             * below keeps its trace, because there the detail is the
             * diagnosis. */
            return 0;
        }
        if (hda_ms() > deadline) {
            /* Bounded, not silenced: a codec that has stopped answering will
             * time out on every command, and eight lines is a diagnosis while
             * hundreds is a flood that pushes the rest of the boot log out. */
            if (g_hda_dbg < 8)
                kprintf("[hda] cmd %x TIMEOUT rirbwp=%u rp=%u corbwp=%u corbrp=%u ctl=%x/%x\n",
                        val, rwp, h->rirb_rp, (unsigned)r16(h, CORBWP),
                        (unsigned)r16(h, CORBRP), (unsigned)r8(h, CORBCTL),
                        (unsigned)r8(h, RIRBCTL));
            g_hda_dbg++;
            return -1;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}

static uint32_t codec_param(struct hda *h, unsigned nid, unsigned param)
{
    uint32_t v = 0;
    if (codec_cmd(h, h->codec_addr, nid, VERB_GET_PARAM, param, &v) != 0) return 0;
    return v;
}

/* ------------------------------------------------------ codec discovery -- */

/* Connection-list helpers, shared by the output pin search below (which
 * already open-coded a short-form (4-per-response) connection list read) and
 * the capture search that follows it. Only the short form is read -- real
 * codecs in this tree's corpus so far (QEMU's, and every ALC datasheet this
 * driver was cross-checked against) never exceed a handful of sources, so 8
 * is generous rather than tight. */
static int hda_conn_len(struct hda *h, unsigned nid)
{
    int n = (int)(codec_param(h, nid, PARAM_CONN_LIST_LEN) & 0x7F);
    return n > 8 ? 8 : n;
}

static int hda_conn_entry(struct hda *h, unsigned nid, unsigned idx, unsigned *out)
{
    uint32_t e = 0;
    if (codec_cmd(h, h->codec_addr, nid, VERB_GET_CONN_LIST, idx & ~3u, &e) != 0)
        return -1;
    *out = (e >> ((idx & 3) * 8)) & 0xFF;      /* short form, 4 per response */
    return 0;
}

static int nid_in(unsigned nid, const unsigned *arr, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (arr[i] == nid) return 1;
    return 0;
}

/* Read GET_PIN_SENSE (0xF09): bit31 is presence -- a jack physically
 * inserted. Only meaningful on a pin whose own PIN_CAP says it has the
 * hardware to sense at all; treating an unsensed pin's read as "0 = nothing
 * plugged in" would be exactly the plausible-small-number failure this tree
 * has been bitten by before (see CLAUDE.md's units-bugs section), so a pin
 * without the capability says so instead of reporting a fake absent. */
static void hda_report_jack(struct hda *h, const char *label, unsigned nid)
{
    uint32_t pcap = codec_param(h, nid, PARAM_PIN_CAP);
    if (pcap & (1u << 2)) {
        uint32_t sense = 0;
        codec_cmd(h, h->codec_addr, nid, VERB_GET_PIN_SENSE, 0, &sense);
        kprintf("[hda] %s pin %u: jack %s\n", label, nid,
                (sense & 0x80000000u) ? "present" : "absent");
    } else {
        kprintf("[hda] %s pin %u: no presence-detect hardware\n", label, nid);
    }
}

/* Walk from each ADC's own connection list, up to two hops, looking for one
 * of the input-capable pins `find_output_path` already validated (connected,
 * PIN_CAP input bit set). The capture-side twin of the pin-to-DAC search
 * below, run in the opposite direction because the ADC is the SINK here
 * instead of the source.
 *
 * Two hops covers both real shapes: a pin wired straight to the ADC (the
 * simple case -- QEMU's codec, and plenty of real ones with a single fixed
 * input) and a pin that goes through an explicit input-selector or
 * summing-mixer widget first (the layout most real multi-input codecs use to
 * offer mic AND line-in on one ADC). Sets h->adc_nid / h->cap_pin_nid /
 * h->cap_sel_nid / h->cap_sel_index on success and returns 1.
 *
 * UNLIKE find_output_path, there is no "just wire the first ADC to the first
 * pin" fallback when the graph search comes up empty. An unproven OUTPUT
 * guess plays into a jack that may not be live -- silent, harmless. An
 * unproven CAPTURE guess reads an ADC that may be listening to a different,
 * disconnected source -- which produces something that LOOKS like a working
 * microphone (silence, or noise off a dead input) and is exactly the
 * "stubbed to success" shape this tree forbids. Refuse instead. */
static int try_capture_path(struct hda *h, const unsigned *adcs, unsigned nadc,
                            const unsigned *inpins, unsigned ninpin)
{
    unsigned a;
    if (!nadc || !ninpin) {
        kprintf("[hda]   fg: %u ADC(s), %u usable input pin(s) -- no capture path\n",
                nadc, ninpin);
        return 0;
    }
    for (a = 0; a < nadc; a++) {
        int len = hda_conn_len(h, adcs[a]);
        unsigned k;
        for (k = 0; k < (unsigned)len; k++) {
            unsigned entry, t;
            if (hda_conn_entry(h, adcs[a], k, &entry) != 0) break;
            t = (codec_param(h, entry, PARAM_AUDIO_WIDGET_CAP) >> 20) & 0xF;
            if (t == WIDGET_PIN && nid_in(entry, inpins, ninpin)) {
                h->adc_nid = adcs[a];
                h->cap_pin_nid = entry;
                if (len > 1) { h->cap_sel_nid = adcs[a]; h->cap_sel_index = k; }
                kprintf("[hda]   capture: adc %u <- pin %u direct (index %u/%d)\n",
                        h->adc_nid, h->cap_pin_nid, k, len);
                return 1;
            }
            if (t == WIDGET_SEL || t == WIDGET_MIXER) {
                int len2 = hda_conn_len(h, entry);
                unsigned k2;
                for (k2 = 0; k2 < (unsigned)len2; k2++) {
                    unsigned e2;
                    if (hda_conn_entry(h, entry, k2, &e2) != 0) break;
                    if (nid_in(e2, inpins, ninpin)) {
                        h->adc_nid = adcs[a];
                        h->cap_pin_nid = e2;
                        /* A mixer sums every input with no select verb; only
                         * a selector (or the ADC itself, if IT has more than
                         * one source) needs one programmed. */
                        if (t == WIDGET_SEL && len2 > 1) {
                            h->cap_sel_nid = entry; h->cap_sel_index = k2;
                        } else if (len > 1) {
                            h->cap_sel_nid = adcs[a]; h->cap_sel_index = k;
                        }
                        kprintf("[hda]   capture: adc %u <- %s %u <- pin %u\n",
                                h->adc_nid, t == WIDGET_SEL ? "sel" : "mix", entry,
                                h->cap_pin_nid);
                        return 1;
                    }
                }
            }
        }
    }
    kprintf("[hda]   capture: %u ADC(s), %u input pin(s), no connection-list "
            "edge between any pair -- refusing rather than guessing\n", nadc, ninpin);
    return 0;
}

/* Walk the widget graph and find a DAC that can reach a usable output pin.
 * While it is walking the graph anyway, it also gathers the ADCs and
 * input-capable pins this fg has and hands them to try_capture_path -- one
 * widget scan instead of two, and capture is resolved independently of
 * whether THIS fg also has a usable output path (a codec can fail one
 * direction and still serve the other).
 *
 * Deliberately generic rather than hardcoded to QEMU's node numbering. QEMU's
 * hda-output codec happens to put the DAC at nid 2 and the pin at nid 3, and
 * hardcoding that would work today and drive nothing else -- which defeats the
 * point of driving HDA at all. Returns 0 if an OUTPUT path was found (capture
 * may or may not have been; check h->adc_nid). */
static int find_output_path(struct hda *h)
{
    uint32_t sub, cap;
    unsigned fg_start, fg_count, i;

    sub = codec_param(h, 0, PARAM_SUBNODE_COUNT);
    fg_start = (sub >> 16) & 0xFF;
    fg_count = sub & 0xFF;
    /* The widget walk, printed. This is the audio equivalent of the [dev]
     * table: when a machine is silent, the question is always "what did the
     * codec say it had", and reconstructing that from a single failure line is
     * impossible. Four lines on QEMU, a dozen on a laptop -- cheap either way,
     * and the first thing anyone needs. */
    kprintf("[hda] codec %u: root subnodes %u..%u\n",
            h->codec_addr, fg_start, fg_start + fg_count - 1);

    for (i = 0; i < fg_count; i++) {
        unsigned fg = fg_start + i;
        unsigned w_start, w_count, j;
        unsigned dacs[16], ndac = 0;
        unsigned pins[16], npin = 0;
        unsigned adcs[16], nadc = 0;
        unsigned inpins[16], ninpin = 0;

        {
            uint32_t t = codec_param(h, fg, PARAM_FG_TYPE);
            sub = codec_param(h, fg, PARAM_SUBNODE_COUNT);
            kprintf("[hda]  fg %u: type %x, widgets %u..%u\n", fg, t,
                    (unsigned)((sub >> 16) & 0xFF),
                    (unsigned)(((sub >> 16) & 0xFF) + (sub & 0xFF) - 1));
            if ((t & 0x7F) != 0x01) continue;   /* not an Audio Function Group */
        }

        /* An AFG comes up powered down on plenty of real codecs; D0 first or
         * every subsequent verb reads back plausible nonsense. */
        codec_cmd(h, h->codec_addr, fg, VERB_SET_POWER, 0, 0);

        sub = codec_param(h, fg, PARAM_SUBNODE_COUNT);
        w_start = (sub >> 16) & 0xFF;
        w_count = sub & 0xFF;

        for (j = 0; j < w_count; j++) {
            unsigned nid = w_start + j;
            unsigned type;
            cap = codec_param(h, nid, PARAM_AUDIO_WIDGET_CAP);
            type = (cap >> 20) & 0xF;
            if (type == WIDGET_DAC && ndac < 16) {
                dacs[ndac++] = nid;
                kprintf("[hda]   nid %u: DAC (cap %x)\n", nid, cap);
            } else if (type == WIDGET_ADC && nadc < 16) {
                adcs[nadc++] = nid;
                kprintf("[hda]   nid %u: ADC (cap %x)\n", nid, cap);
            } else if (type == WIDGET_PIN) {
                uint32_t pcap = codec_param(h, nid, PARAM_PIN_CAP);
                uint32_t cfg = 0;
                int connected;
                codec_cmd(h, h->codec_addr, nid, VERB_GET_CONFIG_DEF, 0, &cfg);
                kprintf("[hda]   nid %u: PIN (cap %x pincap %x cfg %x)\n",
                        nid, cap, pcap, cfg);
                /* Port connectivity 0x1 == "no physical connection". Picking
                 * one of those is how sound goes to (or is expected FROM) a
                 * jack that does not exist -- every register correct, nothing
                 * audible or nothing captured. Checked once for both
                 * directions rather than duplicated per branch, because a pin
                 * can be output-capable, input-capable, or (real hardware's
                 * combo mic/line jacks) both. */
                connected = ((cfg >> 30) & 0x3) != 0x1;
                if (connected && (pcap & (1u << 4)) && npin   < 16) pins[npin++]     = nid;
                if (connected && (pcap & (1u << 5)) && ninpin < 16) inpins[ninpin++] = nid;
            } else {
                kprintf("[hda]   nid %u: type %u (cap %x)\n", nid, type, cap);
            }
        }

        /* Independent of the output check below: a fg with no usable output
         * pin can still have a working microphone, and the reverse. Guarded
         * so a second fg (rare, but the loop does not assume there is only
         * one AFG) does not overwrite a capture path an earlier fg already
         * proved. */
        if (!h->adc_nid)
            try_capture_path(h, adcs, nadc, inpins, ninpin);

        if (!ndac || !npin) {
            kprintf("[hda]   fg %u: %u DAC(s), %u usable output pin(s)\n",
                    fg, ndac, npin);
            continue;
        }

        /* Prefer a pin that actually lists a DAC among its sources; fall back
         * to the first output pin and the first DAC, which is right for the
         * simple one-path codecs (QEMU's included) whose pin has a single
         * hardwired source and may report an empty connection list. */
        for (j = 0; j < npin; j++) {
            uint32_t len = codec_param(h, pins[j], PARAM_CONN_LIST_LEN) & 0x7F;
            unsigned k;
            for (k = 0; k < len && k < 8; k++) {
                uint32_t e = 0;
                unsigned entry;
                if (codec_cmd(h, h->codec_addr, pins[j], VERB_GET_CONN_LIST, k & ~3u, &e) != 0)
                    break;
                entry = (e >> ((k & 3) * 8)) & 0xFF;      /* short form, 4 per response */
                for (unsigned d = 0; d < ndac; d++)
                    if (dacs[d] == entry) {
                        h->dac_nid = dacs[d];
                        h->pin_nid = pins[j];
                        return 0;
                    }
            }
        }
        h->dac_nid = dacs[0];
        h->pin_nid = pins[0];
        return 0;
    }
    return -1;
}

/* SDnFMT / converter format: base 48 kHz, x1, /1, 16-bit, 2 channels. */
#define HDA_FMT_48K_S16_2CH  0x0011

static void codec_setup_output(struct hda *h)
{
    /* Route: tell the DAC which stream tag to consume and which channel to
     * start at. THIS is the connection between the DMA engine and the codec --
     * without it the engine runs, LPIB advances, and nothing is heard. */
    codec_cmd(h, h->codec_addr, h->dac_nid, VERB_SET_STREAM_FMT, HDA_FMT_48K_S16_2CH, 0);
    codec_cmd(h, h->codec_addr, h->dac_nid, VERB_SET_STREAM_CHAN,
              (HDA_STREAM_TAG << 4) | 0, 0);

    /* Unmute and set gain. 0xB000 = set output amp, left+right, index 0,
     * unmuted, gain 0x00... which on most codecs is the MINIMUM, not 0 dB. Use
     * a mid-scale gain instead: a driver that "works" but is inaudible because
     * it left the amp at its floor is a classic. */
    codec_cmd(h, h->codec_addr, h->dac_nid, VERB_SET_AMP, 0, 0);   /* payload below */
    {
        /* SET_AMP is a 4-bit verb with a 16-bit payload:
         * b15 set-output, b14 set-input, b13 left, b12 right, b7 mute,
         * b6..0 gain. */
        unsigned p = (1u << 15) | (1u << 13) | (1u << 12) | 0x2A;
        codec_cmd(h, h->codec_addr, h->dac_nid, VERB_SET_AMP, p, 0);
        codec_cmd(h, h->codec_addr, h->pin_nid, VERB_SET_AMP, p, 0);
    }

    /* Pin: output enable (b6) + headphone drive (b7). */
    codec_cmd(h, h->codec_addr, h->pin_nid, VERB_SET_PIN_CTL, 0x40 | 0x80, 0);
    /* EAPD/amplifier power. Harmless where unsupported, and on the laptops that
     * do need it the speakers stay dead without it. */
    codec_cmd(h, h->codec_addr, h->pin_nid, VERB_SET_EAPD, 0x02, 0);
    codec_cmd(h, h->codec_addr, h->pin_nid, VERB_SET_POWER, 0, 0);
    codec_cmd(h, h->codec_addr, h->dac_nid, VERB_SET_POWER, 0, 0);
}

/* The capture-side twin of codec_setup_output. Same verbs, opposite amp
 * direction bit, and one extra step output never needs: programming WHICH
 * source a multi-input ADC (or an intermediate selector) actually listens to,
 * because unlike a DAC -- which only ever has one thing to say -- an ADC can
 * have several possible inputs and defaults to an unspecified one. */
static void codec_setup_capture(struct hda *h)
{
    if (h->cap_sel_nid)
        codec_cmd(h, h->codec_addr, h->cap_sel_nid, VERB_SET_CONN_SELECT,
                  h->cap_sel_index, 0);

    /* Route: tell the ADC which stream tag to FILL and which channel to start
     * at. Same connection the DAC needs on the output side -- without it the
     * engine runs, LPIB advances, and the bytes that land in the ring are
     * whatever was there before (typically the zeroed startup buffer), not
     * anything the pin is receiving. */
    codec_cmd(h, h->codec_addr, h->adc_nid, VERB_SET_STREAM_FMT, HDA_FMT_48K_S16_2CH, 0);
    codec_cmd(h, h->codec_addr, h->adc_nid, VERB_SET_STREAM_CHAN,
              (HDA_CAPTURE_STREAM_TAG << 4) | 0, 0);

    {
        /* b14 SET-INPUT instead of output's b15 SET-OUTPUT is the whole
         * difference from codec_setup_output's amp write, and getting it
         * backwards is invisible for the exact reason this file's header
         * warns about generally: the verb completes, the register reads back
         * programmed, and only the direction that actually reaches the wire
         * is wrong. The index field (bits 11:8) selects which of the ADC's
         * several possible SOURCES this gain applies to on a multi-source
         * ADC -- leaving it 0 on a widget where cap_sel_index != 0 would
         * gain-stage a source that is not the one actually selected, and
         * leave the real one at its power-on default (frequently muted). */
        unsigned idx = (h->cap_sel_nid == h->adc_nid) ? h->cap_sel_index : 0;
        unsigned p = (1u << 14) | (1u << 13) | (1u << 12) | (idx << 8) | 0x2A;
        codec_cmd(h, h->codec_addr, h->adc_nid, VERB_SET_AMP, p, 0);
        codec_cmd(h, h->codec_addr, h->cap_pin_nid, VERB_SET_AMP,
                  (1u << 14) | (1u << 13) | (1u << 12) | 0x2A, 0);
    }

    /* Pin: Input Enable is bit5 (0x20) -- NOT bit6/bit7, which are Output
     * Enable and Headphone-Amp-Enable on the output side. VREF (bits 2:0) is
     * left at its power-on default (HiZ / no bias): QEMU's virtual codec
     * models no analogue bias network for this to matter to, and a real
     * external mic that needs VREF bias to produce a signal at all is a
     * real-hardware gap this driver has -- named here and in the report
     * rather than left silent, not fixed, because there is no way to verify
     * it against anything this environment can run. */
    codec_cmd(h, h->codec_addr, h->cap_pin_nid, VERB_SET_PIN_CTL, 0x20, 0);
    codec_cmd(h, h->codec_addr, h->cap_pin_nid, VERB_SET_POWER, 0, 0);
    codec_cmd(h, h->codec_addr, h->adc_nid, VERB_SET_POWER, 0, 0);
}

/* ---------------------------------------------------------- the engine --- */

static void hda_stop(struct snd_device *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->out_base;
    w8(h, sd + SD_CTL, 0);                     /* clear RUN + interrupt enables */
    for (unsigned i = 0; i < 100 && (r8(h, sd + SD_CTL) & 2); i++) udelay(100);
    if (r8(h, sd + SD_CTL) & 2) {
        dma_device_quarantine(&h->dma);
        kprintf("[hda] stream stop unconfirmed: DMA quarantined\n");
        return;
    }
    w8(h, sd + SD_STS, 0x1C);                  /* ack BCIS/FIFOE/DESE */
    if (h->ring_dma && h->ring_dma->state == DMA_DEVICE_OWNED)
        dma_buffer_complete(h->ring_dma, h->ring_dma->token);
    if (h->bdl_dma && h->bdl_dma->state == DMA_DEVICE_OWNED)
        dma_buffer_complete(h->bdl_dma, h->bdl_dma->token);
}

static int hda_start(struct snd_device *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->out_base, i;
    if (__atomic_load_n(&h->removing, __ATOMIC_ACQUIRE) || h->dma.blocked) return -1;
    uint64_t ring_phys = dma_addr_value(h->ring_dma->dma);
    uint64_t bdl_phys  = dma_addr_value(h->bdl_dma->dma);

    /* Stream reset. SRST must be observed as 1 and then as 0 -- writing it and
     * carrying on leaves the descriptor half-configured, which shows up as a
     * DMA engine that starts and immediately reports a descriptor error. */
    w32(h, sd + SD_CTL, 1);
    for (i = 0; i < 100 && !(r8(h, sd + SD_CTL) & 1); i++) udelay(10);
    w32(h, sd + SD_CTL, 0);
    for (i = 0; i < 100 && (r8(h, sd + SD_CTL) & 1); i++) udelay(10);
    if (r8(h, sd + SD_CTL) & 1) {
        kprintf("[hda] stream reset did not clear\n");
        return -1;
    }

    /* One BDL entry per period, each asking for an interrupt. That is what
     * makes the refill interrupt-driven: the engine tells us it crossed a
     * period boundary instead of us polling LPIB and guessing. */
    for (i = 0; i < HDA_PERIODS; i++) {
        h->bdl[i].addr  = ring_phys + (uint64_t)i * HDA_PERIOD_BYTES;
        h->bdl[i].len   = HDA_PERIOD_BYTES;
        h->bdl[i].flags = 1;                   /* IOC */
    }

    if (h->ring_dma->state != DMA_DEVICE_OWNED &&
        !dma_buffer_submit(h->ring_dma)) return -1;
    if (h->bdl_dma->state != DMA_DEVICE_OWNED &&
        !dma_buffer_submit(h->bdl_dma)) return -1;
    dma_wmb();
    w32(h, sd + SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    w32(h, sd + SD_BDPU, (uint32_t)(bdl_phys >> 32));
    w32(h, sd + SD_CBL, HDA_RING_BYTES);
    w16(h, sd + SD_LVI, HDA_PERIODS - 1);
    w16(h, sd + SD_FMT, HDA_FMT_48K_S16_2CH);
    w8 (h, sd + SD_STS, 0x1C);

    /* Stream number in bits 23:20, plus RUN and interrupt-on-completion. The
     * stream number must match what the DAC was told, or the bytes go to a
     * converter that is not connected to anything. */
    w32(h, sd + SD_CTL, (HDA_STREAM_TAG << 20) | (1u << 2) | (1u << 1));

    /* Global interrupt enable + this stream's bit. The stream's INTCTL bit is
     * indexed by its position among ALL descriptors, not among the output
     * ones -- input streams come first.
     *
     * READ-MODIFY-WRITE, not the plain overwrite this line used to be. INTCTL
     * is ONE register shared by every stream on the controller: an overwrite
     * here would silently clear the capture stream's enable bit if capture
     * had already been started (hda_cap_start below sets its own bit the same
     * way) -- and the reverse just as true if capture starts after playback.
     * The failure mode is nasty precisely because it is not visible from
     * either engine's own registers: SD_CTL still reads RUN, LPIB still
     * advances, and the OTHER stream simply stops generating interrupts,
     * which looks exactly like "the ISR was never wired" from that stream's
     * side. */
    {
        unsigned idx = (h->out_base - SD_BASE) / 0x20;
        w32(h, INTCTL, r32(h, INTCTL) | (1u << 31) | (1u << 30) | (1u << idx));
    }
    return 0;
}

static uint64_t hda_position(struct snd_device *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    return r32(h, h->out_base + SD_LPIB) / (HDA_CHANNELS * 2u);
}

/* -------------------------------------------------------- capture engine -- */

static void hda_cap_stop(struct snd_capdevice *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->in_base;
    w8(h, sd + SD_CTL, 0);
    for (unsigned i = 0; i < 100 && (r8(h, sd + SD_CTL) & 2); i++) udelay(100);
    if (r8(h, sd + SD_CTL) & 2) {
        dma_device_quarantine(&h->dma);
        kprintf("[hda] stream stop unconfirmed: DMA quarantined\n");
        return;
    }
    w8(h, sd + SD_STS, 0x1C);
    if (h->cap_ring_dma && h->cap_ring_dma->state == DMA_DEVICE_OWNED)
        dma_buffer_complete(h->cap_ring_dma, h->cap_ring_dma->token);
    if (h->cap_bdl_dma && h->cap_bdl_dma->state == DMA_DEVICE_OWNED)
        dma_buffer_complete(h->cap_bdl_dma, h->cap_bdl_dma->token);

}

#ifdef HDA_DMA_CAPTURE_CANARY
void hda_capture_stop_check(void)
{
    static unsigned closes;
    struct hda *h = &g_hda;
    /* Allow an already pending interrupt to drain before taking the baseline.
     * The guest waits 250 ms before reopen, so this observer cannot race restart. */
    sched_sleep_ms(20);
    uint32_t pos = r32(h, h->in_base + SD_LPIB);
    uint64_t irqs = h->cap_irqs;
    memset(h->cap_ring, 0xc7, HDA_RING_BYTES);
    dma_wmb();
    sched_sleep_ms(100);
    int ok = pos == r32(h, h->in_base + SD_LPIB) && irqs == h->cap_irqs;
    for (unsigned i = 0; i < HDA_RING_BYTES; i++)
        if (((volatile uint8_t *)h->cap_ring)[i] != 0xc7) ok = 0;
    kprintf("HDA_CAPTURE_STOP_%s lpib=%u irqs=%llu\n", ok ? "PASS" : "FAIL", pos, irqs);
    if (++closes == 2) {
        hda_remove(h->dev);
        kprintf("HDA_REMOVE_%s\n", !h->dma.buffers && !snd_present() && !snd_capture_present() ? "PASS" : "FAIL");
    }
}
#endif

static int hda_cap_start(struct snd_capdevice *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->in_base, i;
    if (__atomic_load_n(&h->removing, __ATOMIC_ACQUIRE) || h->dma.blocked) return -1;
    uint64_t ring_phys = dma_addr_value(h->cap_ring_dma->dma);
    uint64_t bdl_phys  = dma_addr_value(h->cap_bdl_dma->dma);

    /* Identical reset/program/run sequence to hda_start -- the DMA engine
     * does not know or care which direction it moves bytes, only that SRST
     * must be OBSERVED both asserted and cleared before the descriptor is
     * touched. See hda_start's comment for why that observation matters. */
    w32(h, sd + SD_CTL, 1);
    for (i = 0; i < 100 && !(r8(h, sd + SD_CTL) & 1); i++) udelay(10);
    w32(h, sd + SD_CTL, 0);
    for (i = 0; i < 100 && (r8(h, sd + SD_CTL) & 1); i++) udelay(10);
    if (r8(h, sd + SD_CTL) & 1) {
        kprintf("[hda] capture stream reset did not clear\n");
        return -1;
    }

    for (i = 0; i < HDA_PERIODS; i++) {
        h->cap_bdl[i].addr  = ring_phys + (uint64_t)i * HDA_PERIOD_BYTES;
        h->cap_bdl[i].len   = HDA_PERIOD_BYTES;
        h->cap_bdl[i].flags = 1;                   /* IOC */
    }

    if (h->cap_ring_dma->state != DMA_DEVICE_OWNED &&
        !dma_buffer_submit(h->cap_ring_dma)) return -1;
    if (h->cap_bdl_dma->state != DMA_DEVICE_OWNED &&
        !dma_buffer_submit(h->cap_bdl_dma)) return -1;
#ifdef HDA_DMA_CAPTURE_CANARY
    /* A silent backend must overwrite this poison through the actual capture
     * DMA engine; zero-initialised buffers would not establish that fact. */
    memset(h->cap_ring, 0xa5, HDA_RING_BYTES);
    kprintf("HDA_CAPTURE_CANARY dma=%p bytes=%u\n",
            (void *)ring_phys, HDA_RING_BYTES);
#endif
    dma_wmb();
    w32(h, sd + SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFu));
    w32(h, sd + SD_BDPU, (uint32_t)(bdl_phys >> 32));
    w32(h, sd + SD_CBL, HDA_RING_BYTES);
    w16(h, sd + SD_LVI, HDA_PERIODS - 1);
    w16(h, sd + SD_FMT, HDA_FMT_48K_S16_2CH);
    w8 (h, sd + SD_STS, 0x1C);

    w32(h, sd + SD_CTL, (HDA_CAPTURE_STREAM_TAG << 20) | (1u << 2) | (1u << 1));

    /* Same read-modify-write as hda_start, and for the identical reason: this
     * is the SAME INTCTL register playback's engine also owns a bit of. */
    {
        unsigned idx = (h->in_base - SD_BASE) / 0x20;
        w32(h, INTCTL, r32(h, INTCTL) | (1u << 31) | (1u << 30) | (1u << idx));
    }
    return 0;
}

static void hda_isr(void *arg)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)arg;
    int removing = __atomic_load_n(&h->removing, __ATOMIC_ACQUIRE);
#ifdef HDA_X79_NEGCTL_ISR_EARLY_RETURN_ON_REMOVE
    if (removing) return;
#endif
    uint32_t sts = r32(h, INTSTS);
    unsigned out_idx = (h->out_base - SD_BASE) / 0x20;

    /* ONE shared, level-triggered interrupt line for the whole controller --
     * both engines' status bits must be checked and, if set, ACKED, every
     * time this fires. The previous version returned after checking only
     * out_idx: with capture wired to the same INTx/MSI vector (there is only
     * one to wire it to), a capture-only completion would hit that early
     * return, leave SD_STS unacknowledged, and re-fire immediately -- an
     * interrupt storm, the exact failure the write-1-to-clear comment below
     * already names as the worst way this driver can fail. Checking BOTH
     * unconditionally, ack-then-report per stream, is what makes coexistence
     * safe rather than merely usually-working. */
    if (sts & (1u << out_idx)) {
        uint8_t ss = r8(h, h->out_base + SD_STS);
        /* Write-1-to-clear. Not clearing leaves the line asserted, and with a
         * level-triggered INTx that is an interrupt storm that wedges the
         * machine -- the worst possible way for an audio driver to fail. */
        w8(h, h->out_base + SD_STS, ss & 0x1C);
        if ((ss & 0x04) && !removing) {    /* BCIS: a buffer (period) completed */
            h->irqs++;
            snd_period_elapsed(&h->snd);
        }
    }

    if (h->has_capture) {
        unsigned in_idx = (h->in_base - SD_BASE) / 0x20;
        if (sts & (1u << in_idx)) {
            uint8_t ss = r8(h, h->in_base + SD_STS);
            w8(h, h->in_base + SD_STS, ss & 0x1C);
            if ((ss & 0x04) && !removing) { /* BCIS: a buffer (period) filled */
                h->cap_irqs++;
                snd_capture_period_elapsed(&h->cap);
            }
        }
    }
}

/* ------------------------------------------------------------- probing --- */

#define HDA_PCI_CLASS       0x04
#define HDA_PCI_SUBCLASS    0x03
#define HDA_INTEL_VENDOR    0x8086
#define HDA_X79_DEVICE      0x1d20
#define HDA_NVIDIA_VENDOR   0x10de
#define HDA_RUN             0x02

/* Bring up BAR decoding without ever reviving firmware DMA.  The legacy
 * wrappers now use the checked Command helper: disable first removes decode
 * and BME, then dev_enable(0) restores only the enumerated BAR decode while
 * keeping BME clear.  The explicit final readback remains the HDA ownership
 * gate; a controller that will not confirm MEM=1/BME=0 is never mapped/reset. */
static int hda_pci_mem_only(struct device *dev)
{
#ifdef HDA_X79_NEGCTL_MASTER_BEFORE_QUIESCE
    /* Mutation control: the historical order restored BME while firmware
     * CORB/RIRB/stream pointers could still name reclaimed boot memory. */
    dev_enable(dev, 1);
    return 0;
#else
    dev_disable(dev);
    dev_enable(dev, 0);
    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
    if (command != UINT16_MAX && (command & PCI_CMD_MEM) &&
        !(command & PCI_CMD_MASTER))
        return 0;
    kprintf("[hda] %s: PCI MEM-only state unconfirmed (command=%x)\n",
            dev->name, command);
    dev_disable(dev);
    return -1;
#endif
}

static int hda_pci_master_enable(struct device *dev)
{
#ifdef HDA_X79_NEGCTL_MASTER_BEFORE_QUIESCE
    (void)dev;
    return 0;
#else
    dev_enable(dev, 1);
    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
    if (command != UINT16_MAX &&
        (command & (PCI_CMD_MEM | PCI_CMD_MASTER)) ==
        (PCI_CMD_MEM | PCI_CMD_MASTER))
        return 0;
    kprintf("[hda] %s: PCI bus-master enable unconfirmed (command=%x)\n",
            dev->name, command);
    return -1;
#endif
}

/* Clear one DMA engine and observe RUN low. Merely issuing the write is
 * insufficient: firmware may have left a command ring or stream descriptor
 * active, and CRST is forbidden until every engine has acknowledged stop. */
static int hda_stop_run(struct hda *h, unsigned reg, uint8_t clear_bits)
{
    uint8_t ctl = r8(h, reg);
    w8(h, reg, (uint8_t)(ctl & ~clear_bits));
    for (unsigned i = 0; i < 100 && (r8(h, reg) & HDA_RUN); i++)
        udelay(100);
#ifdef HDA_X79_NEGCTL_IGNORE_STUCK_FIRMWARE_RUN
    return 0;
#else
    return (r8(h, reg) & HDA_RUN) ? -1 : 0;
#endif
}

/* Stop every host-DMA source advertised by GCAP.  Input descriptors precede
 * output descriptors, followed by bidirectional descriptors; all three groups
 * use the same 0x20-byte register shape. */
static int hda_stop_all_dma(struct hda *h)
{
    if (!h->mmio) return 0;
    uint16_t cap = r16(h, GCAP);
    unsigned streams = ((cap >> 8) & 0x0f) + ((cap >> 12) & 0x0f) +
                       ((cap >> 3) & 0x1f);
    if (streams > 30) {
        kprintf("[hda] invalid GCAP stream count %u; reset refused\n", streams);
        return -1;
    }

    w32(h, INTCTL, 0);
    int stopped = 0;
    if (hda_stop_run(h, CORBCTL, 0x03) != 0) stopped = -1;
    if (hda_stop_run(h, RIRBCTL, 0x07) != 0) stopped = -1;
    for (unsigned i = 0; i < streams; i++)
        if (hda_stop_run(h, SD_BASE + i * 0x20 + SD_CTL, 0x1e) != 0)
            stopped = -1;
    if (stopped)
        kprintf("[hda] firmware DMA RUN state did not quiesce; CRST refused\n");
    return stopped;
}

/* CORBSIZE/RIRBSIZE advertise supported depths in bits 6:4 and accept the
 * selected encoding in bits 1:0. Prefer the largest implemented ring, but
 * support the legal 16- and 2-entry-only controllers too. */
static int hda_set_ring_size(struct hda *h, unsigned reg, unsigned *entries)
{
    uint8_t caps = r8(h, reg);
    uint8_t select;
#ifdef HDA_X79_NEGCTL_FORCE_256_RINGS
    (void)caps;
    select = 2;
    *entries = 256;
#else
    if (caps & 0x40)      { select = 2; *entries = 256; }
    else if (caps & 0x20) { select = 1; *entries = 16; }
    else if (caps & 0x10) { select = 0; *entries = 2; }
    else return -1;
#endif
    w8(h, reg, select);
    if ((r8(h, reg) & 0x03) != select) return -1;
    return 0;
}

static int hda_is_x79_onboard(const struct device *dev)
{
    /* Patsburg exposes the X79/C600 HD Audio function at 00:1b.0.  Requiring
     * both the published ID and the integrated BDF prevents an unrelated
     * passthrough endpoint with a copied ID from becoming the boot-wide
     * single HDA instance. */
    return dev && dev->bus_type == DEV_BUS_PCI && dev->seg == 0 &&
           dev->bus == 0 && dev->slot == 0x1b && dev->func == 0 &&
           dev->vendor == HDA_INTEL_VENDOR && dev->device == HDA_X79_DEVICE &&
           dev->class_code == HDA_PCI_CLASS &&
           dev->subclass == HDA_PCI_SUBCLASS;
}

static int hda_has_display_sibling(const struct device *dev)
{
    /* A GPU's HDMI/DP codec is normally function 1 beside display function 0.
     * Vendor matching alone is too narrow (the same shape exists on AMD and
     * Intel GPUs); the enumerated multifunction relationship is the property
     * that identifies it without touching either function. */
    for (int i = 0; dev && i < dev_count(); i++) {
        const struct device *sibling = dev_at(i);
        if (!sibling || sibling == dev || sibling->bus_type != DEV_BUS_PCI)
            continue;
        if (sibling->seg == dev->seg && sibling->bus == dev->bus &&
            sibling->slot == dev->slot && sibling->func != dev->func &&
            sibling->class_code == 0x03)
            return 1;
    }
    return 0;
}

static int hda_x79_onboard_present(void)
{
    for (int i = 0; i < dev_count(); i++)
        if (hda_is_x79_onboard(dev_at(i))) return 1;
    return 0;
}

static int hda_controller_candidate(const struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI ||
        dev->class_code != HDA_PCI_CLASS ||
        dev->subclass != HDA_PCI_SUBCLASS)
        return 0;
#ifdef HDA_X79_NEGCTL_ACCEPT_GPU_AUDIO
    /* Mutation control: recreates the GTX 1050 HDMI function taking g_hda
     * before the X79 controller is reached. */
    if (dev->vendor == HDA_NVIDIA_VENDOR) return 1;
#endif
    if (hda_has_display_sibling(dev)) return 0;
    if (!hda_is_x79_onboard(dev) && hda_x79_onboard_present()) return 0;
    return 1;
}

static void hda_retain_unconfirmed_dma(struct hda *h)
{
    dma_device_quarantine(&h->dma);

    /* dma_device_quarantine changes only device-owned objects.  Objects not
     * yet published are safe to release even though another object in this
     * probe must be retained. */
    for (struct dma_buffer *b = h->dma.buffers, *next; b; b = next) {
        next = b->next;
        if (b->state != DMA_DEVICE_OWNED && b->state != DMA_QUARANTINED)
            (void)dma_free_coherent(b);
    }

    if (h->dma.buffers) {
        struct dma_buffer *tail = h->dma.buffers;
        for (struct dma_buffer *b = h->dma.buffers; b; b = b->next) {
            b->dev = &g_hda_quarantined_dma;
            tail = b;
        }
        tail->next = g_hda_quarantined_dma.buffers;
        g_hda_quarantined_dma.buffers = h->dma.buffers;
        h->dma.buffers = NULL;
    }
    if (h->dma.mappings) {
        struct dma_mapping *tail = h->dma.mappings;
        for (struct dma_mapping *m = h->dma.mappings; m; m = m->next) {
            m->dev = &g_hda_quarantined_dma;
            tail = m;
        }
        tail->next = g_hda_quarantined_dma.mappings;
        g_hda_quarantined_dma.mappings = h->dma.mappings;
        h->dma.mappings = NULL;
    }
}

static void hda_probe_abort(struct hda *h, struct device *dev)
{
    int reset_ok = 1;

    __atomic_store_n(&h->removing, 1, __ATOMIC_RELEASE);
    if (h->mmio) {
        /* Stop every DMA-capable engine before judging ownership.  CRST is the
         * controller-wide acknowledgement; a write request by itself is not
         * evidence that the controller stopped fetching memory. */
        if (hda_stop_all_dma(h) != 0) {
            reset_ok = 0;
        } else {
            w32(h, GCTL, 0);
            for (unsigned i = 0; i < 100 && (r32(h, GCTL) & 1); i++) udelay(100);
            reset_ok = !(r32(h, GCTL) & 1);
        }
    }

    if (reset_ok) {
        dma_device_quiesced(&h->dma);
        while (h->dma.buffers)
            if (dma_free_coherent(h->dma.buffers) != 0) break;
    } else if (h->dma.buffers || h->dma.mappings) {
#ifdef HDA_X79_NEGCTL_RELEASE_UNACKED_DMA
        /* Mutation control: discards the quarantine on an unobserved reset. */
        hda_retain_unconfirmed_dma(h);
        dma_device_quiesced(&g_hda_quarantined_dma);
        while (g_hda_quarantined_dma.buffers)
            if (dma_free_coherent(g_hda_quarantined_dma.buffers) != 0) break;
#else
        hda_retain_unconfirmed_dma(h);
#endif
        kprintf("[hda] failed reset: DMA memory quarantined\n");
    }

    dev_set_drvdata(dev, NULL);
    dev_disable(dev);
#ifndef HDA_X79_NEGCTL_KEEP_POISONED_INSTANCE
    /* The device model can now continue to a later HDA function.  The
     * quarantine ledger above, rather than these active-instance pointers,
     * owns anything hardware could still reach. */
    memset(h, 0, sizeof *h);
#endif
}

static int hda_probe(struct device *dev)
{
    struct hda *h = &g_hda;
    uint64_t bar;
    uint16_t statests;
    unsigned i, iss, oss;

    if (!hda_controller_candidate(dev)) {
        if (hda_has_display_sibling(dev))
            kprintf("[hda] %s: display-function audio skipped\n", dev->name);
        else if (hda_x79_onboard_present())
            kprintf("[hda] %s: deferred to X79 8086:1d20 at 00:1b.0\n",
                    dev->name);
        return -1;
    }
    if (h->mmio) return -1;          /* one output device; see snd_register_device */

    if (hda_pci_mem_only(dev) != 0) return -1;
    bar = dev_bar_map(dev, 0);
    if (!bar) { kprintf("[hda] %s: no BAR0\n", dev->name); goto probe_fail; }

    h->mmio = (volatile uint8_t *)(uintptr_t)bar;
    h->dev = dev;

    /* Firmware can leave command rings and any advertised stream running.
     * PCI BME is confirmed low above, so stale physical pointers cannot fetch
     * while each RUN bit is cleared and read back. CRST is legal only after
     * every engine reports stopped. */
    if (hda_stop_all_dma(h) != 0) goto probe_fail;

    /* Controller reset: CRST low, wait for it to read back low, then high and
     * wait for it to read back high. Both directions must be OBSERVED. */
    w32(h, GCTL, 0);
    for (i = 0; i < 100 && (r32(h, GCTL) & 1); i++) udelay(100);
    if (r32(h, GCTL) & 1) {
        kprintf("[hda] %s: controller did not enter reset\n", dev->name);
        goto probe_fail;
    }
    w32(h, GCTL, 1);
    for (i = 0; i < 100 && !(r32(h, GCTL) & 1); i++) udelay(100);
    if (!(r32(h, GCTL) & 1)) {
        kprintf("[hda] %s: controller did not come out of reset\n", dev->name);
        goto probe_fail;
    }
    /* Codecs need time on the link before STATESTS is meaningful. Reading it
     * immediately finds zero codecs on hardware that has several. */
    udelay(1000);

    {
        uint16_t cap = r16(h, GCAP);
        dma_device_init(&h->dma, "hda", (cap & 1) ? DMA_MASK_64 : DMA_MASK_32);
        iss = (cap >> 8) & 0xF;
        oss = (cap >> 12) & 0xF;
        if (!oss) { kprintf("[hda] %s: no output streams\n", dev->name); goto probe_fail; }
        h->out_base = SD_BASE + iss * 0x20;    /* input descriptors come first */
        /* Input descriptors occupy [SD_BASE, SD_BASE + iss*0x20); the first
         * one is capture's, when there is one at all. GCAP reporting 0 input
         * streams is a real controller shape (an output-only HDA function),
         * not a bug to work around -- capture is simply impossible on it, and
         * probe() below says so rather than treating in_base as valid anyway. */
        h->in_base = SD_BASE;
        if (!iss)
            kprintf("[hda] %s: GCAP reports 0 input streams -- capture impossible on this controller\n",
                    dev->name);
    }

    statests = r16(h, STATESTS);
    if (!statests) {
        kprintf("[hda] %s: no codec responded (STATESTS=0)\n", dev->name);
        goto probe_fail;
    }
    for (i = 0; i < 15; i++) if (statests & (1u << i)) { h->codec_addr = i; break; }

    /* CORB/RIRB. One page each: 256 x 4 and 256 x 8 both fit in 4 KiB, and a
     * PMM page is 4 KiB-aligned, which covers the 128-byte alignment the spec
     * requires for both rings. */
    h->corb_dma = dma_alloc_coherent(&h->dma, 4096, 4096, 0);
    h->rirb_dma = dma_alloc_coherent(&h->dma, 4096, 4096, 0);
    if (!h->corb_dma || !h->rirb_dma) {
        kprintf("[hda] no memory for CORB/RIRB\n"); goto probe_fail;
    }
    h->corb = h->corb_dma->cpu;
    h->rirb = h->rirb_dma->cpu;
    if (hda_set_ring_size(h, CORBSIZE, &h->corb_entries) != 0 ||
        hda_set_ring_size(h, RIRBSIZE, &h->rirb_entries) != 0) {
        kprintf("[hda] %s: no supported/readable CORB or RIRB size\n",
                dev->name);
        goto probe_fail;
    }
    if (!dma_buffer_submit(h->corb_dma) ||
        !dma_buffer_submit(h->rirb_dma)) {
        kprintf("[hda] %s: CORB/RIRB DMA ownership failed\n", dev->name);
        goto probe_fail;
    }
    dma_wmb();
    uint64_t corb_addr = dma_addr_value(h->corb_dma->dma);
    uint64_t rirb_addr = dma_addr_value(h->rirb_dma->dma);
    w32(h, CORBLBASE, (uint32_t)corb_addr);
    w32(h, CORBUBASE, (uint32_t)(corb_addr >> 32));
    w32(h, RIRBLBASE, (uint32_t)rirb_addr);
    w32(h, RIRBUBASE, (uint32_t)(rirb_addr >> 32));

    /* Reset the read pointer: set bit 15, wait for it to read back, clear it. */
    w16(h, CORBRP, 0x8000);
    udelay(100);
    w16(h, CORBRP, 0);
    w16(h, CORBWP, 0);
    w16(h, RIRBWP, 0x8000);                /* write-1-to-reset, self-clearing */
    /* 0xFF, not 1: see the ack note in codec_cmd. With RINTCNT=1 the
     * controller stops fetching after every single response until the driver
     * acks, which turns a polled command bus into exactly one working verb. */
    w16(h, RINTCNT, 0xFF);
    h->corb_wp = 0;
    h->rirb_rp = 0;

    /* All DMA addresses and ring geometry are now valid. This is the first
     * point at which PCI bus mastering may be restored. */
    if (hda_pci_master_enable(dev) != 0) goto probe_fail;
    w8(h, CORBCTL, HDA_RUN);                /* RUN */
    w8(h, RIRBCTL, HDA_RUN);                /* RUN, response interrupts off:
                                            * we poll RIRBWP inside codec_cmd,
                                            * which is bounded and only runs at
                                            * probe time. */
    udelay(100);
    if (!(r8(h, CORBCTL) & HDA_RUN) || !(r8(h, RIRBCTL) & HDA_RUN)) {
        kprintf("[hda] %s: CORB/RIRB RUN state did not latch\n", dev->name);
        goto probe_fail;
    }

    if (find_output_path(h) != 0) {
        kprintf("[hda] %s: codec %u exposes no usable output path\n",
                dev->name, h->codec_addr);
        goto probe_fail;
    }

    hda_report_jack(h, "output", h->pin_nid);
    if (h->adc_nid)
        hda_report_jack(h, "capture", h->cap_pin_nid);
    else if (iss)
        kprintf("[hda] %s: codec %u exposes no usable capture path -- input disabled\n",
                dev->name, h->codec_addr);
    /* (iss == 0 already reported above, at the GCAP read.) */

    {
        uint32_t vid = codec_param(h, 0, PARAM_VENDOR);
        char *p = h->snd.codec;
        char *end = h->snd.codec + sizeof h->snd.codec - 1;
        p = put_str(p, end, "codec");
        p = put_dec(p, end, h->codec_addr);
        p = put_str(p, end, " ");
        p = put_hex4(p, end, (unsigned)(vid >> 16));
        p = put_str(p, end, ":");
        p = put_hex4(p, end, (unsigned)(vid & 0xFFFF));
        p = put_str(p, end, " dac=");
        p = put_dec(p, end, h->dac_nid);
        p = put_str(p, end, " pin=");
        p = put_dec(p, end, h->pin_nid);
        *p = 0;
    }

    if (h->adc_nid) {
        uint32_t vid = codec_param(h, 0, PARAM_VENDOR);
        char *p = h->cap.codec;
        char *end = h->cap.codec + sizeof h->cap.codec - 1;
        p = put_str(p, end, "codec");
        p = put_dec(p, end, h->codec_addr);
        p = put_str(p, end, " ");
        p = put_hex4(p, end, (unsigned)(vid >> 16));
        p = put_str(p, end, ":");
        p = put_hex4(p, end, (unsigned)(vid & 0xFFFF));
        p = put_str(p, end, " adc=");
        p = put_dec(p, end, h->adc_nid);
        p = put_str(p, end, " pin=");
        p = put_dec(p, end, h->cap_pin_nid);
        *p = 0;
    }

    codec_setup_output(h);
    if (h->adc_nid) codec_setup_capture(h);

    /* The PCM ring the engine walks, and the BDL that describes it. Contiguous
     * because a BDL entry is a physical extent -- a ring stitched from scattered
     * pages would need one entry per page and would work, but the mixer wants a
     * flat buffer it can index. */
    {
        h->ring_dma = dma_alloc_coherent(&h->dma, HDA_RING_BYTES, 4096, 0);
        h->bdl_dma = dma_alloc_coherent(&h->dma, 4096, 4096, 0);
        if (!h->ring_dma || !h->bdl_dma) {
            kprintf("[hda] no memory for the DMA ring\n"); goto probe_fail;
        }
        h->ring = h->ring_dma->cpu;
        h->bdl = h->bdl_dma->cpu;
    }

    /* Capture's own ring + BDL, same shape and same contiguity argument as
     * the output ring above -- allocated only when a capture path exists, so
     * a machine (or a controller) with none pays nothing extra. A failure
     * here disables capture rather than failing the whole probe: playback is
     * the mandatory half of this driver (see the `return -1` above this
     * block on the output ring), capture is additive. */
    if (h->adc_nid) {
        h->cap_ring_dma = dma_alloc_coherent(&h->dma, HDA_RING_BYTES, 4096, 0);
        h->cap_bdl_dma = dma_alloc_coherent(&h->dma, 4096, 4096, 0);
        if (!h->cap_ring_dma || !h->cap_bdl_dma) {
            /* Neither address was published; release a partial allocation. */
            if (h->cap_ring_dma) dma_free_coherent(h->cap_ring_dma);
            if (h->cap_bdl_dma) dma_free_coherent(h->cap_bdl_dma);
            h->cap_ring_dma = h->cap_bdl_dma = NULL;
            kprintf("[hda] %s: no memory for capture DMA -- capture disabled\n", dev->name);
            h->adc_nid = 0;
        } else {
            h->cap_ring = h->cap_ring_dma->cpu;
            h->cap_bdl = h->cap_bdl_dma->cpu;
        }
    }

    h->snd.name = "hda";
    h->snd.rate = HDA_RATE;
    h->snd.channels = HDA_CHANNELS;
    h->snd.format = SND_FMT_S16;
    h->snd.period_bytes = HDA_PERIOD_BYTES;
    h->snd.periods = HDA_PERIODS;
    h->snd.ring = h->ring;
    h->snd.start = hda_start;
    h->snd.stop = hda_stop;
    h->snd.position = hda_position;
    h->snd.priv = h;

    if (h->adc_nid && h->cap_ring) {
        h->cap.name = "hda-in";
        h->cap.rate = HDA_RATE;
        h->cap.channels = HDA_CHANNELS;
        h->cap.format = SND_FMT_S16;
        h->cap.period_bytes = HDA_PERIOD_BYTES;
        h->cap.periods = HDA_PERIODS;
        h->cap.ring = h->cap_ring;
        h->cap.start = hda_cap_start;
        h->cap.stop = hda_cap_stop;
        h->cap.priv = h;
        h->has_capture = 1;
    }

    dev_set_drvdata(dev, h);

    if (snd_register_device(&h->snd) != 0) goto probe_fail;
    if (h->has_capture && snd_register_capture_device(&h->cap) != 0) {
        /* Registration failing here (format/geometry rejected, or a second
         * capture device already claimed) does not fail the probe -- exactly
         * the "capture is additive" posture the ring-allocation comment
         * above already established. The DMA engine for it is simply never
         * started (snd_cap_engine_start only runs from the first SYS_SND_CAP_OPEN,
         * and there is no open without a registered device to open against). */
        kprintf("[hda] %s: capture device registration failed -- input disabled\n",
                dev->name);
        h->has_capture = 0;
    }

    /* ONE shared interrupt for the whole controller -- there is only one
     * vector to wire either engine to, which is exactly why hda_isr checks
     * both stream indices unconditionally (see its comment).  Registration
     * deliberately precedes IRQ publication: after an IRQ exists there is no
     * fallible probe step left, so a failed teardown can never leave a live
     * callback pointing into an instance that probe() is about to clear. */
    if (dev_irq_request(dev, hda_isr, h, "hda") >= 0) {
        h->snd.irq_mode = dev->irq_mode;
        if (h->has_capture) h->cap.irq_mode = dev->irq_mode;
    } else {
        kprintf("[hda] %s: no interrupt could be wired -- refills would stall\n",
                dev->name);
    }
    /* Start the mixer here rather than from kmain: driver.h's whole point is
     * that adding a driver requires editing no other file.
     *
     * The second half of that comment used to read "and dev_probe_all() runs
     * late enough in boot that thread_create() is available". It does not:
     * kmain.c calls dev_probe_all() and only then wm_run(), which is what calls
     * sched_init(). snd_init() therefore allocates here and defers the DMA
     * engine and the kaudio thread to the first stream open. Nothing in this
     * driver changes; hda_start() is pure MMIO and is called from wherever the
     * mixer decides it is safe to call it. */
    snd_init();
    return 0;

probe_fail:
    hda_probe_abort(h, dev);
    return -1;
}

/* Match by class so the policy above can inspect the complete enumerated PCI
 * topology.  Matching a GPU's HDMI function is harmless because probe declines
 * it without enabling or mapping it; restricting the table to one Intel ID
 * would instead discard the generic non-display HDA fallback. */
/* Existing device-model unbind, not a hotplug framework. Detaching upper
 * pointers under their worker locks makes subsequent DMA-buffer access
 * impossible even when hardware reset fails and the memory is quarantined. */
static void hda_remove(struct device *dev)
{
    struct hda *h = &g_hda;
    if (!h->mmio || __atomic_exchange_n(&h->removing, 1, __ATOMIC_ACQ_REL)) return;
    /* dev_unbind normally drains the IRQ before calling remove().  Keep this
     * check for the direct diagnostic removal path as well: on failure the
     * callback can still run, so every object it can reach must stay live and
     * a later teardown must be allowed to retry. */
    w32(h, INTCTL, 0);
    if (dev_irq_release(dev) != 0) {
#ifndef HDA_X79_NEGCTL_REMOVE_AFTER_IRQ_RELEASE_FAIL
        __atomic_store_n(&h->removing, 0, __ATOMIC_RELEASE);
        kprintf("[hda] unbind IRQ teardown unconfirmed: instance retained\n");
        return;
#endif
    }
    snd_unregister_device(&h->snd);
    snd_unregister_capture_device(&h->cap);
    if (h->ring_dma) hda_stop(&h->snd);
    if (h->cap_ring_dma) hda_cap_stop(&h->cap);
    IO_GUARD(&hda_gate);
    if (hda_stop_all_dma(h) != 0) {
        dma_device_quarantine(&h->dma);
        dev_disable(dev);
        kprintf("[hda] unbind DMA stop unconfirmed: DMA quarantined\n");
        return;
    }
    w32(h, GCTL, 0);
    for (unsigned i = 0; i < 100 && (r32(h, GCTL) & 1); i++) udelay(100);
    if (r32(h, GCTL) & 1) {
        dma_device_quarantine(&h->dma);
        dev_disable(dev);
        kprintf("[hda] unbind reset unconfirmed: DMA quarantined\n");
        return;
    }
    dma_device_quiesced(&h->dma);
    while (h->dma.buffers) {
        if (dma_free_coherent(h->dma.buffers) != 0) break;
    }
    dev_set_drvdata(dev, NULL);
    dev_disable(dev);
    memset(h, 0, sizeof *h);
}

static const struct dev_match hda_ids[] = {
    DEV_MATCH_CLASS(0x04, 0x03),
    DEV_MATCH_END
};

static struct driver hda_driver = {
    .name     = "hda",
    .bus_type = DEV_BUS_PCI,
    .match    = hda_ids,
    .probe    = hda_probe,
    .remove   = hda_remove,
    .next     = NULL,
};
DRIVER_DECLARE(hda_driver);
