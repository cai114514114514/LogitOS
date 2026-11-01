#include "../core/io_lock.h"
/* Command rings and stream register RMW belong to this controller. The ISR
 * takes this short gate but never an audio worker/lifecycle owner. */
static io_lock_t hda_gate = IO_LOCK_INIT;
/* Intel High Definition Audio controller + codec.
 *
 * HDA is the right target because it is what real machines have: every PC
 * built since about 2004 has an HDA controller, the register map is a published
 * spec rather than a per-vendor secret, and QEMU emulates it faithfully enough
 * that the same code drives both. Earlier notes dismissed AC'97 as a dead end
 * and claimed an ac97.c fallback already existed. That file never existed.
 * The separate audio/ac97/ driver now provides the older-controller path.
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
#include "hda_route.h"
#include "hda_output_paths.h"
#include "hda_status.h"
/* The existing LOGIT_HOST_TEST register gates compile hda.c as one TU and
 * predate the route module. Include the SAME implementation for that seam;
 * the kernel's recursive source list compiles hda_route.c separately. This
 * avoids either editing another track's fixtures or growing a host-only port
 * that can drift away from the production graph and verb logic. The output
 * planner is pure and shares the seam for the same reason. */
#ifdef LOGIT_HOST_TEST
#include "hda_route.c"
#include "hda_output_paths.c"
#endif
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
/* HDA 1.0a 7.3.3.10, table 82: status is a separate GET, not the SET reply. */
#define VERB_GET_POWER        0xF05
#define HDA_POWER_ERROR       (1u << 8) /* PS-Error: requested transition refused */
#define HDA_POWER_STATE_MASK  0xffu     /* PS-Act[7:4] and PS-Set[3:0]; D0 is 0 */
/* Local probe policy, not a codec timing guarantee: allow 500 ms for D0.
 * Also cap status transactions at 16384 when IF=0 or the clock is frozen;
 * each transaction has codec_cmd's own transport bounds. No udelay/sleep:
 * udelay's clock-only loop could hang this very frozen-clock case. */
#define HDA_POWER_TIMEOUT_MS  500
#define HDA_POWER_MAX_READS   16384
#define VERB_SET_CONN_SELECT  0x701    /* choose which connection-list entry a
                                        * multi-source widget (an ADC, a mixer,
                                        * an input selector) actually listens
                                        * to right now */
#define VERB_GET_PIN_SENSE    0xF09    /* bit31 = a jack is physically present */
/* HDA 1.0a 7.3.3.15: a pin whose capabilities set Trigger Required only
 * measures presence after this verb (Linux hda_jack.c, read_pin_sense()). */
#define VERB_SET_PIN_SENSE    0x709

/* How often the jack state is re-read. Linux prefers unsolicited responses and
 * polls only a pin that cannot raise them (hda_jack.c, jackpoll_interval).
 * This driver does not enable unsolicited responses: codec_cmd treats every
 * RIRB entry as the answer to its own verb, so an unsolicited entry would be
 * misread as a reply. A plug event therefore waits at most this long before
 * the speakers mute: fast enough for a human, and cheap (one sense verb and
 * one read-back per output pin) for the audio worker that runs the poll. */
#define HDA_JACK_POLL_INTERVAL_MS  250

#define PARAM_VENDOR          0x00
#define PARAM_SUBNODE_COUNT   0x04
#define PARAM_FG_TYPE         0x05
#define PARAM_AUDIO_WIDGET_CAP 0x09
#define PARAM_PIN_CAP         0x0C
#define PARAM_AMP_IN_CAP      0x0D    /* HDA 1.0a Amplifier Capabilities */
#define PARAM_CONN_LIST_LEN   0x0E
#define PARAM_AMP_OUT_CAP     0x12    /* HDA 1.0a Amplifier Capabilities */

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
    struct hda_route output_route; /* proven path, including intermediate amps */

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

    /* --------------------------------------------- output planning -- */
    /* Every connected analog output pin with its converter path (see
     * hda_output_paths.h). output_route / pin_nid / dac_nid above always
     * describe the plan's first path, the one boot has relied on until now. */
    struct hda_output_plan output_plan;
    unsigned char     path_usable[HDA_OUTPUT_PATHS_MAX];         /* setup verbs all succeeded */
    enum hda_jack_state path_jack_state[HDA_OUTPUT_PATHS_MAX];   /* sense at the last poll */
    unsigned char     path_output_enabled[HDA_OUTPUT_PATHS_MAX]; /* pin control read back */
    int               jack_poll_running;     /* one poller at a time; a second skips */
    int               jack_poll_failed;      /* a verb timed out: stop polling for good */
    int               jack_polled_once;
    uint64_t          last_jack_poll_ms;

    /* ---------------------------------------------- panel diagnostics -- */
    uint32_t          codec_vendor_and_device;
    uint64_t          played_bytes;          /* monotonic, folded from LPIB */
    uint32_t          last_link_position;    /* LPIB at the last fold */
    uint64_t          panel_played_bytes;    /* byte count at the previous hda_status_get() */
    enum hda_codec_error last_error;
    unsigned          last_error_nid;
    const char       *bring_up_failure;      /* static text; why the command bus is down */
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

    /* The audio worker polls the jacks on its own schedule, so a verb can
     * arrive after remove() or a failed probe cleared this instance. Both
     * clear mmio (or set removing first); refuse instead of touching a ring
     * that no longer exists. */
    if (!h->mmio || __atomic_load_n(&h->removing, __ATOMIC_ACQUIRE)) {
        return -1;
    }

    /* HDA 1.0a section 7.3: a 12-bit verb carries eight payload bits; the
     * Converter Format (2) and Amplifier Gain/Mute (3) verbs carry sixteen.
     * Historical code chose by payload > 0xff and used verb & 0xf. Correction:
     * our constants are 0x200/0x300, so that mask sent verb ZERO for every
     * stereo amp write. Select by verb kind, shift the constant down, and
     * preserve the wide format even when its data happens to fit in a byte. */
    if (verb == VERB_SET_STREAM_FMT || verb == VERB_SET_AMP)
        val = (cad << 28) | (nid << 20) | ((verb >> 8) << 16) | (payload & 0xFFFF);
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
            h->last_error = HDA_CODEC_ERROR_COMMAND_TIMEOUT;
            h->last_error_nid = nid;
            return -1;
        }
        __asm__ volatile ("pause");
    }
    h->last_error = HDA_CODEC_ERROR_COMMAND_TIMEOUT;
    h->last_error_nid = nid;
    return -1;
}

static uint32_t codec_param(struct hda *h, unsigned nid, unsigned param)
{
    uint32_t v = 0;
    if (codec_cmd(h, h->codec_addr, nid, VERB_GET_PARAM, param, &v) != 0) return 0;
    return v;
}

/* Bring an AFG or power-capable output widget to proven D0 before using it.
 * The old SET_POWER-only sequence treated command delivery as success; a
 * codec may acknowledge it while staying in D3. HDA 1.0a 7.3.3.10 requires
 * checking PS-Error separately from the asynchronous PS-Act transition:
 * https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf
 * Refusal, unreadable status and bounded noncompletion all leave output
 * unavailable; retrying a refused transition or guessing D0 hides silence.
 * This polls CORB/RIRB without relying on interrupts, including boot at IF=0.
 * Model evidence: not verified on X79-VG4 hardware. */
