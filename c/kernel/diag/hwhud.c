/* See hwhud.h. Everything here is read-only diagnosis except the pixels it
 * draws: MSRs are read only after CPUID says they exist, and nothing is
 * written to any device. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "wm_trace.h"
#include "smp.h"
#include "hwhud_panel.h"
#ifndef HWHUD_FORMAT_HOST
#include "kprintf.h"
#endif

/* Decode a leaf PTE using the live PAT, not the stale fb_init snapshot.
 * Intel SDM Vol. 3A sections 4.5 and 11.12: PWT/PCD/PAT are bits 3/4/7 and
 * Present is bit 0; each selected IA32_PAT byte encodes a memory type.
 * pat.c normally changes only PA1 from reset WT to WC, but reading the MSR
 * keeps a later PAT change visible too. MTRR interaction is shown
 * separately by lines_mtrr: these labels describe PTE policy, not a measured
 * effective bus memory type. Not verified on X79-VG4 hardware. */
static const char *scanout_type(uint64_t pte, uint64_t pat)
{
    static const char *const types[] = {"UC", "WC", "??", "??",
                                        "WT", "WP", "WB", "UC-"};
    if (!(pte & 1)) {
        return "unmapped";
    }
    unsigned index = ((pte >> 5) & 4) | ((pte >> 3) & 3);
    unsigned type = (pat >> (index * 8)) & 0xff;
    return type < sizeof types / sizeof types[0] ? types[type] : "??";
}

/* Keep boot and live policy side by side: the old HUD called a UC remap WC
 * because fb_scanout_wc() remembers only fb_init's first readback. Both ends
 * matter when only part of the scanout has been remapped. A non-WC boot flag
 * cannot distinguish UC from WB, so deliberately do not invent that value. */
static void scanout_format(char *out, size_t capacity, unsigned width,
                            unsigned height, int boot_wc, uint64_t first,
                            uint64_t last, uint64_t pat)
{
#ifdef HUD_NEGCTL_STALE_SCANOUT
    first = boot_wc ? 0x9 : 0x19;
    last = first;
#endif
    const char *first_type = scanout_type(first, pat);
    const char *last_type = scanout_type(last, pat);
    int first_wc = first_type[0] == 'W' && first_type[1] == 'C';
    int last_wc = last_type[0] == 'W' && last_type[1] == 'C';
    ksnprintf(out, capacity, "fb %ux%u scanout first=%s last=%s boot=%s%s",
              width, height, first_type, last_type, boot_wc ? "WC" : "not-WC",
              first_wc != !!boot_wc || last_wc != !!boot_wc ? " CHANGED" : "");
}

/* Show enumerated identity and actual binding, rather than inferring a GPU
 * from a boot framebuffer. BAR lengths are bytes so small/absent apertures
 * remain distinguishable without a rounding-dependent size label. VBIOS_SOURCE
 * is where the driver found this function's ATOM image (module, vfct or none)
 * or NULL when no resource probe covers the device, in which case the field is
 * left out rather than printed as a guess. */
static void gpu_format(char *out, size_t capacity, const struct device *device,
                       const char *vbios_source)
{
    int used = ksnprintf(out, capacity,
              "gpu %04x:%04x rev=%02x BAR0=%luB BAR5=%luB driver=%s",
              device->vendor, device->device, device->revision,
              (unsigned long)device->res[0].size,
              (unsigned long)device->res[5].size,
              device->drv ? device->drv->name : "unbound");
    if (vbios_source && used > 0 && (size_t)used < capacity) {
        ksnprintf(out + used, capacity - (size_t)used, " vbios=%s", vbios_source);
    }
}

/* Preserve every stage of the worst measured frame for machines without a
 * serial cable. Subtract in ns before truncating, as the serial formatter
 * does; CPU time above wall time must not wrap blocked time into a huge stall. */
static void slow_format(char *out, size_t capacity,
                        const struct wm_slowframe *frame, uint64_t count)
{
    const struct wm_slowframe empty = {0};
    if (!frame) {
        frame = &empty;
    }
    uint64_t blocked_ns = frame->total_ns > frame->cpu_ns ?
                          frame->total_ns - frame->cpu_ns : 0;
    ksnprintf(out, capacity, "slow count=%lu us total=%lu cpu=%lu blocked=%lu "
              "lock=%lu layers=%lu top=%lu present=%lu glass=%lu",
              (unsigned long)count, (unsigned long)(frame->total_ns / 1000),
              (unsigned long)(frame->cpu_ns / 1000),
              (unsigned long)(blocked_ns / 1000),
              (unsigned long)(frame->lockwait_ns / 1000),
              (unsigned long)(frame->layers_ns / 1000),
              (unsigned long)(frame->top_ns / 1000),
              (unsigned long)(frame->present_ns / 1000),
              (unsigned long)(frame->glass_ns / 1000));
}

/* One HUD line for the IPI-parallel present: whether the other cores answer
 * vector 241 inside the wait, which ones do not, and whether the path has
 * switched itself off. Pure so the host test pins every character. */
static void par_format(char *out, size_t capacity, const struct smp_present_stats *stats)
{
    uint64_t max_wait_us = stats->tsc_hz ?
        stats->wait_cycles_max * 1000000ull / stats->tsc_hz : 0;
    char state[48];
    if (stats->disabled) {
        ksnprintf(state, sizeof state, "off(%lu/%lu timed out)",
                  (unsigned long)stats->disable_timeouts, (unsigned long)stats->disable_calls);
    } else {
        ksnprintf(state, sizeof state, "on");
    }
    ksnprintf(out, capacity, "par  calls=%lu fast=%lu timeouts=%lu fallback_bands=%lu "
              "max_wait_us=%lu missing=%lx state=%s",
              (unsigned long)stats->calls, (unsigned long)stats->fast,
              (unsigned long)stats->timeouts, (unsigned long)stats->fallback_bands,
              (unsigned long)max_wait_us, (unsigned long)stats->missing_mask, state);
}

