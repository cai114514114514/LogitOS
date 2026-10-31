/* See hwhud.h. Everything here is read-only diagnosis except the pixels it
 * draws: MSRs are read only after CPUID says they exist, and nothing is
 * written to any device. */
#include <stdint.h>
#include <stddef.h>
#include "hwhud.h"
#include "hwscreen_font.h"
#include "fb.h"
#include "kprintf.h"
#include "ktime.h"
#include "pit.h"
#include "irq.h"
#include "lapic.h"
#include "hardware.h"
#include "hid_diag.h"
#include "logit_boot.h"

#define COLS 92
#define ROWS 22

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

static int g_intel, g_aperf, g_mtrr, g_smi, g_physbits = 36;

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
    if (max >= 6) { cpuid(6, 0, r); g_aperf = r[2] & 1; }
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
    LINE("fb %ux%u scanout %s", fb_width(), fb_height(), fb_scanout_wc() ? "WC" : "not WC");
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
    LINE("clock %s, now() %u ns/call, pit %lu ticks/s", time_source_name(src), ns,
         dms ? (unsigned long)((ticks - p_ticks) * 1000 / dms) : 0ul);
    p_ticks = ticks;
    (void)sink;
}

static void lines_wm(const struct hwhud_wm *w, uint64_t dms)
{
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
    lines_irq(dms);
    lines_mtrr();
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
