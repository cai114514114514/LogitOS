/* See hwhud.h. Everything here is read-only diagnosis except the pixels it
 * draws: MSRs are read only after CPUID says they exist, and nothing is
 * written to any device. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "wm_trace.h"
#include "smp.h"
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
 * remain distinguishable without a rounding-dependent size label. */
static void gpu_format(char *out, size_t capacity, const struct device *device)
{
    ksnprintf(out, capacity, "gpu %04x:%04x rev=%02x BAR0=%luB BAR5=%luB driver=%s",
              device->vendor, device->device, device->revision,
              (unsigned long)device->res[0].size,
              (unsigned long)device->res[5].size,
              device->drv ? device->drv->name : "unbound");
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

/* Host gates include just the pure formatters above. The production build
 * retains all collectors below; no host stand-in can hide a stale formatter. */
#ifndef HWHUD_FORMAT_HOST
#include "hwhud.h"
#include "hwscreen_font.h"
#include "fb.h"
#include "mmhost.h"
#include "vmm.h"
#include "ktime.h"
#include "pit.h"
#include "irq.h"
#include "lapic.h"
#include "hardware.h"
#include "hid_diag.h"
#include "ehci_diag.h"
#include "nct6779.h"
#include "logit_boot.h"

#define COLS 92
#define ROWS 36

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
            gpu_format(gpu, sizeof gpu, device);
            add_wrapped(gpu);
        }
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
    struct ehci_ep_diag e;
    for (int i = 0; ehci_diag_get(i, &e) == 0; i++)
        LINE("ehci%u dev %u ep %02x period %u phase %u %s done %lu err %lu td %08x qh %08x",
             e.controller, e.dev_addr, e.ep, e.period, e.phase, e.armed ? "armed" : "idle",
             (unsigned long)e.done, (unsigned long)e.errors, e.td_token, e.qh_token);
}

void hwhud_tick(const struct hwhud_wm *wm)
{
    uint64_t ms = time_mono_ms();
    if (p_ms && ms - p_ms < 1000) return;
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
    lines_irq(dms);
    lines_mtrr();
    lines_thermal();
    lines_hid();

    int w = COLS * HWSCREEN_FONT_W + 8, h = g_rows * HWSCREEN_FONT_H + 8;
    int x = (int)fb_width() - w - 8, y = 40;
    if (x < 0) x = 0;
    fb_graphics_lock();
    fb_fill_rect(x, y, w, h, fb_rgb(12, 12, 20));
    uint32_t fg = fb_rgb(120, 255, 160);
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; g_line[r][c]; c++)
            glyph(x + 4 + c * HWSCREEN_FONT_W, y + 4 + r * HWSCREEN_FONT_H, g_line[r][c], fg);
    fb_present_rect(x, y, w, h);
    fb_graphics_unlock();
}
#endif /* !HWHUD_FORMAT_HOST */