/* What the network line shows. A plain struct so the formatter stays pure and
 * the host gate can pin every character; the collector below fills it from the
 * live counters. mbps 0 and duplex -1 mean unknown (link down, or a driver that
 * cannot say), shown as "?" rather than a stale register decode. */
struct hwhud_net {
    const char *ifname;
    int         link_up;
    unsigned    mbps;
    int         duplex;             /* 1 full, 0 half, -1 unknown */
    uint32_t    ip;                 /* host order; 0 = unconfigured */
    const char *dhcp_state;
    unsigned    dhcp_retries;
    unsigned    dhcp_next_seconds;
    unsigned long rx_frames;
    unsigned long tx_frames;
    unsigned long irqs;
    int         poll_active;
};

/* One HUD line for the network: link, address, DHCP state with its retry
 * count and the time to its next transmission, the frame counters, and whether
 * receive is being served by polling because the NIC interrupt is silent. The
 * photo of a screen is the only instrument on a machine with no serial cable,
 * and this is the line that says whether the lease is missing because the
 * link is down, the router is silent, or receive is deaf. */
static void net_format(char *out, size_t capacity, const struct hwhud_net *net)
{
    char speed[16] = "?";
    if (net->link_up && net->mbps) {
        ksnprintf(speed, sizeof speed, "%uMb/s", net->mbps);
    }
    const char *duplex = "?";
    if (net->link_up && net->duplex >= 0) {
        duplex = net->duplex ? "full" : "half";
    }
    ksnprintf(out, capacity,
              "net %s link %s %s %s ip %u.%u.%u.%u dhcp %s retries %u next %us "
              "rx %lu tx %lu irqs %lu poll %s",
              net->ifname, net->link_up ? "up" : "down", speed, duplex,
              (net->ip >> 24) & 255, (net->ip >> 16) & 255,
              (net->ip >> 8) & 255, net->ip & 255,
              net->dhcp_state, net->dhcp_retries, net->dhcp_next_seconds,
              net->rx_frames, net->tx_frames, net->irqs,
              net->poll_active ? "on" : "off");
}

/* One HUD line for the serial console: the rate bytes are queued into the TX
 * ring, the rate they actually leave on the wire, the bytes lost, and the ring
 * fill. A photo of the real board shows at a glance that the UART no longer
 * throttles anything: queued/s follows what the kernel prints, sent/s follows
 * the 115200-baud wire (at most ~11520), dropped stays 0 and the ring drains
 * back toward empty. Pure so the host test pins every character. */
static void console_format(char *out, size_t capacity, uint64_t queued_per_s,
                           uint64_t sent_per_s, uint64_t dropped,
                           unsigned ring_used, unsigned ring_size)
{
    ksnprintf(out, capacity, "con  queued/s %lu sent/s %lu dropped %lu ring %u/%u",
              (unsigned long)queued_per_s, (unsigned long)sent_per_s,
              (unsigned long)dropped, ring_used, ring_size);
}

/* What the two extra HID rows show. A plain struct for the same reason as
 * hwhud_net: the formatters stay pure, the host gate pins every character, and
 * neither needs hid_diag.h on its include path. The request states use the same
 * numbers as HID_DIAG_REQ_* (0 never sent, 1 ok, 2 failed), the decoder 0/1/2
 * as HID_DIAG_DECODE_* and the control state as HID_DIAG_CTL_*; the collector
 * below copies them across and a host gate holds the correspondence. */
struct hwhud_hid {
    unsigned subclass, protocol;
    unsigned decoder;                       /* 1 report descriptor, 2 boot */
    unsigned max_packet, interval;
    unsigned cfg_state, cfg_value;          /* GET_CONFIGURATION */
    unsigned idle_state, idle_units;        /* SET_IDLE, 4 ms units */
    unsigned proto_state, proto_sent;       /* SET_PROTOCOL; 1 boot, 2 report */
    unsigned rdesc_state, rdesc_len;        /* GET_DESCRIPTOR(report) */
    unsigned led_state, led_id, led_len;    /* LED SET_REPORT(Output) */
    unsigned ctl_state;                     /* 0 none .. 4 failed */
    unsigned ctl_rc, ctl_len;               /* last GET_REPORT(Input) */
    unsigned char ctl_first[8];
    unsigned ctl_polls, ctl_fed, ctl_fail;
};

static const char *hid_req_name(unsigned state)
{
    return state == 1 ? "ok" : state == 2 ? "ERR" : "--";
}

/* The bring-up row: one photo shows what the descriptor says (subclass and
 * protocol, endpoint size and interval), which decoder was chosen, which
 * configuration the device reports, and the outcome of every request the probe
 * sent -- so "keyboard silent" can be told apart from "keyboard refused SET_IDLE"
 * or "keyboard thinks it is unconfigured" without another hardware round trip.
 * Typical rows are about 80 columns; with every byte-sized field at 255 and
 * both 16-bit sizes at 65535 it is 99, still inside COLS. */
