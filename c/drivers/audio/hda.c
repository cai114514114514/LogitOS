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
#include "snd.h"
#include "pmm.h"
#include "kprintf.h"
#include "pit.h"

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
#define CORBSIZE    0x4D
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

struct bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;      /* bit0 = interrupt on completion */
};

struct hda {
    volatile uint8_t *mmio;
    struct device    *dev;

    uint32_t         *corb;       /* 256 entries */
    uint64_t         *rirb;       /* 256 entries of 2 x 32 */
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
};

static struct hda g_hda;
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

    wp = (h->corb_wp + 1) % 256;
    h->corb[wp] = val;
    h->corb_wp = wp;
    w16(h, CORBWP, (uint16_t)wp);

    /* TWO bounds, not one. The clock bound is the meaningful one; the spin
     * bound is what keeps a missing, frozen or IF=0-stalled clock from turning
     * an unresponsive codec into a hung boot. A driver that can hang the
     * machine when its hardware is absent is worse than no driver. */
    deadline = hda_ms() + 50;
    for (unsigned long spins = 0; spins < 200000000ul; spins++) {
        unsigned rwp = r16(h, RIRBWP) & 0xFF;
        if (rwp != h->rirb_rp) {
            h->rirb_rp = (h->rirb_rp + 1) % 256;
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

/* Walk the widget graph and find a DAC that can reach a usable output pin.
 *
 * Deliberately generic rather than hardcoded to QEMU's node numbering. QEMU's
 * hda-output codec happens to put the DAC at nid 2 and the pin at nid 3, and
 * hardcoding that would work today and drive nothing else -- which defeats the
 * point of driving HDA at all. Returns 0 on success. */
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
            } else if (type == WIDGET_PIN && npin < 16) {
                uint32_t pcap = codec_param(h, nid, PARAM_PIN_CAP);
                uint32_t cfg = 0;
                codec_cmd(h, h->codec_addr, nid, VERB_GET_CONFIG_DEF, 0, &cfg);
                kprintf("[hda]   nid %u: PIN (cap %x pincap %x cfg %x)\n",
                        nid, cap, pcap, cfg);
                if (!(pcap & (1u << 4))) continue;        /* not output-capable */
                /* Port connectivity 0x1 == "no physical connection". Picking
                 * one of those is how sound goes to a jack that does not
                 * exist -- every register correct, nothing audible. */
                if (((cfg >> 30) & 0x3) == 0x1) continue;
                pins[npin++] = nid;
            } else {
                kprintf("[hda]   nid %u: type %u (cap %x)\n", nid, type, cap);
            }
        }

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

/* ---------------------------------------------------------- the engine --- */

static void hda_stop(struct snd_device *d)
{
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->out_base;
    w8(h, sd + SD_CTL, 0);                     /* clear RUN + interrupt enables */
    udelay(100);
    w8(h, sd + SD_STS, 0x1C);                  /* ack BCIS/FIFOE/DESE */
}

static int hda_start(struct snd_device *d)
{
    struct hda *h = (struct hda *)d->priv;
    unsigned sd = h->out_base, i;
    uint64_t ring_phys = (uint64_t)(uintptr_t)h->ring;
    uint64_t bdl_phys  = (uint64_t)(uintptr_t)h->bdl;

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
     * ones -- input streams come first. */
    {
        unsigned idx = (h->out_base - SD_BASE) / 0x20;
        w32(h, INTCTL, (1u << 31) | (1u << 30) | (1u << idx));
    }
    return 0;
}

static uint64_t hda_position(struct snd_device *d)
{
    struct hda *h = (struct hda *)d->priv;
    return r32(h, h->out_base + SD_LPIB) / (HDA_CHANNELS * 2u);
}

static void hda_isr(void *arg)
{
    struct hda *h = (struct hda *)arg;
    uint32_t sts = r32(h, INTSTS);
    uint8_t  ss;
    unsigned idx = (h->out_base - SD_BASE) / 0x20;

    if (!(sts & (1u << idx))) return;

    ss = r8(h, h->out_base + SD_STS);
    /* Write-1-to-clear. Not clearing leaves the line asserted, and with a
     * level-triggered INTx that is an interrupt storm that wedges the machine
     * -- the worst possible way for an audio driver to fail. */
    w8(h, h->out_base + SD_STS, ss & 0x1C);

    if (ss & 0x04) {                 /* BCIS: a buffer (period) completed */
        h->irqs++;
        snd_period_elapsed(&h->snd);
    }
}

/* ------------------------------------------------------------- probing --- */