static int hda_power_up(struct hda *h, unsigned nid)
{
    if (codec_cmd(h, h->codec_addr, nid, VERB_SET_POWER, 0, NULL) != 0) {
        kprintf("[hda] D0 request failed: nid %u\n", nid);
        return -1;
    }
#ifdef HDA_NEGCTL_NO_POWER_CONFIRM
    /* Reproduce the acknowledged-but-still-D3 bug for the integration gate. */
    return 0;
#else
    uint64_t started = hda_ms();
    uint32_t state = 0;

    for (unsigned reads = 0; reads < HDA_POWER_MAX_READS; reads++) {
        if (codec_cmd(h, h->codec_addr, nid, VERB_GET_POWER, 0, &state) != 0) {
            kprintf("[hda] D0 status unreadable: nid %u\n", nid);
            return -1;
        }
        if (state & HDA_POWER_ERROR) {
            kprintf("[hda] D0 refused: nid %u state %x\n", nid, state);
            h->last_error = HDA_CODEC_ERROR_POWER_REFUSED;
            h->last_error_nid = nid;
            return -1;
        }
        if (!(state & HDA_POWER_STATE_MASK)) {
            return 0;
        }
        if (hda_ms() - started >= HDA_POWER_TIMEOUT_MS) {
            break;
        }
        __asm__ volatile ("pause");
    }
    kprintf("[hda] D0 transition timed out: nid %u state %x\n", nid, state);
    h->last_error = HDA_CODEC_ERROR_POWER_TIMEOUT;
    h->last_error_nid = nid;
    return -1;
#endif
}

/* ------------------------------------------------------ codec discovery -- */

/* Historical description: shared by the output pin search below (which
 * already open-coded a short-form (4-per-response) connection list read) and
 * the capture search that follows it. Only the short form is read -- real
 * codecs in this tree's corpus so far (QEMU's, and every ALC datasheet this
 * driver was cross-checked against) never exceed a handful of sources, so 8
 * is generous rather than tight.
 * Correction (P7): capture alone retains these helpers unchanged. Output now
 * expands short/long/range entries with hda_read_output_connections below. */
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
    if (pcap & HDA_PIN_CAPABILITY_PRESENCE_DETECT) {
        uint32_t sense = 0;
        codec_cmd(h, h->codec_addr, nid, VERB_GET_PIN_SENSE, 0, &sense);
        kprintf("[hda] %s pin %u: jack %s\n", label, nid,
                (sense & HDA_PIN_SENSE_PRESENCE) ? "present" : "absent");
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
 * "stubbed to success" shape this tree forbids. Refuse instead.
 * Correction (P7): output now refuses an unproven graph too; the old contrast
 * above is retained for readers following the original fallback diagnosis. */
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

/* Read the output graph's connection list without changing capture's legacy
 * reader. HDA 1.0a Connection List Length / Get Connection List Entry tables
 * and Linux v6.12 sound/hda/hdac_device.c, snd_hdac_get_connections(): bit 7
 * selects long form, entries pack four bytes or two words per response, and
 * each entry's top bit expands a range AFTER the preceding NID.
 * https://github.com/torvalds/linux/blob/v6.12/sound/hda/hdac_device.c
 * Reject overflow/malformed lists completely: truncating a range changes
 * Connection Select indices and can enable the wrong mixer input silently. */
static int hda_read_output_connections(struct hda *h, struct hda_widget *widget)
{
    uint32_t length = 0;
    uint32_t packed = 0;
    unsigned previous = 0;

    widget->connection_count = 0;
    if (codec_cmd(h, h->codec_addr, widget->nid, VERB_GET_PARAM,
                  PARAM_CONN_LIST_LEN, &length) != 0) {
        return -1;
    }
    unsigned entry_bits = (length & 0x80) ? 16 : 8;
    unsigned per_response = 32 / entry_bits;
    unsigned range_bit = 1u << (entry_bits - 1);
    unsigned entry_mask = range_bit - 1;

    for (unsigned index = 0; index < (length & 0x7f); index++) {
        if (index % per_response == 0) {
            if (codec_cmd(h, h->codec_addr, widget->nid, VERB_GET_CONN_LIST,
                          index, &packed) != 0) {
                goto invalid;
            }
        }
        unsigned entry = packed >> ((index % per_response) * entry_bits);
        unsigned nid = entry & entry_mask;
        unsigned first = nid;

        /* Codec command NIDs are eight bits. Long-form entries are legal
         * only when they still fit this address space (HDA 1.0a 7.3). */
        if (nid == 0 || nid > 255) {
            goto invalid;
        }
        if (entry & range_bit) {
            if (index == 0 || nid <= previous) {
                goto invalid;
            }
            first = previous + 1;
        }
        for (unsigned source = first; source <= nid; source++) {
            if (widget->connection_count == HDA_ROUTE_CONNECTIONS) {
                goto invalid;
            }
            widget->connections[widget->connection_count++] = source;
        }
        previous = nid;
    }
    return 0;

invalid:
    widget->connection_count = 0;
    return -1;
}

/* Resolve amplifier inheritance before the pure search sees the graph. HDA
 * 1.0a Audio Widget Capabilities bit 3 (Amp Param Override) says whether the
 * widget supplies capabilities or inherits its AFG's defaults; Linux v6.12
 * sound/pci/hda/hda_codec.c, query_amp_caps(), follows the same rule.
 * A failed capability/list read invalidates only this output widget. Capture
 * keeps its existing independent search and never consumes this table. */
static int hda_read_output_caps(struct hda *h, struct hda_widget *widget,
                                uint32_t default_input, uint32_t default_output,
                                int input_ready, int output_ready)
{
    unsigned amp_node = (widget->caps & (1u << 3)) ? widget->nid : 0;

    if (widget->caps & (1u << 1)) {
        if (!amp_node && !input_ready) {
            return -1;
        }
        widget->amp_in = default_input;
        if (amp_node && codec_cmd(h, h->codec_addr, amp_node, VERB_GET_PARAM,
                                  PARAM_AMP_IN_CAP, &widget->amp_in) != 0) {
            return -1;
        }
    }
    if (widget->caps & (1u << 2)) {
        if (!amp_node && !output_ready) {
            return -1;
        }
        widget->amp_out = default_output;
        if (amp_node && codec_cmd(h, h->codec_addr, amp_node, VERB_GET_PARAM,
                                  PARAM_AMP_OUT_CAP, &widget->amp_out) != 0) {
            return -1;
        }
    }
    if (widget->type == WIDGET_PIN || widget->type == WIDGET_MIXER ||
        widget->type == WIDGET_SEL) {
        return hda_read_output_connections(h, widget);
    }
    return 0;
}

/* Make the plan's first path the driver's primary route and say, one line per
 * path, what was planned. The first path keeps feeding output_route / pin_nid /
 * dac_nid, the fields the capture code, the codec string and the older gates
 * read, so adding the other outputs changes nothing for them. */
static void hda_adopt_output_plan(struct hda *h)
{
    const struct hda_output_plan *plan = &h->output_plan;
    const struct hda_route *primary = &plan->paths[0].route;

    h->output_route = *primary;
    h->pin_nid = primary->nodes[0].nid;
    h->dac_nid = primary->nodes[primary->count - 1].nid;
    for (unsigned index = 0; index < plan->path_count; index++) {
        const struct hda_output_path *path = &plan->paths[index];
        const struct hda_route *route = &path->route;

        if (route->count == HDA_ROUTE_NODES) {
            kprintf("[hda]   output %s: pin 0x%x -> widget 0x%x -> DAC 0x%x%s, jack-detect %s\n",
                    hda_output_role_name(path->role), route->nodes[0].nid,
                    route->nodes[1].nid, route->nodes[2].nid,
                    path->converter_shared ? " (shared)" : "",
                    path->jack_detectable ? "yes" : "no");
        } else {
            kprintf("[hda]   output %s: pin 0x%x -> DAC 0x%x direct%s, jack-detect %s\n",
                    hda_output_role_name(path->role), route->nodes[0].nid,
                    route->nodes[1].nid,
                    path->converter_shared ? " (shared)" : "",
                    path->jack_detectable ? "yes" : "no");
        }
    }
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
    /* A full eight-bit NID table is too large for the kernel stack. Probe is
     * admitted through the existing g_hda singleton; keep only its selected
     * three-node route in h, and reuse this scratch table for the next AFG. */
    static struct hda_widget widgets[256];

    h->output_route.count = 0;
    h->output_plan.path_count = 0;
    h->pin_nid = 0;
    h->dac_nid = 0;

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
        uint32_t default_input = 0;
        uint32_t default_output = 0;
        int input_ready, output_ready;
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

        /* Historical assumption: sending D0 made later widget reads safe.
         * Correction: prove AFG D0 (HDA 1.0a 7.3.3.10) before reading the
         * graph; a rejected transition must not fabricate a usable output. */
        if (hda_power_up(h, fg) != 0) {
            return -1;
        }

        sub = codec_param(h, fg, PARAM_SUBNODE_COUNT);
        w_start = (sub >> 16) & 0xFF;
        w_count = sub & 0xFF;
        if (w_start + w_count > 256) {
            kprintf("[hda]   fg %u: widget range exceeds codec NID space\n", fg);
            continue;
        }
        memset(widgets, 0, sizeof widgets);
        /* HDA 1.0a Amplifier Capabilities: AFG defaults apply when a widget
         * does not advertise Amp Param Override. Refuse failed reads instead
         * of interpreting a timeout as a mute-only, zero-gain amplifier. */
        input_ready = codec_cmd(h, h->codec_addr, fg, VERB_GET_PARAM,
                                 PARAM_AMP_IN_CAP, &default_input) == 0;
        output_ready = codec_cmd(h, h->codec_addr, fg, VERB_GET_PARAM,
                                  PARAM_AMP_OUT_CAP, &default_output) == 0;

        for (j = 0; j < w_count; j++) {
            unsigned nid = w_start + j;
            unsigned type;
            struct hda_widget *widget = &widgets[j];
            int output_valid;

            cap = 0;
            output_valid = codec_cmd(h, h->codec_addr, nid, VERB_GET_PARAM,
                                      PARAM_AUDIO_WIDGET_CAP, &cap) == 0;
            type = (cap >> 20) & 0xF;
            widget->nid = nid;
            widget->type = type;
            widget->caps = cap;
            if (type == WIDGET_DAC) {
                kprintf("[hda]   nid %u: DAC (cap %x)\n", nid, cap);
            } else if (type == WIDGET_ADC && nadc < 16) {
                adcs[nadc++] = nid;
                kprintf("[hda]   nid %u: ADC (cap %x)\n", nid, cap);
            } else if (type == WIDGET_PIN) {
                uint32_t pcap = 0;
                uint32_t cfg = 0;
                int connected;
                if (codec_cmd(h, h->codec_addr, nid, VERB_GET_PARAM,
                              PARAM_PIN_CAP, &pcap) != 0) {
                    output_valid = 0;
                }
                if (codec_cmd(h, h->codec_addr, nid, VERB_GET_CONFIG_DEF, 0, &cfg) != 0) {
                    output_valid = 0;
                }
                widget->pin_caps = pcap;
                widget->cfg = cfg;
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
                if (connected && (pcap & (1u << 5)) && ninpin < 16) inpins[ninpin++] = nid;
            } else {
                kprintf("[hda]   nid %u: type %u (cap %x)\n", nid, type, cap);
            }
            if (!output_valid ||
                hda_read_output_caps(h, widget, default_input, default_output,
                                      input_ready, output_ready) != 0) {
                /* Vendor-defined type F cannot be mistaken for a DAC or pin.
                 * Do not remove the node: indices refer to hardware lists. */
                widget->type = 0xf;
                kprintf("[hda]   nid %u: output capabilities/list unreadable or oversized\n", nid);
            }
        }

        /* Independent of the output check below: a fg with no usable output
         * pin can still have a working microphone, and the reverse. Guarded
         * so a second fg (rare, but the loop does not assume there is only
         * one AFG) does not overwrite a capture path an earlier fg already
         * proved. */
        if (!h->adc_nid)
            try_capture_path(h, adcs, nadc, inpins, ninpin);

        /* Route EVERY connected analog output, the way Linux does, so the
         * speakers are heard whichever jack they are on. Only when the pin
         * Configuration Defaults name no line-out, speaker or headphone does
         * the single best-route search below still decide. */
        if (hda_output_plan_build(widgets, w_count, &h->output_plan) > 0) {
            hda_adopt_output_plan(h);
            return 0;
        }

        /* The previous direct-only search fell back to the first DAC/pin.
         * Correction: QEMU's pin has a real connection-list edge too, while
         * ALC887 needs its summing mixer opened; neither warrants guessing. */
        if (hda_route_find(widgets, w_count, NULL, 0, &h->output_route) == 0) {
            hda_output_plan_from_single_route(&h->output_route, &h->output_plan);
            h->pin_nid = h->output_route.nodes[0].nid;
            h->dac_nid = h->output_route.nodes[h->output_route.count - 1].nid;
            if (h->output_route.count == 3) {
                kprintf("[hda]   output: pin %u -> widget %u -> DAC %u\n",
                        h->pin_nid, h->output_route.nodes[1].nid, h->dac_nid);
            } else {
                kprintf("[hda]   output: pin %u -> DAC %u direct\n",
                        h->pin_nid, h->dac_nid);
            }
            return 0;
        }
        kprintf("[hda]   fg %u: no reachable output DAC -- refusing to guess\n", fg);
    }
    return -1;
}