static void hid_state_format(char *out, size_t capacity, const struct hwhud_hid *hid)
{
    char cfg[8] = "--";
    if (hid->cfg_state == 1) {
        ksnprintf(cfg, sizeof cfg, "%u", hid->cfg_value);
    } else if (hid->cfg_state == 2) {
        ksnprintf(cfg, sizeof cfg, "ERR");
    }
    char idle[20] = "idle --";
    if (hid->idle_state) {
        ksnprintf(idle, sizeof idle, "idle%u %s", hid->idle_units, hid_req_name(hid->idle_state));
    }
    char proto[20] = "proto --";
    if (hid->proto_sent) {
        ksnprintf(proto, sizeof proto, "proto %s %s", hid->proto_sent == 1 ? "boot" : "rpt",
                  hid_req_name(hid->proto_state));
    }
    char rdesc[20] = "rd --";
    if (hid->rdesc_state) {
        ksnprintf(rdesc, sizeof rdesc, "rd %uB %s", hid->rdesc_len, hid_req_name(hid->rdesc_state));
    }
    char led[24] = "led --";
    if (hid->led_state) {
        ksnprintf(led, sizeof led, "led id%u/%uB %s", hid->led_id, hid->led_len,
                  hid_req_name(hid->led_state));
    }
    ksnprintf(out, capacity, "  sc%u/p%u %s mp%u iv%u cfg %s %s %s %s %s",
              hid->subclass, hid->protocol,
              hid->decoder == 1 ? "desc" : hid->decoder == 2 ? "boot" : "dec?",
              hid->max_packet, hid->interval, cfg, idle, proto, rdesc, led);
}

/* The control-pipe fallback row, printed only for a keyboard interface that has
 * one. "poll ctl" is the state in which the control pipe, not the interrupt
 * endpoint, is delivering this keyboard's keystrokes; every other state means
 * the interrupt path is (or may yet be) in charge. "get" is the last
 * GET_REPORT(Input): its outcome, length and first eight bytes, which show
 * whether the answer was real key data. 98 columns at the largest counters. */
static void hid_ctl_format(char *out, size_t capacity, const struct hwhud_hid *hid)
{
    static const char *const modes[] = {"none", "wait 3s", "poll ctl", "retired", "failed"};
    const char *mode = hid->ctl_state < 5 ? modes[hid->ctl_state] : "?";
    ksnprintf(out, capacity,
              "  ctl %s get %s %uB %02x %02x %02x %02x %02x %02x %02x %02x polls %u fed %u fail %u",
              mode, hid_req_name(hid->ctl_rc), hid->ctl_len, hid->ctl_first[0], hid->ctl_first[1],
              hid->ctl_first[2], hid->ctl_first[3], hid->ctl_first[4], hid->ctl_first[5],
              hid->ctl_first[6], hid->ctl_first[7], hid->ctl_polls, hid->ctl_fed, hid->ctl_fail);
}

/* Host gates include just the pure formatters above. The production build
 * retains all collectors below; no host stand-in can hide a stale formatter. */
#ifndef HWHUD_FORMAT_HOST
#include "hwhud.h"
#include "wm.h"
#include "hwscreen_font.h"
#include "serial.h"
#include "fb.h"
#include "mmhost.h"
#include "vmm.h"
#include "ktime.h"
#include "pit.h"
#include "irq.h"
#include "lapic.h"
#include "hardware.h"
#include "hid_diag.h"
#include "../sched/observe/observe_hud.h"
#include "ehci_diag.h"
#include "usb_hc.h"
#include "nct6779.h"
#include "logit_boot.h"
#include "amd/bootfb.h"
#include "net.h"
#include "netdev.h"
#include "eth.h"
#include "dhcp.h"
#include "../../../include/weaksym.h"

/* 100 columns: the two-line EHCI endpoint rows below run to about 95 with
 * three-digit counters. 61 rows: the EHCI rows are drawn last, the network
 * line can wrap to a second row on a long address and counter, and the serial
 * console line takes one more, so a full panel would silently drop exactly the
 * lines the USB bring-up reads. The HID section grew from one row per bound
 * interface to two (bring-up facts) and, for a keyboard, three (control-pipe
 * fallback): the X79-VG4's keyboard and mouse bind four interfaces, so 51 + 6
 * rows are needed and 4 are headroom for a second composite device. 61 rows
 * are 976 px of 16 px text plus the 40 px offset, which still fits a 1080-line
 * panel (the 1080 - 40 - 8 pixels left hold at most 64 rows). */
#define COLS 100
#define ROWS 61

/* The panel's own state; touched only by the WM thread (hwhud_tick and
 * hwhud_click both run in its loop), so it needs no lock. */
static struct hwhud_panel g_panel;

/* The bring-up's NIC interrupt counter (every ISR entry, before the receive
 * path's own count); absent when the driver is not linked. */
extern uint64_t rtl8169_irq_count LOGIT_WEAK;
LOGIT_WEAK_STUB(rtl8169_irq_count);

static struct logit_boot_display_tag g_disp;
static int g_have_disp;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void hwhud_capture_boot_info(uint64_t info)
{
    if (!info) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)info;
    uint32_t total = rd32(base);
    if (total < 16 || total > 0x100000) return;
    for (uint32_t off = 8; off + 8 <= total; ) {
        uint32_t type = rd32(base + off), size = rd32(base + off + 4);
        if (type == 0) break;
        if (size < 8 || size > total - off) return;
        if (type == LOGIT_BOOT_TAG_DISPLAY && size >= sizeof g_disp) {
            const uint8_t *s = base + off;
            uint8_t *d = (uint8_t *)&g_disp;
            for (size_t k = 0; k < sizeof g_disp; k++) d[k] = s[k];
            g_disp.why[sizeof g_disp.why - 1] = 0;
            if (g_disp.listed > LOGIT_BOOT_DISPLAY_MODES_MAX) g_disp.listed = LOGIT_BOOT_DISPLAY_MODES_MAX;
            if (g_disp.edid_len > LOGIT_BOOT_DISPLAY_EDID_MAX) g_disp.edid_len = 0;
            g_have_disp = 1;
        }
        off += (size + 7) & ~7u;
    }
    if (!g_have_disp) { kprintf("[display] no firmware display report\n"); return; }
    kprintf("[display] firmware %ux%u, %u GOP modes, picked by %s%s\n", g_disp.fw_width,
            g_disp.fw_height, g_disp.mode_count, g_disp.why, g_disp.set_failed ? " (SetMode failed)" : "");
}