static int hda_probe(struct device *dev)
{
    struct hda *h = &g_hda;
    uint64_t bar;
    uint16_t statests;
    unsigned i, iss, oss;

    if (h->mmio) return -1;          /* one output device; see snd_register_device */

    dev_enable(dev, 1);              /* MMIO + bus master: this device does DMA */
    bar = dev_bar_map(dev, 0);
    if (!bar) { kprintf("[hda] %s: no BAR0\n", dev->name); return -1; }

    h->mmio = (volatile uint8_t *)(uintptr_t)bar;
    h->dev = dev;

    /* Controller reset: CRST low, wait for it to read back low, then high and
     * wait for it to read back high. Both directions must be OBSERVED. */
    w32(h, GCTL, 0);
    for (i = 0; i < 100 && (r32(h, GCTL) & 1); i++) udelay(100);
    w32(h, GCTL, 1);
    for (i = 0; i < 100 && !(r32(h, GCTL) & 1); i++) udelay(100);
    if (!(r32(h, GCTL) & 1)) {
        kprintf("[hda] %s: controller did not come out of reset\n", dev->name);
        return -1;
    }
    /* Codecs need time on the link before STATESTS is meaningful. Reading it
     * immediately finds zero codecs on hardware that has several. */
    udelay(1000);

    {
        uint16_t cap = r16(h, GCAP);
        iss = (cap >> 8) & 0xF;
        oss = (cap >> 12) & 0xF;
        if (!oss) { kprintf("[hda] %s: no output streams\n", dev->name); return -1; }
        h->out_base = SD_BASE + iss * 0x20;    /* input descriptors come first */
    }

    statests = r16(h, STATESTS);
    if (!statests) {
        kprintf("[hda] %s: no codec responded (STATESTS=0)\n", dev->name);
        return -1;
    }
    for (i = 0; i < 15; i++) if (statests & (1u << i)) { h->codec_addr = i; break; }

    /* CORB/RIRB. One page each: 256 x 4 and 256 x 8 both fit in 4 KiB, and a
     * PMM page is 4 KiB-aligned, which covers the 128-byte alignment the spec
     * requires for both rings. */
    h->corb = (uint32_t *)(uintptr_t)pmm_alloc();
    h->rirb = (uint64_t *)(uintptr_t)pmm_alloc();
    if (!h->corb || !h->rirb) { kprintf("[hda] no memory for CORB/RIRB\n"); return -1; }
    memset((void *)h->corb, 0, 4096);
    memset((void *)h->rirb, 0, 4096);

    w8(h, CORBCTL, 0);
    w8(h, RIRBCTL, 0);
    w32(h, CORBLBASE, (uint32_t)((uintptr_t)h->corb & 0xFFFFFFFFu));
    w32(h, CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)h->corb >> 32));
    w32(h, RIRBLBASE, (uint32_t)((uintptr_t)h->rirb & 0xFFFFFFFFu));
    w32(h, RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)h->rirb >> 32));
    w8(h, CORBSIZE, 0x02);                 /* 256 entries */
    w8(h, RIRBSIZE, 0x02);

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

    w8(h, CORBCTL, 0x02);                  /* RUN */
    w8(h, RIRBCTL, 0x02);                  /* RUN, response interrupts off:
                                            * we poll RIRBWP inside codec_cmd,
                                            * which is bounded and only runs at
                                            * probe time. */
    udelay(100);

    if (find_output_path(h) != 0) {
        kprintf("[hda] %s: codec %u exposes no usable output path\n",
                dev->name, h->codec_addr);
        return -1;
    }

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

    codec_setup_output(h);

    /* The PCM ring the engine walks, and the BDL that describes it. Contiguous
     * because a BDL entry is a physical extent -- a ring stitched from scattered
     * pages would need one entry per page and would work, but the mixer wants a
     * flat buffer it can index. */
    {
        uint64_t ringp = pmm_alloc_contig((HDA_RING_BYTES + 4095) / 4096);
        uint64_t bdlp  = pmm_alloc();
        if (!ringp || !bdlp) { kprintf("[hda] no memory for the DMA ring\n"); return -1; }
        h->ring = (uint8_t *)(uintptr_t)ringp;
        h->bdl  = (struct bdl_entry *)(uintptr_t)bdlp;
        memset(h->ring, 0, HDA_RING_BYTES);
        memset((void *)h->bdl, 0, 4096);
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

    if (dev_irq_request(dev, hda_isr, h, "hda") >= 0)
        h->snd.irq_mode = dev->irq_mode;
    else
        kprintf("[hda] %s: no interrupt could be wired -- refills would stall\n",
                dev->name);

    dev_set_drvdata(dev, h);

    if (snd_register_device(&h->snd) != 0) return -1;
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
}

/* Match by CLASS, not by ID: 0x04/0x03 is "HD Audio controller" and covers
 * QEMU's intel-hda and ich9-intel-hda as well as every real Intel, AMD, NVIDIA
 * and VIA controller, none of which share a vendor:device. This is exactly what
 * the device model was built for. */
static const struct dev_match hda_ids[] = {
    DEV_MATCH_CLASS(0x04, 0x03),
    DEV_MATCH_END
};

static struct driver hda_driver = {
    .name     = "hda",
    .bus_type = DEV_BUS_PCI,
    .match    = hda_ids,
    .probe    = hda_probe,
    .remove   = NULL,
    .next     = NULL,
};
DRIVER_DECLARE(hda_driver);