/* ----------------------------------------------- jacks and auto-mute --- */

/* Fold the output stream's Link Position In Buffer into a monotonic byte count.
 * The register wraps at the cyclic buffer length (HDA 1.0a, Stream Descriptor
 * LPIB), so a sampler must look at least once per lap of the ring (170 ms for
 * 32 KiB at 48 kHz stereo); the audio worker's housekeeping does, every couple
 * of periods. A count that only ever grows is what snd_device.position() has
 * always promised and what the panel needs to show "bytes played". The caller
 * holds hda_gate. */
static void hda_fold_link_position(struct hda *h)
{
    uint32_t position;
    uint32_t advance;

    if (!h->mmio) {
        return;
    }
    position = r32(h, h->out_base + SD_LPIB) % HDA_RING_BYTES;
    advance = position >= h->last_link_position
                  ? position - h->last_link_position
                  : position + HDA_RING_BYTES - h->last_link_position;
    h->played_bytes += advance;
    h->last_link_position = position;
}

/* Read one pin's presence-detect line the way Linux does (hda_jack.c,
 * read_pin_sense): trigger a measurement first when Pin Capabilities demand it,
 * then GET_PIN_SENSE bit 31. A pin without usable detection answers NO_DETECT
 * without any traffic. A verb that does not complete sets *transport_failed so
 * the caller can stop polling instead of stalling every 250 ms on a dead bus. */
static enum hda_jack_state hda_read_jack_state(struct hda *h, const struct hda_widget *pin,
                                               int *transport_failed)
{
    uint32_t sense = 0;

    if (!hda_pin_jack_detectable(pin)) {
        return HDA_JACK_NO_DETECT;
    }
    if (pin->pin_caps & HDA_PIN_CAPABILITY_TRIGGER_REQUIRED) {
        if (codec_cmd(h, h->codec_addr, pin->nid, VERB_SET_PIN_SENSE, 0, NULL) != 0) {
            *transport_failed = 1;
            return HDA_JACK_NO_DETECT;
        }
    }
    if (codec_cmd(h, h->codec_addr, pin->nid, VERB_GET_PIN_SENSE, 0, &sense) != 0) {
        *transport_failed = 1;
        return HDA_JACK_NO_DETECT;
    }
    return (sense & HDA_PIN_SENSE_PRESENCE) ? HDA_JACK_PLUGGED : HDA_JACK_EMPTY;
}

/* Make one pin drive (or stop driving) and read back what the codec holds.
 * The read-back is the truth the panel shows; a write that did not stick is
 * visible there instead of hiding behind a driver variable. Only the output
 * and headphone enable bits are compared, because a pin's other control bits
 * (input enable, VREF) are not this code's business. Returns 0, or -1 when a
 * verb did not complete. */