/* ---- text ----------------------------------------------------------------- */

static char g_line[ROWS][COLS + 1];
static int g_rows;

static void addline(const char *s)
{
    if (g_rows >= ROWS) return;
    int i = 0;
    for (; s[i] && i < COLS; i++) g_line[g_rows][i] = s[i];
    g_line[g_rows][i] = 0;
    g_rows++;
}

/* A worst frame has eight timings and may exceed one 92-column photo row.
 * Wrap between fields instead of silently clipping present/glass, precisely
 * the attribution the serial-less hardware report was missing. */
static void add_wrapped(const char *text)
{
    while (*text) {
        char line[COLS + 1];
        size_t length = 0;
        while (length < COLS && text[length]) {
            length++;
        }
        if (text[length]) {
            size_t boundary = length;
            while (boundary && text[boundary] != ' ') {
                boundary--;
            }
            if (boundary) {
                length = boundary;
            }
        }
        for (size_t i = 0; i < length; i++) {
            line[i] = text[i];
        }
        line[length] = 0;
        addline(line);
        text += length;
        while (*text == ' ') {
            text++;
        }
    }
}

/* ksnprintf has no va_list form; format into a buffer through it per call */
#define LINE(...) do { char b_[COLS + 1]; ksnprintf(b_, sizeof b_, __VA_ARGS__); addline(b_); } while (0)

static void glyph(int x, int y, char c, uint32_t fg)
{
    if (c < 0x20 || c > 0x7e) c = '?';
    const uint8_t *g = hwscreen_font[c - 0x20];
    for (int row = 0; row < HWSCREEN_FONT_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < HWSCREEN_FONT_W; col++)
            if (bits & (0x80 >> col)) fb_fill_rect(x + col, y + row, 1, 1, fg);
    }
}

/* ---- sources -------------------------------------------------------------- */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static int g_intel, g_aperf, g_mtrr, g_smi, g_ptm, g_pat, g_physbits = 36;

static void probe_cpu(void)
{
    static int done;
    if (done) return;
    done = 1;
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max = r[0];
    g_intel = r[1] == 0x756e6547 && r[3] == 0x49656e69 && r[2] == 0x6c65746e;
    cpuid(1, 0, r);
    unsigned family = (r[0] >> 8) & 15, model = ((r[0] >> 4) & 15) | ((r[0] >> 12) & 0xf0);
    g_mtrr = (r[3] >> 12) & 1;
    /* Intel SDM Vol. 2 CPUID.01H:EDX.PAT[16] gates IA32_PAT reads; do not
     * fault the diagnostic panel on a CPU without that optional MSR. */
    g_pat = (r[3] >> 16) & 1;
    int hypervisor = (r[2] >> 31) & 1;
    if (max >= 6) { cpuid(6, 0, r); g_aperf = r[2] & 1; g_ptm = g_intel && !hypervisor && ((r[0] >> 6) & 1); }
    /* MSR_SMI_COUNT (0x34): Nehalem and later (Intel SDM vol 4, table 2-15) */
    g_smi = g_intel && family == 6 && model >= 0x1a;
    cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000008u) { cpuid(0x80000008u, 0, r); g_physbits = r[0] & 0xff; }
}

static const char *mtrr_type(unsigned t)
{
    switch (t) { case 0: return "UC"; case 1: return "WC"; case 4: return "WT";
                 case 5: return "WP"; case 6: return "WB"; default: return "??"; }
}
static void size_str(char *b, int n, uint64_t v)
{
    if (v >= (1ull << 30) && !(v & ((1ull << 30) - 1))) ksnprintf(b, n, "%luG", (unsigned long)(v >> 30));
    else if (v >= (1ull << 20)) ksnprintf(b, n, "%luM", (unsigned long)(v >> 20));
    else ksnprintf(b, n, "%luK", (unsigned long)(v >> 10));
}

static void lines_mtrr(void)
{
    if (!g_mtrr) { LINE("mtrr: not supported"); return; }
    uint64_t cap = power_rdmsr(0xfe), def = power_rdmsr(0x2ff);
    unsigned vcnt = cap & 0xff;
    char out[COLS + 1];
    int n = ksnprintf(out, sizeof out, "mtrr def=%s %s fixed=%s var:", mtrr_type(def & 0xff),
                      (def & 0x800) ? "on" : "OFF", (def & 0x400) ? "on" : "off");
    uint64_t physmask = (g_physbits >= 64) ? ~0ull : ((1ull << g_physbits) - 1);
    for (unsigned i = 0; i < vcnt && i < 20; i++) {
        uint64_t base = power_rdmsr(0x200 + 2 * i), mask = power_rdmsr(0x201 + 2 * i);
        if (!(mask & 0x800)) continue;
        uint64_t b = base & physmask & ~0xfffull, sz = ((~mask & physmask) | 0xfff) + 1;
        char bs[12], ss[12];
        size_str(bs, sizeof bs, b);
        size_str(ss, sizeof ss, sz);
        char item[40];
        ksnprintf(item, sizeof item, " %s+%s %s", b ? bs : "0", ss, mtrr_type(base & 0xff));
        int len = 0; while (item[len]) len++;
        if (n + len > COLS) { addline(out); n = ksnprintf(out, sizeof out, "    "); }
        n += ksnprintf(out + n, (int)sizeof out - n, "%s", item);
    }
    addline(out);
}

static void lines_display(void)
{
    uint64_t lfb, bytes;
    uint64_t first = 0;
    uint64_t last = 0;
    if (fb_boot_lfb_range(&lfb, &bytes) && bytes && lfb <= UINT64_MAX - bytes) {
        uint64_t cr3 = mm_read_cr3();
        uint64_t *first_pte = vmm_pte(cr3, lfb);
        uint64_t *last_pte = vmm_pte(cr3, lfb + bytes - 1);
        if (first_pte) {
            first = __atomic_load_n(first_pte, __ATOMIC_RELAXED);
        }
        if (last_pte) {
            last = __atomic_load_n(last_pte, __ATOMIC_RELAXED);
        }
    }
    char scanout[COLS + 1];
    /* Intel SDM Vol. 3A section 11.12: IA32_PAT is MSR 277h. Without PAT,
     * legacy PWT/PCD policy matches its architectural reset table below. */
    uint64_t pat = g_pat ? power_rdmsr(0x277) : UINT64_C(0x0007040600070406);
    scanout_format(scanout, sizeof scanout, fb_width(), fb_height(),
                   fb_scanout_wc(), first, last, pat);
    addline(scanout);
    /* PCI base class 03h is Display Controller (pci.h). Include every display
     * function, even unbound ones; an HDMI audio function is not a GPU. */
    for (int i = 0; i < dev_count(); i++) {
        const struct device *device = dev_at(i);
        if (device && device->bus_type == DEV_BUS_PCI && device->class_code == 3) {
            char gpu[256];
            gpu_format(gpu, sizeof gpu, device, amd_bootfb_vbios_source(device));
            add_wrapped(gpu);
        }
    }
    /* The read-only CRTC scan and VRAM arena choice (amd/bootfb.c), as the
     * photo-readable twin of the [amd-dce] and [amd-arena] serial lines. */
    static const char *const display_label[] = {"dce ", "arena "};
    for (unsigned line = 0; amd_bootfb_display_hud_line(line); line++) {
        char text[COLS * 2];
        ksnprintf(text, sizeof text, "%s%s", display_label[line],
                  amd_bootfb_display_hud_line(line));
        add_wrapped(text);
    }
    if (!g_have_disp) { LINE("gop: no firmware report"); return; }
    LINE("gop firmware %ux%u, %u modes, pick by %s%s", g_disp.fw_width, g_disp.fw_height,
         g_disp.mode_count, g_disp.why, g_disp.set_failed ? " SETMODE FAILED" : "");
    char out[COLS + 1];
    int n = ksnprintf(out, sizeof out, "modes:");
    for (uint32_t i = 0; i < g_disp.listed; i++) {
        char item[16];
        ksnprintf(item, sizeof item, " %ux%u", g_disp.mode[i].width, g_disp.mode[i].height);
        int len = 0; while (item[len]) len++;
        if (n + len > COLS) { addline(out); n = ksnprintf(out, sizeof out, "      "); }
        n += ksnprintf(out + n, (int)sizeof out - n, "%s", item);
    }
    addline(out);
    if (g_disp.edid_len < 128) { LINE("edid: none from firmware (%u bytes)", g_disp.edid_len); return; }
    const uint8_t *e = g_disp.edid, *dtd = e + 54;
    unsigned w = dtd[2] | (dtd[4] & 0xf0) << 4, h = dtd[5] | (dtd[7] & 0xf0) << 4;
    unsigned clk = (dtd[0] | dtd[1] << 8) * 10;
    char name[14] = "";
    for (int d = 0; d < 4; d++) {
        const uint8_t *x = e + 54 + 18 * d;
        if (x[0] || x[1] || x[3] != 0xfc) continue;
        int k = 0;
        for (; k < 13 && x[5 + k] != 0x0a; k++) name[k] = (char)x[5 + k];
        name[k] = 0;
    }
    LINE("edid %u bytes v1.%u preferred %ux%u %ukHz-clock %ux%ucm \"%s\"", g_disp.edid_len, e[19],
         w, h, clk, e[21], e[22], name);
}

static uint64_t p_tsc, p_mperf, p_aperf, p_smi, p_ticks, p_ms;
static uint32_t p_apic = 0xffffffffu;
static uint64_t p_irq[IRQ_NVEC];
static struct hwhud_wm p_wm;

static void lines_cpu(uint64_t dms)
{
    uint64_t tsc = rdtsc();
    uint32_t apic = lapic_id();
    uint64_t mperf = g_aperf ? power_rdmsr(0xe7) : 0, aperf = g_aperf ? power_rdmsr(0xe8) : 0;
    uint64_t smi = g_smi ? power_rdmsr(0x34) : 0;
    uint64_t hz = time_source_hz(TIMESRC_TSC);
    char busy[48] = "busy ?";
    if (g_aperf && apic == p_apic && tsc > p_tsc && mperf > p_mperf) {
        uint64_t dt = tsc - p_tsc, dm = mperf - p_mperf, da = aperf - p_aperf;
        unsigned mhz = hz ? (unsigned)((hz / 1000000) * da / dm) : 0;
        ksnprintf(busy, sizeof busy, "busy %u%% at %u MHz", (unsigned)(dm * 100 / dt), mhz);
    }
    char smis[24] = "smi n/a";
    /* MSR_SMI_COUNT is per logical CPU and 32 bits wide */
    if (g_smi && dms && apic == p_apic)
        ksnprintf(smis, sizeof smis, "smi %lu/s", (unsigned long)((uint32_t)(smi - p_smi) * 1000ull / dms));
    LINE("cpu apic %u (WM thread): %s  tsc %lu MHz  %s", apic, busy, (unsigned long)(hz / 1000000), smis);
    p_tsc = tsc; p_mperf = mperf; p_aperf = aperf; p_smi = smi; p_apic = apic;
}