static int hda_apply_pin_control(struct hda *h, unsigned index, int enabled)
{
    const struct hda_output_path *path = &h->output_plan.paths[index];
    unsigned pin_nid = path->route.nodes[0].nid;
    unsigned wanted = enabled ? hda_output_path_pin_control(path) : HDA_PIN_CONTROL_ALL_OUTPUTS_OFF;
    unsigned compared_bits = HDA_PIN_CONTROL_OUTPUT_ENABLE | HDA_PIN_CONTROL_HEADPHONE_ENABLE;
    uint32_t current = 0;

    if (codec_cmd(h, h->codec_addr, pin_nid, VERB_GET_PIN_CTL, 0, &current) != 0) {
        return -1;
    }
    if ((current & compared_bits) != (wanted & compared_bits)) {
        if (codec_cmd(h, h->codec_addr, pin_nid, VERB_SET_PIN_CTL, wanted, NULL) != 0) {
            return -1;
        }
        if (codec_cmd(h, h->codec_addr, pin_nid, VERB_GET_PIN_CTL, 0, &current) != 0) {
            return -1;
        }
    }
    h->path_output_enabled[index] = (current & HDA_PIN_CONTROL_OUTPUT_ENABLE) != 0;
    return 0;
}

/* Re-read every output jack and apply Linux's auto-mute: a plugged headphone
 * silences the line-outs and speakers (hda_generic.c, snd_hda_gen_update_outputs
 * and do_automute). Runs from thread context only, because it sends codec
 * verbs. `force` skips the interval check (used right after the pins are set
 * up). Two callers may race (audio worker and panel); the second one simply
 * skips, the first one's answer is as fresh as its own would have been. */