static void lines_clock(uint64_t dms)
{
    uint64_t t0 = rdtsc();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 256; i++) sink += time_mono_ns();
    uint64_t cyc = (rdtsc() - t0) / 256, hz = time_source_hz(TIMESRC_TSC);
    unsigned ns = hz ? (unsigned)(cyc * 1000000000ull / hz) : 0;
    uint64_t ticks = timer_ticks();
    int src = time_get_source();
    LINE("clock %s, now() %u ns/call, pit %lu ticks/s, lapic timer %lu kHz", time_source_name(src), ns,
         dms ? (unsigned long)((ticks - p_ticks) * 1000 / dms) : 0ul, (unsigned long)(lapic_timer_hz() / 1000));
    p_ticks = ticks;
    (void)sink;
}

static void lines_wm(const struct hwhud_wm *w, uint64_t dms)
{
    /* Other callers can render too. Copy the borrowed history under the same
     * graphics lock used by wm_render's note, before formatting outside it. */
    struct wm_slowframe worst = {0};
    uint64_t count;
    fb_graphics_lock();
    const struct wm_slowframe *record = wm_slowframe_worst(&count);
    if (record) {
        worst = *record;
    }
    fb_graphics_unlock();
    char slow[384];
    slow_format(slow, sizeof slow, &worst, count);
    add_wrapped(slow);
    uint64_t dc = w->composites - p_wm.composites, dn = w->comp_ns - p_wm.comp_ns;
    uint64_t dp = w->present_ns - p_wm.present_ns, dcm = w->cursor_moves - p_wm.cursor_moves;
    uint64_t dcn = w->cursor_ns - p_wm.cursor_ns, dmo = w->motions - p_wm.motions;
    unsigned per = dms ? (unsigned)(dc * 1000 / dms) : 0;
    LINE("wm %u frames/s  compose avg %lu us (max ever %lu us)  present %lu us/s", per,
         dc ? (unsigned long)(dn / dc / 1000) : 0ul, (unsigned long)(w->comp_ns_max / 1000),
         (unsigned long)(dp / 1000));
    LINE("   cursor %lu moves avg %lu us, %lu motion samples", (unsigned long)dcm,
         dcm ? (unsigned long)(dcn / dcm / 1000) : 0ul, (unsigned long)dmo);
    p_wm = *w;
}

/* Name the speed an EHCI endpoint was scheduled for. ps.speed carries the
 * shared USB encoding (usb_hc.h: 1 full, 2 low, 3 high), not the QH's EPS
 * field (EHCI 1.0 3.6: 0 full, 1 low, 2 high), which is offset by one. */
static const char *ehci_speed_name(unsigned speed)
{
    if (speed == USB_SPEED_HIGH) {
        return "high";
    }
    if (speed == USB_SPEED_LOW) {
        return "low";
    }
    return "full";
}

static uint64_t p_con_queued, p_con_sent;

/* The serial console's throughput line (see console_format), as per-second
 * rates over the same interval every other HUD rate uses. */
static void lines_console(uint64_t dms)
{
    struct serial_tx_stats stats;
    serial_tx_stats(&stats);
    uint64_t queued_per_s = dms ? (stats.queued - p_con_queued) * 1000 / dms : 0;
    uint64_t sent_per_s = dms ? (stats.sent - p_con_sent) * 1000 / dms : 0;
    p_con_queued = stats.queued;
    p_con_sent = stats.sent;
    char line[COLS + 1];
    console_format(line, sizeof line, queued_per_s, sent_per_s, stats.dropped,
                   stats.ring_used, stats.ring_size);
    addline(line);
}

static void lines_irq(uint64_t dms)
{
    char out[COLS + 1];
    int n = ksnprintf(out, sizeof out, "irq/s:");
    for (int i = 0; i < IRQ_NVEC; i++) {
        uint64_t c = irq_vector_count(IRQ_VEC_BASE + i), d = c - p_irq[i];
        p_irq[i] = c;
        if (!d || !dms) continue;
        char item[40];
        ksnprintf(item, sizeof item, " %x:%s=%lu", IRQ_VEC_BASE + i, irq_vector_name(IRQ_VEC_BASE + i),
                  (unsigned long)(d * 1000 / dms));
        int len = 0; while (item[len]) len++;
        if (n + len > COLS) { addline(out); n = ksnprintf(out, sizeof out, "      "); }
        n += ksnprintf(out + n, (int)sizeof out - n, "%s", item);
    }
    addline(out);
}

/* Whether the bound NIC is the RTL8168/8169 driver. Its ISR counter is a
 * global that exists whenever the driver is linked, which is every build, so
 * "the symbol is present" says nothing about which card is in the machine; the
 * first QEMU capture of this line showed `irqs 0` beside a working e1000. */
static int nic_is_rtl8169(const struct netdev *dev)
{
    const char *want = "rtl8169";
    const char *name = dev->name;
    while (*want && *name == *want) {
        name++;
        want++;
    }
    return *want == 0 && *name == 0;
}

/* The primary NIC: link, address, DHCP and receive health on one line (wrapped
 * between fields if it outgrows the panel). Counters are the existing ones --
 * eth stats for frames, the driver's ISR count for interrupts -- so this adds no
 * instrumentation of its own. */