static void hda_poll_output_jacks(struct hda *h, int force)
{
    enum hda_jack_state jack_states[HDA_OUTPUT_PATHS_MAX];
    unsigned char output_enabled[HDA_OUTPUT_PATHS_MAX];
    const struct hda_output_plan *plan = &h->output_plan;
    int transport_failed = 0;

    if (!plan->path_count || h->jack_poll_failed || !h->mmio ||
        __atomic_load_n(&h->removing, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (!force && h->jack_polled_once &&
        hda_ms() - h->last_jack_poll_ms < HDA_JACK_POLL_INTERVAL_MS) {
        return;
    }
    if (__atomic_exchange_n(&h->jack_poll_running, 1, __ATOMIC_ACQ_REL)) {
        return;
    }

    for (unsigned index = 0; index < plan->path_count; index++) {
        jack_states[index] = h->path_usable[index]
                                 ? hda_read_jack_state(h, &plan->paths[index].route.nodes[0],
                                                       &transport_failed)
                                 : HDA_JACK_NO_DETECT;
    }
    if (transport_failed) {
        h->jack_poll_failed = 1;
        kprintf("[hda] jack sense verb timed out: jack polling and auto-mute disabled\n");
        __atomic_store_n(&h->jack_poll_running, 0, __ATOMIC_RELEASE);
        return;
    }
    hda_output_automute_decide(plan, jack_states, output_enabled);
    for (unsigned index = 0; index < plan->path_count; index++) {
        const struct hda_output_path *path = &plan->paths[index];

        if (jack_states[index] != h->path_jack_state[index] || !h->jack_polled_once) {
            kprintf("[hda] jack %s@0x%x: %s\n", hda_output_role_name(path->role),
                    path->route.nodes[0].nid, hda_jack_state_name(jack_states[index]));
        }
        h->path_jack_state[index] = jack_states[index];
        if (!h->path_usable[index]) {
            continue;
        }
        if (hda_apply_pin_control(h, index, output_enabled[index]) != 0) {
            h->jack_poll_failed = 1;
            kprintf("[hda] pin control verb timed out: jack polling and auto-mute disabled\n");
            break;
        }
    }
    h->jack_polled_once = 1;
    h->last_jack_poll_ms = hda_ms();
    __atomic_store_n(&h->jack_poll_running, 0, __ATOMIC_RELEASE);
}

/* SDnFMT / converter format: base 48 kHz, x1, /1, 16-bit, 2 channels. */
#define HDA_FMT_48K_S16_2CH  0x0011

/* Send one planned path's compiled verb sequence, stopping on transport or
 * power-state failure rather than registering a silent output. Power verbs are
 * barriers: prove each D0 before the compiler's stream/select/amp writes.
 * hda_route.c documents capability checks and the HDA/Linux sequence sources.
 * Every converter in the plan is told the SAME stream tag and first channel, so
 * all of them play the one stereo stream: a jack with a speaker on it is
 * audible whichever one it is (Linux would leave the surround converters on
 * silence when playing two channels; here the point is to be heard). */
static int hda_run_output_path(struct hda *h, const struct hda_output_path *path)
{
    struct hda_route_verb verbs[HDA_ROUTE_VERBS];
    int count = hda_output_path_verbs(path, HDA_FMT_48K_S16_2CH, HDA_STREAM_TAG,
                                      verbs, HDA_ROUTE_VERBS);

    if (count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (verbs[i].verb == VERB_SET_POWER) {
            if (hda_power_up(h, verbs[i].nid) != 0) {
                return -1;
            }
            continue;
        }
        if (codec_cmd(h, h->codec_addr, verbs[i].nid,
                      verbs[i].verb, verbs[i].payload, NULL) != 0) {
            kprintf("[hda] output setup failed: nid %u verb %x\n",
                    verbs[i].nid, verbs[i].verb);
            return -1;
        }
    }
    return 0;
}

/* Bring up every planned output path. The first path is the one boot has
 * always depended on, so its failure fails the setup exactly as before; a later
 * path that cannot be brought up is logged and left switched off, because one
 * dead jack must not take the working ones down with it. Afterwards the jacks
 * are read once so the pins start in their auto-muted state. */
static int codec_setup_output(struct hda *h)
{
    if (h->output_plan.path_count == 0 && h->output_route.count != 0) {
        hda_output_plan_from_single_route(&h->output_route, &h->output_plan);
    }
    if (h->output_plan.path_count == 0) {
        return -1;
    }
    for (unsigned index = 0; index < h->output_plan.path_count; index++) {
        h->path_usable[index] = 0;
        if (hda_run_output_path(h, &h->output_plan.paths[index]) == 0) {
            h->path_usable[index] = 1;
            continue;
        }
        if (index == 0) {
            return -1;
        }
        kprintf("[hda] output %s pin 0x%x could not be set up; left off\n",
                hda_output_role_name(h->output_plan.paths[index].role),
                h->output_plan.paths[index].route.nodes[0].nid);
        h->last_error = HDA_CODEC_ERROR_OUTPUT_SETUP_FAILED;
        h->last_error_nid = h->output_plan.paths[index].route.nodes[0].nid;
    }
    h->jack_polled_once = 0;
    hda_poll_output_jacks(h, 1);
    return 0;
}

/* Historical claim before P7: the capture-side twin of codec_setup_output.
 * Same verbs, opposite amp
 * direction bit, and one extra step output never needs: programming WHICH
 * source a multi-input ADC (or an intermediate selector) actually listens to,
 * because unlike a DAC -- which only ever has one thing to say -- an ADC can
 * have several possible inputs and defaults to an unspecified one.
 *
 * Correction (P7, 2026-11-01): output now also selects the pin's connection
 * and any intermediate selector in hda_route_verbs(). A mixer sums inputs
 * instead of selecting one, so output enables its chosen DAC input through
 * the indexed input amplifier. These are routing requirements, not an ADC-only
 * distinction (HDA 1.0a Connection Select / Amplifier Gain-Mute verb tables;
 * Linux v6.12 sound/pci/hda/hda_generic.c, snd_hda_activate_path()). */
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
        /* Historical claim before P7: b14 SET-INPUT instead of output's
         * b15 SET-OUTPUT is the whole
         * difference from codec_setup_output's amp write, and getting it
         * backwards is invisible for the exact reason this file's header
         * warns about generally: the verb completes, the register reads back
         * programmed, and only the direction that actually reaches the wire
         * is wrong. The index field (bits 11:8) selects which of the ADC's
         * several possible SOURCES this gain applies to on a multi-source
         * ADC -- leaving it 0 on a widget where cap_sel_index != 0 would
         * gain-stage a source that is not the one actually selected, and
         * leave the real one at its power-on default (frequently muted).
         *
         * Correction (P7, 2026-11-01): output now programs both directions:
         * b15 for output amplifiers and b14 for intermediate mixer/selector
         * input amplifiers. hda_route_verbs() uses each route edge's source
         * index and amplifier capabilities; changing the direction bit alone
         * cannot configure that path. Capture's writes below are unchanged
         * (HDA 1.0a Amplifier Gain-Mute verb table; Linux v6.12
         * sound/pci/hda/hda_generic.c, activate_amp_in()/activate_amp_out()). */
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
    /* The reset zeroed LPIB; restart the monotonic byte count with it. */
    h->played_bytes = 0;
    h->last_link_position = 0;

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

/* Frames played since start(), as snd_device.position() promises: monotonic.
 * The raw LPIB register wraps every ring lap (8192 frames), which is useless
 * for "did the engine move" over more than 170 ms; the folded byte count is
 * what the boot tone and the panel report. */
static uint64_t hda_position(struct snd_device *d)
{
    IO_GUARD(&hda_gate);
    struct hda *h = (struct hda *)d->priv;
    hda_fold_link_position(h);
    return h->played_bytes / (HDA_CHANNELS * 2u);
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
#define HDA_NVIDIA_VENDOR   0x10de
#define HDA_RUN             0x02

/* Linux v6.12 hda_intel.c azx_ids lists HDA_CPT/PBG/PPT as PCH controllers;
 * include/linux/pci_ids.h supplies these values. Previously only 1d20 was
 * preferred here, which let GPU audio take the singleton on a 6-series PCH.
 * https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/hda_intel.c
 * https://github.com/torvalds/linux/blob/v6.12/include/linux/pci_ids.h
 * Selection is host-model evidence: not verified on X79-VG4 hardware. */
static const uint16_t pch_hda_ids[] = {
    0x1c20, /* PCI_DEVICE_ID_INTEL_HDA_CPT: Cougar Point, 6/C200 series. */
    0x1d20, /* PCI_DEVICE_ID_INTEL_HDA_PBG: Patsburg, C600/X79. */
    0x1e20, /* PCI_DEVICE_ID_INTEL_HDA_PPT: Panther Point, 7 series. */
};

static int hda_pch_id(uint16_t id)
{
    for (unsigned i = 0; i < sizeof pch_hda_ids / sizeof pch_hda_ids[0]; i++)
        if (id == pch_hda_ids[i]) return 1;
    return 0;
}

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

static int hda_is_pch_onboard(const struct device *dev)
{
    /* These PCH generations expose HD Audio at 00:1b.0. Requiring
     * both the published ID and the integrated BDF prevents an unrelated
     * passthrough endpoint with a copied ID from becoming the boot-wide
     * single HDA instance. */
    return dev && dev->bus_type == DEV_BUS_PCI && dev->seg == 0 &&
           dev->bus == 0 && dev->slot == 0x1b && dev->func == 0 &&
           dev->vendor == HDA_INTEL_VENDOR && hda_pch_id(dev->device) &&
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

static int hda_display_audio(const struct device *dev)
{
    /* A partial PCI inventory need not contain the VGA sibling. The target
     * inventory in docs/superpowers/specs/2026-10-31-x79-vg4-bringup-design.md
     * identifies 10de:0fb9 as the GTX 1050 HDMI function.
     * Keep an explicit identity fallback for it, rather than rejecting all
     * NVIDIA HDA: older NVIDIA chipset audio can be an analogue controller.
     * HDMI/DP routing and ELD are deliberately deferred to phase 3. */
    return dev && ((dev->vendor == HDA_NVIDIA_VENDOR && dev->device == 0x0fb9) ||
                   hda_has_display_sibling(dev));
}

static int hda_pch_onboard_present(void)
{
    for (int i = 0; i < dev_count(); i++)
        if (hda_is_pch_onboard(dev_at(i))) return 1;
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
    if (hda_display_audio(dev)) return 0;
    if (!hda_is_pch_onboard(dev) && hda_pch_onboard_present()) return 0;
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

/* One record per HDA PCI function the driver has been offered, kept after the
 * probe finishes so the panel can say WHY a function is bound or not, even once
 * the controller instance has been released (the display function is released
 * right after it has been asked who its codec is). Keyed by PCI location, so a
 * second attempt on the same function overwrites its first. */
#define HDA_FUNCTION_RECORDS_MAX     4

struct hda_function_record {
    int attempted;
    int bound;
    int is_display_function;
    unsigned bus;
    unsigned slot;
    unsigned function;
    int codec_present;
    uint32_t codec_vendor_and_device;
    unsigned display_pins;               /* display function only: pin widgets found */
    unsigned display_pins_with_sink;     /* ... of which report a sink attached */
    const char *failure_reason;          /* static text; NULL while bound */
};

static struct hda_function_record g_function_records[HDA_FUNCTION_RECORDS_MAX];

/* Does this record describe this PCI function? Segment is ignored: the
 * panel, like the boot log, names functions as bus:slot.function. */
static int hda_function_record_matches(const struct hda_function_record *record,
                                       const struct device *dev)
{
    return record->attempted && record->bus == dev->bus &&
           record->slot == dev->slot && record->function == dev->func;
}

/* Find the record for this PCI function, or claim a free slot (the last one is
 * recycled when the table is full: more HDA functions than this are not
 * expected, and losing the oldest line is better than losing the newest).
 * The record is reset on every call so a retry starts from a clean slate. */
static struct hda_function_record *hda_function_record_for(const struct device *dev,
                                                           int is_display_function)
{
    struct hda_function_record *record = &g_function_records[HDA_FUNCTION_RECORDS_MAX - 1];

    for (unsigned index = 0; index < HDA_FUNCTION_RECORDS_MAX; index++) {
        struct hda_function_record *candidate = &g_function_records[index];

        if (!candidate->attempted || hda_function_record_matches(candidate, dev)) {
            record = candidate;
            break;
        }
    }
    memset(record, 0, sizeof *record);
    record->attempted = 1;
    record->is_display_function = is_display_function;
    record->bus = dev->bus;
    record->slot = dev->slot;
    record->function = dev->func;
    return record;
}

/* The audio worker calls this every few periods (snd_device.housekeeping),
 * from thread context with the mixer lock held, so it may send codec verbs.
 * It keeps the played-byte count fresh enough that LPIB never wraps unseen
 * and re-reads the jacks at most every HDA_JACK_POLL_INTERVAL_MS. */
static void hda_housekeeping(struct snd_device *device)
{
    struct hda *h = (struct hda *)device->priv;

    if (!h || !h->mmio || __atomic_load_n(&h->removing, __ATOMIC_ACQUIRE)) {
        return;
    }
    {
        IO_GUARD(&hda_gate);
        hda_fold_link_position(h);
    }
    hda_poll_output_jacks(h, 0);
}

/* GCAP (HDA 1.0a 3.3.2): bit 0 is 64OK, bits 11:8 count input stream
 * descriptors and bits 15:12 output ones. Stream descriptors are laid out
 * back to back, SD_STRIDE bytes apart, inputs first (3.3.38 onward). */
#define GCAP_64BIT_ADDRESSING        0x1u
#define GCAP_INPUT_STREAMS_SHIFT     8
#define GCAP_OUTPUT_STREAMS_SHIFT    12
#define GCAP_STREAM_COUNT_MASK       0xfu
#define SD_STRIDE                    0x20u

/* The ring base registers come in lower and upper halves (HDA 1.0a 3.3.18,
 * 3.3.24): the upper dword holds address bits 63:32. */
#define ADDRESS_UPPER_DWORD_SHIFT    32

/* Parameter 00h, Vendor/Device ID (HDA 1.0a 7.3.4.1): vendor in 31:16, device
 * in 15:0. */
#define CODEC_VENDOR_ID_SHIFT        16
#define CODEC_DEVICE_ID_MASK         0xffffu

/* CORB and RIRB each live in one page-aligned page: 256 x 4 and 256 x 8 bytes
 * both fit in 4 KiB, and 4 KiB alignment covers the 128-byte alignment the
 * specification requires of both rings (HDA 1.0a 3.3.18, 3.3.24). */
#define HDA_COMMAND_RING_BYTES       4096u

/* CORBRP bit 15 resets the read pointer; RIRBWP bit 15 resets the write
 * pointer (HDA 1.0a 3.3.21, 3.3.26). RINTCNT is set to 0xFF so the response
 * counter can never reach the point where the controller stops fetching
 * commands (see the ack note in codec_cmd). */
#define CORBRP_RESET                 0x8000u
#define RIRBWP_RESET                 0x8000u
#define RINTCNT_NEVER_STALLS         0xFFu

/* Controller reset handshakes poll up to this many times, HDA_RESET_POLL_US
 * apart: ten milliseconds, far above the 521 us the specification allows a
 * codec to take (HDA 1.0a 4.3) and short enough that a dead controller fails
 * the probe instead of the boot. */
#define HDA_RESET_POLL_LIMIT         100u
#define HDA_RESET_POLL_US            100u

/* After CRST rises, codecs need time on the link before STATESTS says anything:
 * reading it immediately finds zero codecs on hardware that has several. */
#define HDA_LINK_SETTLE_US           1000u

/* A display codec can take longer than the analog one to show up in STATESTS
 * after reset (it sits behind the GPU's own power sequencing). Waiting up to
 * this many polls of HDA_RESET_POLL_US each, 100 ms in total, costs nothing on
 * a healthy board: the loop ends on the first poll that sees a codec. */
#define HDA_CODEC_WAKE_POLL_LIMIT    1000u
#define HDA_CODEC_ADDRESS_LIMIT      15u

/* Take a mapped-and-memory-only controller from firmware state to "verbs can
 * flow": quiesce stale DMA, reset, find the first codec, and start CORB and
 * RIRB with bus mastering restored only after every ring address is
 * programmed. Shared by the analog probe and the enumerate-only display probe
 * so both obey the identical ownership rules (the X79 ownership gates pin
 * them). Returns 0, or -1 with h->bring_up_failure naming the step; the caller
 * owns the cleanup (hda_probe_abort). `input_streams` receives GCAP's input
 * stream count. require_output_stream is 0 for a display function, which is
 * only asked who its codec is. */
static int hda_start_command_bus(struct hda *h, struct device *dev,
                                 int require_output_stream, unsigned *input_streams)
{
    uint64_t bar = dev_bar_map(dev, 0);
    uint16_t capabilities;
    uint16_t codecs_present;
    unsigned output_streams;
    uint64_t corb_address;
    uint64_t rirb_address;

    if (!bar) {
        kprintf("[hda] %s: no BAR0\n", dev->name);
        h->bring_up_failure = "BAR0 could not be mapped";
        return -1;
    }
    h->mmio = (volatile uint8_t *)(uintptr_t)bar;
    h->dev = dev;

    /* Firmware can leave command rings and any advertised stream running.
     * PCI BME is confirmed low above, so stale physical pointers cannot fetch
     * while each RUN bit is cleared and read back. CRST is legal only after
     * every engine reports stopped. */
    if (hda_stop_all_dma(h) != 0) {
        h->bring_up_failure = "firmware DMA did not stop";
        return -1;
    }

    /* Controller reset: CRST low, wait for it to read back low, then high and
     * wait for it to read back high. Both directions must be OBSERVED. */
    w32(h, GCTL, 0);
    for (unsigned poll = 0; poll < HDA_RESET_POLL_LIMIT && (r32(h, GCTL) & 1); poll++) {
        udelay(HDA_RESET_POLL_US);
    }
    if (r32(h, GCTL) & 1) {
        kprintf("[hda] %s: controller did not enter reset\n", dev->name);
        h->bring_up_failure = "controller did not enter reset";
        return -1;
    }
    w32(h, GCTL, 1);
    for (unsigned poll = 0; poll < HDA_RESET_POLL_LIMIT && !(r32(h, GCTL) & 1); poll++) {
        udelay(HDA_RESET_POLL_US);
    }
    if (!(r32(h, GCTL) & 1)) {
        kprintf("[hda] %s: controller did not come out of reset\n", dev->name);
        h->bring_up_failure = "controller did not leave reset";
        return -1;
    }
    udelay(HDA_LINK_SETTLE_US);

    capabilities = r16(h, GCAP);
    dma_device_init(&h->dma, "hda", (capabilities & GCAP_64BIT_ADDRESSING) ? DMA_MASK_64 : DMA_MASK_32);
    *input_streams = (capabilities >> GCAP_INPUT_STREAMS_SHIFT) & GCAP_STREAM_COUNT_MASK;
    output_streams = (capabilities >> GCAP_OUTPUT_STREAMS_SHIFT) & GCAP_STREAM_COUNT_MASK;
    if (!output_streams && require_output_stream) {
        kprintf("[hda] %s: no output streams\n", dev->name);
        h->bring_up_failure = "controller has no output stream";
        return -1;
    }
    h->out_base = SD_BASE + *input_streams * SD_STRIDE;    /* input descriptors come first */
    /* Input descriptors occupy [SD_BASE, SD_BASE + input_streams * SD_STRIDE);
     * the first one is capture's, when there is one at all. GCAP reporting 0
     * input streams is a real controller shape (an output-only HDA function),
     * not a bug to work around -- capture is simply impossible on it, and
     * probe() says so rather than treating in_base as valid anyway. */
    h->in_base = SD_BASE;
    if (!*input_streams) {
        kprintf("[hda] %s: GCAP reports 0 input streams -- capture impossible on this controller\n",
                dev->name);
    }

    codecs_present = r16(h, STATESTS);
    for (unsigned poll = 0; poll < HDA_CODEC_WAKE_POLL_LIMIT && !codecs_present; poll++) {
        udelay(HDA_RESET_POLL_US);
        codecs_present = r16(h, STATESTS);
    }
    if (!codecs_present) {
        kprintf("[hda] %s: no codec responded (STATESTS=0)\n", dev->name);
        h->bring_up_failure = "no codec responded after reset (STATESTS=0)";
        return -1;
    }
    for (unsigned address = 0; address < HDA_CODEC_ADDRESS_LIMIT; address++) {
        if (codecs_present & (1u << address)) {
            h->codec_addr = address;
            break;
        }
    }

    h->corb_dma = dma_alloc_coherent(&h->dma, HDA_COMMAND_RING_BYTES, HDA_COMMAND_RING_BYTES, 0);
    h->rirb_dma = dma_alloc_coherent(&h->dma, HDA_COMMAND_RING_BYTES, HDA_COMMAND_RING_BYTES, 0);
    if (!h->corb_dma || !h->rirb_dma) {
        kprintf("[hda] no memory for CORB/RIRB\n");
        h->bring_up_failure = "no memory for the command rings";
        return -1;
    }
    h->corb = h->corb_dma->cpu;
    h->rirb = h->rirb_dma->cpu;
    if (hda_set_ring_size(h, CORBSIZE, &h->corb_entries) != 0 ||
        hda_set_ring_size(h, RIRBSIZE, &h->rirb_entries) != 0) {
        kprintf("[hda] %s: no supported/readable CORB or RIRB size\n", dev->name);
        h->bring_up_failure = "command ring size not supported";
        return -1;
    }
    if (!dma_buffer_submit(h->corb_dma) || !dma_buffer_submit(h->rirb_dma)) {
        kprintf("[hda] %s: CORB/RIRB DMA ownership failed\n", dev->name);
        h->bring_up_failure = "command ring DMA ownership failed";
        return -1;
    }
    dma_wmb();
    corb_address = dma_addr_value(h->corb_dma->dma);
    rirb_address = dma_addr_value(h->rirb_dma->dma);
    w32(h, CORBLBASE, (uint32_t)corb_address);
    w32(h, CORBUBASE, (uint32_t)(corb_address >> ADDRESS_UPPER_DWORD_SHIFT));
    w32(h, RIRBLBASE, (uint32_t)rirb_address);
    w32(h, RIRBUBASE, (uint32_t)(rirb_address >> ADDRESS_UPPER_DWORD_SHIFT));

    /* Reset the read pointer: set the reset bit, wait for it to read back,
     * clear it. */
    w16(h, CORBRP, CORBRP_RESET);
    udelay(HDA_RESET_POLL_US);
    w16(h, CORBRP, 0);
    w16(h, CORBWP, 0);
    w16(h, RIRBWP, RIRBWP_RESET);          /* write-1-to-reset, self-clearing */
    w16(h, RINTCNT, RINTCNT_NEVER_STALLS);
    h->corb_wp = 0;
    h->rirb_rp = 0;

    /* All DMA addresses and ring geometry are now valid. This is the first
     * point at which PCI bus mastering may be restored. */
    if (hda_pci_master_enable(dev) != 0) {
        h->bring_up_failure = "PCI bus-master enable unconfirmed";
        return -1;
    }
    w8(h, CORBCTL, HDA_RUN);
    w8(h, RIRBCTL, HDA_RUN);               /* RUN, response interrupts off: we
                                            * poll RIRBWP inside codec_cmd,
                                            * which is bounded. */
    udelay(HDA_RESET_POLL_US);
    if (!(r8(h, CORBCTL) & HDA_RUN) || !(r8(h, RIRBCTL) & HDA_RUN)) {
        kprintf("[hda] %s: CORB/RIRB RUN state did not latch\n", dev->name);
        h->bring_up_failure = "command ring RUN did not latch";
        return -1;
    }
    return 0;
}

static int hda_probe(struct device *dev)
{
    struct hda *h = &g_hda;
    struct hda_function_record *record;
    unsigned iss = 0;

    if (!hda_controller_candidate(dev)) {
        if (hda_display_audio(dev)) {
            kprintf("[hda] %s: GPU audio function: left to the enumerate-only display driver\n",
                    dev->name);
        } else if (hda_pch_onboard_present()) {
            kprintf("[hda] %s: deferred to Intel PCH HDA at 00:1b.0\n",
                    dev->name);
        }
        return -1;
    }
    if (h->mmio) {
        return -1;          /* one output device; see snd_register_device */
    }
    record = hda_function_record_for(dev, 0);

    if (hda_pci_mem_only(dev) != 0) {
        record->failure_reason = "PCI memory-only state unconfirmed";
        return -1;
    }

    if (hda_start_command_bus(h, dev, 1, &iss) != 0) {
        goto probe_fail;
    }

    if (find_output_path(h) != 0) {
        kprintf("[hda] %s: codec %u exposes no usable output path\n",
                dev->name, h->codec_addr);
        h->bring_up_failure = "codec exposes no usable output path";
        goto probe_fail;
    }

    if (h->adc_nid)
        hda_report_jack(h, "capture", h->cap_pin_nid);
    else if (iss)
        kprintf("[hda] %s: codec %u exposes no usable capture path -- input disabled\n",
                dev->name, h->codec_addr);
    /* (iss == 0 already reported above, at the GCAP read.) */

    {
        uint32_t vid = codec_param(h, 0, PARAM_VENDOR);
        char *p = h->snd.codec;
        h->codec_vendor_and_device = vid;
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

    if (codec_setup_output(h) != 0) {
        h->bring_up_failure = "output path setup failed";
        goto probe_fail;
    }
    if (h->adc_nid) codec_setup_capture(h);

    /* The PCM ring the engine walks, and the BDL that describes it. Contiguous
     * because a BDL entry is a physical extent -- a ring stitched from scattered
     * pages would need one entry per page and would work, but the mixer wants a
     * flat buffer it can index. */
    {
        h->ring_dma = dma_alloc_coherent(&h->dma, HDA_RING_BYTES, 4096, 0);
        h->bdl_dma = dma_alloc_coherent(&h->dma, 4096, 4096, 0);
        if (!h->ring_dma || !h->bdl_dma) {
            kprintf("[hda] no memory for the DMA ring\n");
            h->bring_up_failure = "no memory for the playback ring";
            goto probe_fail;
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
    h->snd.housekeeping = hda_housekeeping;

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

    if (snd_register_device(&h->snd) != 0) {
        h->bring_up_failure = "sound device registration refused";
        goto probe_fail;
    }
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
    record->bound = 1;
    record->codec_present = 1;
    record->codec_vendor_and_device = h->codec_vendor_and_device;
    return 0;

probe_fail:
    record->failure_reason = h->bring_up_failure ? h->bring_up_failure : "probe failed";
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
    for (unsigned index = 0; index < HDA_FUNCTION_RECORDS_MAX; index++) {
        if (hda_function_record_matches(&g_function_records[index], dev)) {
            g_function_records[index].bound = 0;
            g_function_records[index].failure_reason = "unbound";
        }
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

/* ------------------------------------------------ GPU audio functions --- */

/* A GPU's HDMI/DP audio function is a second HD Audio controller with its own
 * codec. It gets its own instance, separate from the analog one, because the
 * analog controller is a boot-wide singleton that owns the sound device. */
static struct hda g_hda_display;

/* HDA 1.0a 7.3.4.6: Audio Widget Capabilities bits 23:20 are the widget type. */
#define AUDIO_WIDGET_CAPABILITY_TYPE_SHIFT   20
#define AUDIO_WIDGET_CAPABILITY_TYPE_MASK    0xfu

/* Audio Function Group identity (HDA 1.0a 7.3.4.4: function group type 01h). */
#define FUNCTION_GROUP_TYPE_MASK             0x7fu
#define FUNCTION_GROUP_TYPE_AUDIO            0x01u

/* Pin Sense on an HDMI/DP pin (HDA 1.0a 7.3.3.15 and the Intel HDMI codec
 * extension, as used by Linux patch_hdmi.c): bit 31 presence, bit 30 ELD valid
 * (the sink supplied its capabilities, so audio could be sent to it). */
#define HDMI_PIN_SENSE_ELD_VALID             (1u << 30)

/* Subordinate Node Count (HDA 1.0a 7.3.4.3): bits 23:16 first node, 7:0 count. */
#define SUBNODE_COUNT_START_SHIFT            16
#define SUBNODE_COUNT_FIELD_MASK             0xffu

struct hda_display_survey {
    unsigned pins;
    unsigned pins_with_sink;
    unsigned pins_with_valid_eld;
};

/* Count the display codec's pin widgets and how many of them report a sink.
 * Read-only apart from the D0 request the specification requires before a
 * function group can be read (HDA 1.0a 7.3.3.10); nothing is routed and no
 * stream is set up. The numbers go to the boot log so the day GPU stage A
 * programs the display engine, "which pin has the monitor" is already known. */
static void hda_display_survey_pins(struct hda *h, struct hda_display_survey *survey)
{
    uint32_t root = codec_param(h, 0, PARAM_SUBNODE_COUNT);
    unsigned group_first = (root >> SUBNODE_COUNT_START_SHIFT) & SUBNODE_COUNT_FIELD_MASK;
    unsigned group_count = root & SUBNODE_COUNT_FIELD_MASK;

    for (unsigned group = group_first; group < group_first + group_count; group++) {
        uint32_t type = codec_param(h, group, PARAM_FG_TYPE) & FUNCTION_GROUP_TYPE_MASK;
        uint32_t widgets;
        unsigned widget_first;
        unsigned widget_count;

        if (type != FUNCTION_GROUP_TYPE_AUDIO) {
            continue;
        }
        (void)hda_power_up(h, group);
        widgets = codec_param(h, group, PARAM_SUBNODE_COUNT);
        widget_first = (widgets >> SUBNODE_COUNT_START_SHIFT) & SUBNODE_COUNT_FIELD_MASK;
        widget_count = widgets & SUBNODE_COUNT_FIELD_MASK;
        for (unsigned nid = widget_first; nid < widget_first + widget_count; nid++) {
            uint32_t capabilities = codec_param(h, nid, PARAM_AUDIO_WIDGET_CAP);
            uint32_t sense = 0;

            if (((capabilities >> AUDIO_WIDGET_CAPABILITY_TYPE_SHIFT) &
                 AUDIO_WIDGET_CAPABILITY_TYPE_MASK) != WIDGET_PIN) {
                continue;
            }
            survey->pins++;
            if (codec_cmd(h, h->codec_addr, nid, VERB_GET_PIN_SENSE, 0, &sense) != 0) {
                continue;
            }
            if (sense & HDA_PIN_SENSE_PRESENCE) {
                survey->pins_with_sink++;
            }
            if (sense & HDMI_PIN_SENSE_ELD_VALID) {
                survey->pins_with_valid_eld++;
            }
        }
        return;
    }
}

/* Bind a GPU's audio function just far enough to learn who its codec is, then
 * hand the hardware back quiet. WHY NOT PLAY: HDMI/DP audio needs the GPU's
 * display engine to packetise it (AFMT) and publish the sink's ELD, which is a
 * later graphics stage ("GPU stage A"); until then any stream the controller
 * plays goes nowhere. WHY BIND AT ALL: "no HDMI controller" and "HDMI
 * controller present, codec answers, output not yet possible" are different
 * facts, and the panel needs to state the second one. The controller is reset
 * and its DMA released afterwards (hda_probe_abort), so nothing here can touch
 * memory once the probe returns. Declines every function that is not display
 * audio; those belong to hda_probe(). */
static int hda_display_probe(struct device *dev)
{
    struct hda *h = &g_hda_display;
    struct hda_function_record *record;
    struct hda_display_survey survey = {0};
    unsigned input_streams = 0;
    uint32_t vendor_and_device;

    if (!hda_display_audio(dev)) {
        return -1;
    }
    record = hda_function_record_for(dev, 1);
    if (h->mmio) {
        record->failure_reason = "another GPU audio function is being enumerated";
        return -1;
    }
    if (hda_pci_mem_only(dev) != 0) {
        record->failure_reason = "PCI memory-only state unconfirmed";
        return -1;
    }
    if (hda_start_command_bus(h, dev, 0, &input_streams) != 0) {
        record->failure_reason = h->bring_up_failure;
        hda_probe_abort(h, dev);
        return -1;
    }
    vendor_and_device = codec_param(h, 0, PARAM_VENDOR);
    if (vendor_and_device == 0 || vendor_and_device == UINT32_MAX) {
        record->failure_reason = "codec did not answer its vendor ID";
        hda_probe_abort(h, dev);
        return -1;
    }
    hda_display_survey_pins(h, &survey);
    record->codec_present = 1;
    record->codec_vendor_and_device = vendor_and_device;
    record->display_pins = survey.pins;
    record->display_pins_with_sink = survey.pins_with_sink;
    record->bound = 1;
    kprintf("[hda] %s: GPU audio function: codec %04x:%04x present, %u pins, %u with a sink, "
            "%u with a valid ELD; HDMI/DP output needs GPU stage A, controller released\n",
            dev->name, (unsigned)(vendor_and_device >> CODEC_VENDOR_ID_SHIFT),
            (unsigned)(vendor_and_device & CODEC_DEVICE_ID_MASK), survey.pins, survey.pins_with_sink,
            survey.pins_with_valid_eld);
    hda_probe_abort(h, dev);
    return 0;
}

/* The display function was released at the end of its probe, so unbinding only
 * has to correct the record the panel reads. */
static void hda_display_remove(struct device *dev)
{
    for (unsigned index = 0; index < HDA_FUNCTION_RECORDS_MAX; index++) {
        if (hda_function_record_matches(&g_function_records[index], dev)) {
            g_function_records[index].bound = 0;
            g_function_records[index].failure_reason = "unbound";
        }
    }
}

static struct driver hda_display_driver = {
    .name     = "hda-display",
    .bus_type = DEV_BUS_PCI,
    .match    = hda_ids,
    .probe    = hda_display_probe,
    .remove   = hda_display_remove,
    .next     = NULL,
};
DRIVER_DECLARE(hda_display_driver);

/* ------------------------------------------------------ panel status --- */

/* Short words for the panel, kept next to the enum they name. */
const char *hda_codec_error_name(enum hda_codec_error error)
{
    switch (error) {
    case HDA_CODEC_ERROR_COMMAND_TIMEOUT:
        return "cmd-timeout";
    case HDA_CODEC_ERROR_POWER_REFUSED:
        return "power-refused";
    case HDA_CODEC_ERROR_POWER_TIMEOUT:
        return "power-timeout";
    case HDA_CODEC_ERROR_OUTPUT_SETUP_FAILED:
        return "setup-failed";
    case HDA_CODEC_ERROR_NONE:
        break;
    }
    return "none";
}

/* The word for how the controller's interrupt was wired (DEV_IRQ_*, set from
 * dev_irq_request()); "polled" means none could be wired. */
static const char *hda_interrupt_mode_name(unsigned mode)
{
    switch (mode) {
    case DEV_IRQ_MSIX:
        return "msix";
    case DEV_IRQ_MSI:
        return "msi";
    case DEV_IRQ_INTX:
        return "intx";
    default:
        break;
    }
    return "polled";
}

/* Add what only the live analog controller knows: per-pin jack and drive
 * state, the DMA engine's state and the counters. Reading refreshes the jacks
 * (interval-limited) so the panel reflects a plug within a poll period even
 * when no audio is playing and the audio worker is asleep. */
static void hda_status_fill_live(struct hda *h, struct hda_controller_status *status)
{
    uint64_t played_before_read;

    hda_poll_output_jacks(h, 0);
    {
        IO_GUARD(&hda_gate);
        hda_fold_link_position(h);
        status->stream_running = (r8(h, h->out_base + SD_CTL) & HDA_RUN) != 0;
    }
    status->pin_count = h->output_plan.path_count;
    for (unsigned index = 0; index < h->output_plan.path_count; index++) {
        const struct hda_output_path *path = &h->output_plan.paths[index];

        status->pins[index].role = path->role;
        status->pins[index].nid = path->route.nodes[0].nid;
        status->pins[index].jack = h->path_jack_state[index];
        status->pins[index].output_enabled = h->path_output_enabled[index];
    }
    played_before_read = h->panel_played_bytes;
    status->bytes_played = h->played_bytes;
    status->position_advancing = h->played_bytes != played_before_read;
    h->panel_played_bytes = h->played_bytes;
    status->interrupts = h->irqs;
    status->interrupt_mode = hda_interrupt_mode_name(h->snd.irq_mode);
    status->last_error = h->last_error;
    status->last_error_nid = h->last_error_nid;
}

/* The panel's single entry point into the driver. Controllers are reported in
 * the order they were offered to the driver, bound or not, so the panel can say
 * why a function has no sound instead of silently omitting it. */
int hda_status_get(unsigned index, struct hda_controller_status *status)
{
    unsigned seen = 0;

    for (unsigned slot = 0; slot < HDA_FUNCTION_RECORDS_MAX; slot++) {
        const struct hda_function_record *record = &g_function_records[slot];

        if (!record->attempted) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        memset(status, 0, sizeof *status);
        status->bus = record->bus;
        status->slot = record->slot;
        status->function = record->function;
        status->is_display_function = record->is_display_function;
        status->bound = record->bound;
        status->not_bound_reason = record->failure_reason;
        status->codec_present = record->codec_present;
        status->codec_vendor_and_device = record->codec_vendor_and_device;
        if (record->bound && !record->is_display_function && g_hda.mmio) {
            hda_status_fill_live(&g_hda, status);
        }
        return 0;
    }
    return -1;
}