static void lines_net(void)
{
    struct hwhud_net net = {0};
    struct netif *iface = netif_by_index(netdev_primary_ifindex());
    if (!iface || !iface->dev) {
        LINE("net: no NIC bound");
        return;
    }
    net.ifname = iface->name;
    net.link_up = netdev_link_up();
    net.duplex = -1;
    unsigned mbps = 0;
    int full_duplex = 0;
    if (netdev_link_info(&mbps, &full_duplex)) {
        net.mbps = mbps;
        net.duplex = full_duplex;
    }
    net.ip = net_cfg.ip;
    struct dhcp_status dhcp;
    dhcp_status(&dhcp);
    net.dhcp_state = dhcp.state;
    net.dhcp_retries = dhcp.retries;
    net.dhcp_next_seconds = dhcp.next_seconds;
    const struct eth_stats *stats = eth_get_stats();
    net.rx_frames = stats->rx;
    net.tx_frames = stats->tx;
    net.irqs = (LOGIT_HAVE(&rtl8169_irq_count) && nic_is_rtl8169(iface->dev))
                   ? (unsigned long)rtl8169_irq_count
                   : (unsigned long)net_rx_irq_count();
    net.poll_active = net_rx_poll_active();
    char line[256];
    net_format(line, sizeof line, &net);
    add_wrapped(line);
}

/* Package temperature from the digital thermal sensor (IA32_PACKAGE_THERM_STATUS
 * readout below MSR_TEMPERATURE_TARGET's TjMax), and the board's NCT6779. */
static void lines_thermal(void)
{
    char out[COLS + 1];
    int n = 0;
    if (g_ptm) {
        uint64_t st = power_rdmsr(0x1b1), tj = (power_rdmsr(0x1a2) >> 16) & 0xff;
        /* The old bit-31 check treated PACKAGE_THERM_STATUS like the core
         * THERM_STATUS MSR. Intel SDM Vol. 3B Figure 16-33 reserves that bit
         * in the package MSR; thermal.c uses TjMax/distance bounds instead. */
#ifdef HUD_NEGCTL_PKG_BIT31
        if (st & (1ull << 31))
#else
        if (tj && tj >= ((st >> 16) & 0x7f))
#endif
            n = ksnprintf(out, sizeof out, "temp pkg %luC (tjmax %lu)", (unsigned long)(tj - ((st >> 16) & 0x7f)),
                          (unsigned long)tj);
    }
    if (!n) n = ksnprintf(out, sizeof out, "temp pkg n/a");
    struct nct_sensors s;
    int sensor_status = nct6779_read(&s);
    if (sensor_status == 0) {
        for (int t = 0; t < NCT_TEMPS; t++)
            if (s.temp_c[t] != -128 && n < COLS)
                n += ksnprintf(out + n, (int)sizeof out - n, " %s %d", nct_temp_label[t], s.temp_c[t]);
        for (int f = 0; f < NCT_FANS; f++)
            if (s.fan_rpm[f] && n < COLS)
                n += ksnprintf(out + n, (int)sizeof out - n, " fan%d %u", f + 1, s.fan_rpm[f]);
        addline(out);
        LINE("volt CPUVC %u.%03u 3VCC %u.%03u 3VSB %u.%03u VBAT %u.%03u VTT %u.%03u", s.mv[0] / 1000,
             s.mv[0] % 1000, s.mv[3] / 1000, s.mv[3] % 1000, s.mv[7] / 1000, s.mv[7] % 1000,
             s.mv[8] / 1000, s.mv[8] % 1000, s.mv[9] / 1000, s.mv[9] % 1000);
        return;
    }
    /* Admission failures describe ownership or an incomplete scan, not broken
     * sensors. LogitOS errno values are EAGAIN=11, EBUSY=16 and EIO=5 (not
     * Darwin's host errno ABI). EIO must not claim the firmware owns a port. */
    if (sensor_status == -11) {
        ksnprintf(out + n, (int)sizeof out - n, " nct pending");
    } else if (sensor_status == -16) {
        ksnprintf(out + n, (int)sizeof out - n, " nct firmware-owned");
    } else if (sensor_status == -5) {
        ksnprintf(out + n, (int)sizeof out - n, " nct ACPI scan failed");
    }
    addline(out);
}

static void lines_hid(void)
{
    struct usb_hid_diag d;
    int any = 0;
    for (int i = 0; usb_hid_diag_get(i, &d) == 0; i++) {
        any = 1;
        LINE("hid %04x:%04x if%u %s%s rep %lu kbd ok %lu rej %lu down %lu last %uB %02x %02x %02x %02x %02x %02x %02x %02x",
             d.vid, d.pid, d.ifnum, (d.role & 1) ? "kbd" : "", (d.role & 2) ? "mouse" : "",
             (unsigned long)d.reports, (unsigned long)d.kbd_ok, (unsigned long)d.kbd_rej,
             (unsigned long)d.kbd_down, d.last_len, d.last[0], d.last[1], d.last[2], d.last[3],
             d.last[4], d.last[5], d.last[6], d.last[7]);
        struct hwhud_hid hid = {
            .subclass = d.subclass, .protocol = d.protocol, .decoder = d.decoder,
            .max_packet = d.max_packet, .interval = d.interval,
            .cfg_state = d.cfg_rc, .cfg_value = d.cfg_value,
            .idle_state = d.idle_rc, .idle_units = d.idle_units,
            .proto_state = d.proto_rc, .proto_sent = d.proto_sent,
            .rdesc_state = d.rdesc_rc, .rdesc_len = d.rdesc_len,
            .led_state = d.led_rc, .led_id = d.led_id, .led_len = d.led_len,
            .ctl_state = d.ctl_state, .ctl_rc = d.ctl_rc, .ctl_len = d.ctl_len,
            .ctl_polls = d.ctl_polls, .ctl_fed = d.ctl_fed, .ctl_fail = d.ctl_fail,
        };
        for (int k = 0; k < 8; k++) hid.ctl_first[k] = d.ctl_first[k];
        char row[COLS + 1];
        hid_state_format(row, sizeof row, &hid);
        addline(row);
        if (d.ctl_state != HID_DIAG_CTL_NONE) {
            hid_ctl_format(row, sizeof row, &hid);
            addline(row);
        }
    }
    if (!any) LINE("hid: no interfaces bound");
    for (unsigned c = 0; c < 4; c++) {
        uint32_t sts, intr; unsigned long errs;
        if (ehci_ctrl_diag(c, &sts, &intr, &errs) == 0)
            LINE("ehci%u usbsts %08x usbintr %08x errors %lu", c, sts, intr, errs);
    }
    for (int i = 0; i < 6; i++) {
        char b[COLS + 1];
        if (pci_intx_diag(i, b, sizeof b) != 0) break;
        addline(b);
    }
    /* Two rows per interrupt endpoint. Row one is the scheduler's view and
     * the error counters; row two is what the QH holds right now, so one photo
     * separates "never visited" (reach 0), "visited and NAKed" (ov Active with
     * CERR intact), a stuck split (SplitXState/MMF in ov) and a toggle or
     * speed/mask mistake (ov bit 31, ch EPS, cap masks and hub address). */
    struct ehci_ep_diag e;
    for (int i = 0; ehci_diag_get(i, &e) == 0; i++) {
        LINE("ehci%u dev %u ep %02x %s s %02x c %02x per %u ph %u uf %u done %lu err h%u x%u m%u d%u b%u st%u rearm %u",
             e.controller, e.dev_addr, e.ep, ehci_speed_name(e.speed), e.smask, e.cmask,
             e.period, e.phase, e.phase_uf, (unsigned long)e.done,
             e.err_halt, e.err_xact, e.err_mmf, e.err_dbe, e.err_babble, e.stalls, e.rearms);
        if (e.live_valid) {
            LINE("  ov %08x cur %08x nxt %08x td %08x ch %08x cap %08x reach %u",
                 e.live_overlay, e.live_current, e.live_next, e.live_td_token,
                 e.ep_chars, e.ep_caps, e.reach);
        } else {
            LINE("  live n/a state %u", e.state);
        }
    }
}

/* Draw the last sample into the back buffer and present it. A collapsed panel
 * is the title row alone. The title carries a [-] / [+] marker so a photograph
 * of the screen says the title row is a button and what it will do. */
static void paint_panel(void)
{
    int rows = g_panel.collapsed ? 1 : g_rows;
    struct hwhud_box box = hwhud_box_for(fb_width(), COLS, rows, HWSCREEN_FONT_W, HWSCREEN_FONT_H);
    char title[COLS + 1];
    ksnprintf(title, sizeof title, "%s %s", g_panel.collapsed ? "[+]" : "[-]", g_line[0]);
    fb_graphics_lock();
    fb_fill_rect(box.x, box.y, box.w, box.h, fb_rgb(12, 12, 20));
    uint32_t fg = fb_rgb(120, 255, 160);
    for (int r = 0; r < rows; r++) {
        const char *text = r == 0 ? title : g_line[r];
        for (int c = 0; text[c]; c++) {
            glyph(box.x + 4 + c * HWSCREEN_FONT_W, box.y + 4 + r * HWSCREEN_FONT_H, text[c], fg);
        }
    }
    fb_present_rect(box.x, box.y, box.w, box.h);
    fb_graphics_unlock();
    hwhud_note_drawn(&g_panel, &box);
}

/* The WM loop calls this at the end of every pass. It is cheap when there is
 * nothing to do (one decision), and does the sampling and the drawing at most
 * once a second -- never while system UI is on the screen, because the panel is
 * painted over the finished frame and would cover it. */
void hwhud_tick(const struct hwhud_wm *wm)
{
    uint64_t ms = time_mono_ms();
    enum hwhud_action action = hwhud_step(&g_panel, wm->system_ui_visible, ms, p_ms);
    if (action == HWHUD_IDLE) {
        return;
    }
    if (action == HWHUD_ERASE) {
        wm_damage(g_panel.box.x, g_panel.box.y, g_panel.box.w, g_panel.box.h);
        return;
    }
    if (action == HWHUD_REDRAW) {
        paint_panel();
        return;
    }
    uint64_t dms = p_ms ? ms - p_ms : 0;
    p_ms = ms;
    probe_cpu();
    g_rows = 0;
    LINE("LogitOS HUD  up %lus  (HWHUD=1 build)", (unsigned long)(ms / 1000));
    lines_display();
    lines_cpu(dms);
    lines_clock(dms);
    lines_wm(wm, dms);
    struct smp_present_stats par;
    smp_present_stats(&par);
    char par_line[256];
    par_format(par_line, sizeof par_line, &par);
    add_wrapped(par_line);
    lines_console(dms);
    /* The scheduler observation summary.  It is formatted from a copy of the
     * counters before anything is drawn, so no observer lock is held while the
     * HUD draws.  A development-machine harness built without the observer gets
     * a stand-in (observe_hud.h) that reports no line, so it adds none. */
    char scheduler_line[COLS + 1];
    if (observe_hud_format_summary(scheduler_line, sizeof scheduler_line) > 0) {
        addline(scheduler_line);
    }
    lines_irq(dms);
    lines_net();
    lines_mtrr();
    lines_thermal();
    lines_hid();
    paint_panel();
}

/* The WM's pointer path asks here first for every left press. Consuming a press
 * on the title row is what keeps it from also raising, dragging or clicking the
 * window under the panel; the smaller (or larger) picture is drawn on the next
 * tick, after the WM has recomposed the box the old one covered. */
int hwhud_click(int x, int y)
{
    struct hwhud_box repaint;
    if (!hwhud_press(&g_panel, x, y, HWSCREEN_FONT_H, &repaint)) {
        return 0;
    }
    wm_damage(repaint.x, repaint.y, repaint.w, repaint.h);
    return 1;
}
#endif /* !HWHUD_FORMAT_HOST */
