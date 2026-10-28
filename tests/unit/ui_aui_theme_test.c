/* ui-aui-theme: the theme flip to 「澄」 (design 3.1-3.6, section 4) in the
 * ring-3 toolkit. Drives the REAL aui.c (included, so its statics -- the
 * accent base, the poll clock, the capability caches -- are reachable) over
 * the host harness with the real v2 handlers for SHADOW2/GLASS2 (v2_fx.c),
 * PATH (v2_path.c) and TEXT_RUN2 (v2_text.c). Each case runs in a forked child,
 * so every one starts from aui's pristine statics.
 *
 *   roles           every legacy aui_t role equals its 3.1 token mapping in
 *                   light and dark, after aui_set_dark AND after EV_THEME; the
 *                   appended roles are the mode's table with the accent derived
 *   default-accent  with no ui.accent stored (and with the pre-「澄」 schema
 *                   default 0x5E96FF stored), the accent is 靛 indigo:
 *                   #5E5CE6 / #6563EE, ink #4D4BD4 / #A5A3FF
 *   accent-store    ui.accent = #1F6FEB (蓝): accent #1F6FEB, ink L #1A62D6 /
 *                   D #79AEFF (+-2), read with Settings' own call
 *   accent-poll     aui_begin re-reads ui.accent at most once per 500 ms;
 *                   EV_THEME re-reads at once; an app's aui_set_accent preview
 *                   survives the poll while the store is unchanged
 *   set-accent      aui_set_accent goes through derive (a too-light custom
 *                   base is darkened to white >= 4.5:1; focus is the fill)
 *   metrics         AUI_FS_BODY (PX) 14, LABEL 13, CAPTION 12, TITLE 17 / 600,
 *                   HEADING 22 / 600; heights 24/28/32/36/44; radii of 3.4;
 *                   AUI_DISABLED_ALPHA 102; aui_label draws at 14; aui_heading
 *                   is ONE run2 at 22 (title1) and aui_title ONE run2 at 17
 *                   (title2), both weight 600 (the bold face bit without
 *                   TEXT_RUN2); each returns its width measured at 600, and
 *                   the drawn ink ends within it (an inline subtitle placed
 *                   at x + width + gap cannot collide, polish-w2)
 *   text-in         aui_text_in puts every size's baseline on the 3.6 metric
 *                   rule (aui_text_baseline24)
 *   focus-ring      ONE ring: 3 pt of accent.focusRing, 1 pt outside the
 *                   control, only after keyboard navigation, faded in over
 *                   T_HOVER_IN; nothing changes outside the ring's band
 *   card            light: bg.card, E1 (two SHADOW2) and a #141428 @ 20 outer
 *                   outline; dark: bg.card #26262D, no E1 shadow, ONE PATH rim
 *                   brighter at the top (@38) than at 40 % (@10) and the
 *                   bottom (@18)
 *   glass           aui_glass is ONE GLASS2 (REGULAR material), no legacy
 *                   SYS_GUI_GLASS; Reduce Transparency draws the RT solid; a
 *                   FLUSH band (radius <= 1) makes no GLASS2 and no GLASS:
 *                   it is the REGULAR tint over bg.window, its edge pixels
 *                   equal its interior (no rim line, polish-w2)
 *   theme-snap      a Secondary button drawn in light, the theme flipped by
 *                   EV_THEME, ONE frame drawn: the fill is already #3A3A43
 *                   (and back to #FFFFFF), and aui_anim_due stays quiet --
 *                   a background window never draws the tween's later frames
 *   list-card       an inset aui_list is an r 12 bg.card container: dark body
 *                   #26262D (not bg.content), corner pixel outside the round
 *   checkbox-off    a disabled checkbox's label ink reaches as far right as
 *                   the enabled one (the fade group no longer cuts a glyph)
 *
 * -DUI_THEME_NEGCTL (tests/ui-aui-theme.mk) keeps the old blue default and
 * must fail 'default-accent' and nothing else. Exits 1 on any failure. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.c"
#include "aui_host.h"

extern void (*v2_text_spy)(const struct logit_run2 *);
extern struct logit_shadow2 aui_fx_log[8];
extern int aui_fx_nshadow;

static int fails;
static void check(int ok, const char *fmt, ...)
{
    if (ok) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("FAIL "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

/* --------------------------------------------------------------- the store */

static char st_accent[32];               /* "" = not stored */
static int st_rt;                         /* ui.reduce_transparency */
static int st_accent_reads;
static long sp_setting(struct aui_host *h, long a, long b, long c)
{
    (void)h;
    const char *key = (const char *)a;
    char *buf = (char *)b;
    const char *v = 0;
    char tmp[8];
    if (!key || !buf || c <= 1) return -1;
    if (!strcmp(key, "ui.accent")) { st_accent_reads++; if (st_accent[0]) v = st_accent; }
    if (!strcmp(key, "ui.reduce_transparency") && st_rt) { snprintf(tmp, sizeof tmp, "1"); v = tmp; }
    if (!v) return -1;
    snprintf(buf, (size_t)c, "%s", v);
    return (long)strlen(buf);
}

/* ---------------------------------------------------------------- recording */

struct rec { int x24, y24, px24, weight, bold, run2; char s[64]; };
static struct rec recs[64];
static int nrec;
static void add_rec(const char *s, int len, int x24, int y24, int px24, int weight, int bold, int run2)
{
    if (nrec >= 64) return;
    struct rec *r = &recs[nrec++];
    if (len < 0) len = 0;
    if (len > 63) len = 63;
    memcpy(r->s, s, (size_t)len); r->s[len] = 0;
    r->x24 = x24; r->y24 = y24; r->px24 = px24; r->weight = weight; r->bold = bold; r->run2 = run2;
}
static long sp_run(struct aui_host *h, long a, long b, long c)
{
    const struct logit_run *r = (const struct logit_run *)a;
    add_rec(r->s, r->len, r->x * 256, r->y * 256, r->px * 256, 0, r->bold, 0);
    return aui_host_sys_builtin(h, SYS_GUI_TEXT_RUN, a, b, c);
}
static void sp_run2(const struct logit_run2 *r)
{ add_rec(r->s, r->len, r->x, r->y, r->px, r->weight, 0, 1); }

/* -------------------------------------------------------------------- setup */

static struct aui_host *H;
static struct aui_host *host(int w, int h, int pct, int v2, int dark)
{
    aui_host_handlers_clear();
    H = aui_host_new(w, h, pct);
    H->dark = dark;
    if (v2) aui_host_enable_v2(H);
    aui_host_on(SYS_SETTING_GET, sp_setting);
    aui_host_on(SYS_GUI_TEXT_RUN, sp_run);
    v2_text_spy = sp_run2;
    aui_host_clock_set(1000);
    gui_create("ui-aui-theme", w, h);
    aui_set_size(w, h);
    return H;
}
static void feed(int type, int a, int b)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b;
    aui_feed(&e);
}
static unsigned px_at(int x, int y) { return H->px[(size_t)y * (size_t)H->w + (size_t)x] & 0xFFFFFFu; }

/* Run fn in a forked child; its FAILs print there, its exit status is its verdict. */
static int case_fails;
static void run(const char *name, void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fails = 0;
        fn();
        if (!fails) printf("ok   %s\n", name);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st)) { printf("FAIL crash: case '%s' died (status 0x%x)\n", name, st); case_fails++; }
    else if (WEXITSTATUS(st)) case_fails++;
}

/* --------------------------------------------------------- colour helpers */

/* src-over of an ARGB token on an opaque colour, rounded: an independent
 * restatement, compared with a tolerance of 1 per channel. */
static unsigned over(unsigned argb, unsigned under)
{
    int a = (int)(argb >> 24), out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int c = (int)((argb >> sh) & 255), u = (int)((under >> sh) & 255);
        out |= ((u * (255 - a) + c * a + 127) / 255) << sh;
    }
    return (unsigned)out;
}
static int near(unsigned a, unsigned b, int tol)
{
    for (int sh = 0; sh <= 16; sh += 8) {
        int d = (int)((a >> sh) & 255) - (int)((b >> sh) & 255);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------- roles */

static int roles_ok(const char *when)
{
    int dark = aui_is_dark(), ok = 1;
    struct logit_tokens k = *logit_tok(dark);
    logit_tokens_derive(accent_base, dark, &k);
#define EXACT(f, v) if (aui_t.f != (unsigned)(v)) { check(0, "roles %s %s: aui_t." #f " %06x, want %06x", when, dark ? "dark" : "light", aui_t.f, (unsigned)(v)); ok = 0; }
#define NEAR(f, v)  if (!near(aui_t.f, (v), 1) || (aui_t.f >> 24)) { check(0, "roles %s %s: aui_t." #f " %06x, want %06x", when, dark ? "dark" : "light", aui_t.f, (unsigned)(v)); ok = 0; }
    EXACT(bg, k.bg_window)               EXACT(surface, k.bg_content)
    EXACT(surface_2, k.bg_raised)        EXACT(face, k.fill_control)
    NEAR(face_hover, over(k.fill_hover, k.fill_control))
    NEAR(face_active, over(k.fill_pressed, k.fill_control))
    EXACT(text, k.label_primary)         EXACT(muted, k.label_secondary)
    NEAR(border, over(k.sep_strong, k.bg_content))
    NEAR(hi, dark ? over(0x14FFFFFFu, k.bg_content) : 0xFFFFFFu)
    EXACT(accent, k.accent)              EXACT(focus, k.accent)
    EXACT(accent_text, k.label_on_accent)
    EXACT(success, k.success)            EXACT(warning, k.warning)
    EXACT(error, k.error)
    EXACT(track, k.fill_control)         EXACT(thumb, dark ? 0x5A5A64u : 0xFFFFFFu)
    EXACT(disabled, k.fill_control)      EXACT(disabled_tx, k.label_quaternary)
    EXACT(scrim, 0)                      EXACT(shadow, k.shadow_ink)
    NEAR(selection, over(k.accent_sel_text, k.bg_content))
    /* the appended roles: the table, the accent roles derived */
    EXACT(bg_window, k.bg_window) EXACT(bg_content, k.bg_content) EXACT(bg_card, k.bg_card)
    EXACT(bg_sidebar, k.bg_sidebar) EXACT(bg_raised, k.bg_raised) EXACT(fill_control, k.fill_control)
    EXACT(fill_hover, k.fill_hover) EXACT(label_primary, k.label_primary)
    EXACT(label_quaternary, k.label_quaternary) EXACT(sep_strong, k.sep_strong)
    EXACT(accent_hover, k.accent_hover) EXACT(accent_pressed, k.accent_pressed)
    EXACT(accent_ink, k.accent_ink) EXACT(accent_tint, k.accent_tint)
    EXACT(accent_focus_ring, k.accent_focus_ring) EXACT(accent_sel_text, k.accent_sel_text)
    EXACT(shadow_ink, k.shadow_ink)
#undef EXACT
#undef NEAR
    if (memcmp(aui_tok(), &k, sizeof k)) { check(0, "roles %s: aui_tok() is not the table with the accent derived", when); ok = 0; }
    /* the legacy roles are opaque: a window-surface write keeps the high byte clear */
    const unsigned *w = &aui_t.bg;
    for (int i = 0; i < 23; i++) if (w[i] >> 24) { check(0, "roles %s: legacy role %d carries alpha %08x", when, i, w[i]); ok = 0; }
    /* the pins the design keeps */
    if (!dark && aui_t.error != 0xFF453Au) { check(0, "roles %s: light error %06x is not AUI_ERROR (255,69,58)", when, aui_t.error); ok = 0; }
    return ok;
}

static void case_roles(void)
{
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    roles_ok("first frame");
    aui_set_dark(1); roles_ok("aui_set_dark(1)");
    aui_set_dark(0); roles_ok("aui_set_dark(0)");
    H->dark = 1; feed(EV_THEME, 0, 0);
    check(aui_is_dark(), "roles: EV_THEME did not follow the desktop to dark");
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    roles_ok("EV_THEME dark");
    /* the first light frame in light mode draws the window in bg.window */
    H->dark = 0; feed(EV_THEME, 0, 0);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    check(px_at(80, 60) == 0xF4F4F8u, "roles: the cleared window is %06x, want bg.window #F4F4F8", px_at(80, 60));
    roles_ok("EV_THEME light");
}

/* ------------------------------------------------------------ the accents */

static void accent_expect(const char *chk, const char *when, unsigned fl, unsigned fd, unsigned il, unsigned id, int tol)
{
    for (int dark = 0; dark < 2; dark++) {
        aui_set_dark(dark);
        unsigned f = dark ? fd : fl, ink = dark ? id : il;
        check(near(aui_t.accent, f, tol) && near(aui_tok()->accent, f, tol) && near(aui_t.focus, f, tol),
              "%s %s %s: accent %06x (tok %06x, focus %06x), want %06x", chk, when, dark ? "dark" : "light",
              aui_t.accent, aui_tok()->accent, aui_t.focus, f);
        check(near(aui_tok()->accent_ink, ink, tol) && aui_t.accent_ink == aui_tok()->accent_ink,
              "%s %s %s: ink %06x, want %06x", chk, when, dark ? "dark" : "light", aui_tok()->accent_ink, ink);
    }
}

static void case_default_accent(void)
{
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    check(st_accent_reads >= 1, "default-accent: ui.accent was never read (%d reads)", st_accent_reads);
    accent_expect("default-accent", "nothing stored", 0x5E5CE6u, 0x6563EEu, 0x4D4BD4u, 0xA5A3FFu, 0);
}
static void case_default_legacy_store(void)
{
    snprintf(st_accent, sizeof st_accent, "0x5E96FF");      /* the untouched store's answer */
    host(160, 120, 100, 1, 1);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    accent_expect("default-accent", "store 0x5E96FF", 0x5E5CE6u, 0x6563EEu, 0x4D4BD4u, 0xA5A3FFu, 0);
}

static void case_accent_store(void)
{
    snprintf(st_accent, sizeof st_accent, "0x1F6FEB");
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    accent_expect("accent-store", "ui.accent=#1F6FEB", 0x1F6FEBu, 0x1F6FEBu, 0x1A62D6u, 0x79AEFFu, 2);
    roles_ok("ui.accent=#1F6FEB");
}

static void frame_at(unsigned long long ms)
{
    aui_host_clock_set(ms);
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
}
static void case_accent_poll(void)
{
    host(160, 120, 100, 1, 0);
    frame_at(1000);
    int r0 = st_accent_reads;
    unsigned a0 = aui_t.accent;                              /* the default, whatever it is */
    snprintf(st_accent, sizeof st_accent, "0x8A4FE0");      /* 紫, written by Settings */
    frame_at(1100); frame_at(1400);
    check(st_accent_reads == r0, "accent-poll: %d reads inside 500 ms of the last", st_accent_reads - r0);
    check(aui_t.accent == a0, "accent-poll: the accent moved before the poll (%06x, was %06x)", aui_t.accent, a0);
    frame_at(1520);
    check(st_accent_reads == r0 + 1, "accent-poll: %d reads at +520 ms, want 1", st_accent_reads - r0);
    check(aui_t.accent == 0x8A4FE0u, "accent-poll: accent %06x after the poll, want 8a4fe0", aui_t.accent);
    frame_at(1600);
    check(st_accent_reads == r0 + 1, "accent-poll: re-read 80 ms after a poll");
    /* EV_THEME re-reads at once */
    snprintf(st_accent, sizeof st_accent, "0x0E8577");      /* 青 */
    feed(EV_THEME, 0, 0);
    frame_at(1650);
    check(aui_t.accent == 0x0E8577u, "accent-poll: EV_THEME did not re-read ui.accent (%06x)", aui_t.accent);
    /* an app's own preview survives the poll while the store does not move */
    aui_set_accent(0xD6336Cu);
    frame_at(2400); frame_at(3000);
    check(aui_t.accent == 0xD6336Cu, "accent-poll: the poll reset an aui_set_accent preview (%06x)", aui_t.accent);
    snprintf(st_accent, sizeof st_accent, "0x1B7F45");
    frame_at(3600);
    check(aui_t.accent == 0x1B7F45u, "accent-poll: a new stored value did not win (%06x)", aui_t.accent);
}

static void case_set_accent(void)
{
    host(160, 120, 100, 1, 0);
    frame_at(1000);
    aui_set_accent(0x1F6FEBu);
    check(aui_t.accent == 0x1F6FEBu && aui_tok()->accent_ink == 0x1A62D6u, "set-accent: 蓝 %06x ink %06x",
          aui_t.accent, aui_tok()->accent_ink);
    aui_set_accent(0x9FC5FFu);                               /* white on it is ~1.8:1 */
    struct logit_tokens k = *logit_tok(0);
    logit_tokens_derive(0x9FC5FFu, 0, &k);
    check(aui_t.accent == k.accent && aui_t.accent != 0x9FC5FFu && aui_t.focus == aui_t.accent &&
          logit_contrast_at_least(0xFFFFFFu, aui_t.accent, 45, 10),
          "set-accent: a too-light base was not derived (accent %06x, derive %06x, focus %06x)",
          aui_t.accent, k.accent, aui_t.focus);
    check(near(aui_t.selection, over(k.accent_sel_text, k.bg_content), 1),
          "set-accent: selection %06x does not follow the derived accent", aui_t.selection);
    aui_set_dark(1);
    check(aui_tok()->accent_ink != 0 && aui_t.accent != 0x9FC5FFu, "set-accent: the base did not survive a theme flip");
}

/* ---------------------------------------------------------------- metrics */

static void case_metrics_const(void)
{
    check(AUI_FS_BODY == 14 && PX == 14, "metrics: AUI_FS_BODY %d PX %d, want 14", AUI_FS_BODY, PX);
    check(AUI_FS_LABEL == 13 && AUI_FS_CAPTION == 12, "metrics: LABEL %d CAPTION %d", AUI_FS_LABEL, AUI_FS_CAPTION);
    check(AUI_FS_TITLE == 17 && AUI_W_TITLE == 600, "metrics: TITLE %d/%d, want 17/600", AUI_FS_TITLE, AUI_W_TITLE);
    check(AUI_FS_HEADING == 22 && AUI_W_HEADING == 600, "metrics: HEADING %d/%d, want 22/600", AUI_FS_HEADING, AUI_W_HEADING);
    check(AUI_H_SM == 24 && AUI_H_CTL == 28 && AUI_H_LG == 32 && AUI_H_XL == 36 && AUI_H_HERO == 44,
          "metrics: heights %d %d %d %d %d, want 24 28 32 36 44", AUI_H_SM, AUI_H_CTL, AUI_H_LG, AUI_H_XL, AUI_H_HERO);
    check(AUI_R_XS == 4 && AUI_R_SM == 6 && AUI_R_MD == 8 && AUI_R_LG == 12 && AUI_R_XL == 14 &&
          AUI_R_2XL == 18 && AUI_R_PANEL == 22 && AUI_R_WINDOW == 20, "metrics: radii are not design 3.4");
    check(AUI_DISABLED_ALPHA == 102, "metrics: AUI_DISABLED_ALPHA %d, want 102", AUI_DISABLED_ALPHA);
}

static void heading_case(int v2)
{
    host(320, 120, 100, v2, 0);
    aui_begin(AUI_BG);
    nrec = 0;
    aui_label(10, 10, "正文", AUI_TEXT);
    check(nrec == 1 && recs[0].px24 == 14 * 256 && !recs[0].bold && recs[0].weight <= 400,
          "metrics %s: aui_label px %d (bold %d weight %d), want one regular run at 14", v2 ? "v2" : "info0",
          nrec ? recs[0].px24 / 256 : -1, nrec ? recs[0].bold : -1, nrec ? recs[0].weight : -1);
    static const struct { const char *name; int px; } st[2] = { { "aui_heading", 22 }, { "aui_title", 17 } };
    for (int k = 0; k < 2; k++) {
        const char *s = "Activity Monitor";
        nrec = 0;
        int w = k == 0 ? aui_heading(10, 40, s, AUI_TEXT) : aui_title(10, 40, s, AUI_TEXT);
        int px = st[k].px;
        if (v2)
            check(nrec == 1 && recs[0].run2 && recs[0].weight == 600 && recs[0].px24 == px * 256,
                  "metrics v2: %s is not ONE run2 at %d px weight 600 (n %d run2 %d weight %d px %d)", st[k].name, px,
                  nrec, nrec ? recs[0].run2 : -1, nrec ? recs[0].weight : -1, nrec ? recs[0].px24 / 256 : -1);
        else
            check(nrec == 1 && !recs[0].run2 && recs[0].bold && recs[0].px24 == px * 256,
                  "metrics info0: %s is not ONE bold run at %d px (n %d bold %d px %d)", st[k].name, px,
                  nrec, nrec ? recs[0].bold : -1, nrec ? recs[0].px24 / 256 : -1);
        check(w == aui_text_measure_w(s, px, 600) && w > aui_text_measure_w(s, px, 400) - 1,
              "metrics %s: %s returned %d, want its 600 measure %d (400 measure %d)", v2 ? "v2" : "info0",
              st[k].name, w, aui_text_measure_w(s, px, 600), aui_text_measure_w(s, px, 400));
    }
    aui_end(); aui_feed_done();
    /* the ink of each title ends inside the width it returned: a subtitle at
     * x + w + 12 (monitor.c, gallery.c) has a clear gap */
    for (int k = 0; k < 2; k++) {
        const char *s = "Activity Monitor";
        aui_begin(AUI_BG);
        int w = k == 0 ? aui_heading(10, 40, s, AUI_TEXT) : aui_title(10, 40, s, AUI_TEXT);
        aui_end(); aui_feed_done();
        unsigned bg = px_at(300, 10);
        int right = -1;
        for (int y = 36; y < 80; y++)
            for (int x = 10; x < 320; x++)
                if (px_at(x, y) != bg && x > right) right = x;
        check(right >= 10 + w / 2 && right <= 10 + w + 1,
              "metrics %s: %s ink ends at x %d, past its returned width (x 10 + %d)", v2 ? "v2" : "info0",
              st[k].name, right, w);
    }
}
static void case_metrics_v2(void) { heading_case(1); }
static void case_metrics_info0(void) { heading_case(0); }

static void case_text_in(void)
{
    static const int pxs[4] = { 12, 13, 14, 17 };
    host(320, 120, 100, 1, 0);
    aui_begin(AUI_BG);
    for (int i = 0; i < 4; i++) {
        struct aui_rect r = aui_r(8, 20, 200, 28);
        nrec = 0;
        aui_text_in(r, "Ag 中", AUI_TEXT, pxs[i], AUI_ALIGN_LEFT);
        int asc24 = (pxs[i] * 256 * LOGIT_FONT_ASCENT + LOGIT_FONT_UPEM / 2) / LOGIT_FONT_UPEM;
        int want = aui_text_baseline24(20 * 256, 28 * 256, pxs[i]);
        check(nrec == 1 && recs[0].run2 && recs[0].y24 + asc24 == want,
              "text-in: %d px baseline %d/256 pt, want the 3.6 metric rule %d/256", pxs[i],
              nrec ? recs[0].y24 + asc24 : -1, want);
    }
    aui_end(); aui_feed_done();
}

/* -------------------------------------------------------------- focus ring */

#define BX 40
#define BY 40
#define BW 80
#define BH 28
static void frame_btn(unsigned long long ms)
{
    aui_host_clock_set(ms);
    aui_begin(AUI_BG);
    aui_button_ex(BX, BY, BW, BH, "按钮", AUI_V_SECONDARY, 1);
    aui_end(); aui_feed_done();
}
static uint32_t *snap(void)
{
    size_t n = (size_t)H->w * (size_t)H->h;
    uint32_t *p = malloc(n * 4);
    memcpy(p, H->px, n * 4);
    return p;
}
/* How far (pixel rows / columns, Chebyshev) a pixel lies outside the control
 * at 100 % (points are pixels): 0 inside. */
static int dist(int x, int y)
{
    int dx = x < BX ? BX - x : (x >= BX + BW ? x - (BX + BW - 1) : 0);
    int dy = y < BY ? BY - y : (y >= BY + BH ? y - (BY + BH - 1) : 0);
    return dx > dy ? dx : dy;
}
/* The straight run of the top and bottom bands, clear of any corner the
 * control can have (its radius is at most BH / 2). */
static int straight(int x) { return x >= BX + BH / 2 + 1 && x < BX + BW - BH / 2 - 1; }
static void case_focus_ring(void)
{
    host(200, 120, 100, 1, 0);
    frame_btn(1000); frame_btn(1016);
    uint32_t *a = snap();
    unsigned ring = aui_t.accent_focus_ring;
    feed(EV_KEY, '\t', 0); frame_btn(1032);                  /* the fade starts */
    check(aui_focus_visible() && aui_focus_id() != 0, "focus-ring: Tab did not focus the button");
    frame_btn(1032 + T_HOVER_IN / 2);
    /* the top band's middle row (1.5 pt into the 3 pt ring) mid-fade */
    unsigned mid = px_at(BX + BW / 2, BY - 3), base = a[(size_t)(BY - 3) * H->w + BX + BW / 2] & 0xFFFFFFu;
    unsigned full = over(ring, base);
    check(mid != base && mid != full, "focus-ring: no fade in over T_HOVER_IN (half-way %06x, from %06x to %06x)", mid, base, full);
    frame_btn(1032 + T_HOVER_IN + 20); frame_btn(1032 + T_HOVER_IN + 60);
    long outside = 0, bad_band = 0, band = 0;
    for (int y = 0; y < H->h; y++)
        for (int x = 0; x < H->w; x++) {
            unsigned was = a[(size_t)y * H->w + x] & 0xFFFFFFu, now = px_at(x, y);
            int d = dist(x, y);
            if (d > 4 && was != now) outside++;
            if (d >= 1 && d <= 4 && (y < BY || y >= BY + BH) && straight(x)) {
                unsigned want = d == 1 ? was : over(ring, was);   /* 1 pt gap, then 3 pt of ring */
                band++;
                if (!near(now, want, 2)) bad_band++;
            }
        }
    check(outside == 0, "focus-ring: %ld pixels changed farther than 4 pt out (a second ring?)", outside);
    check(bad_band == 0, "focus-ring: %ld of %ld band pixels are not a 1 pt gap + 3 pt of accent.focusRing %08x",
          bad_band, band, ring);
    free(a);
}
static void case_focus_mouse(void)
{
    host(200, 120, 100, 1, 0);
    frame_btn(1000); frame_btn(1016);
    uint32_t *a = snap();
    feed(EV_MOUSE, BX + 10, BY + 10); frame_btn(1032);
    feed(EV_MOUSE_UP, BX + 10, BY + 10); frame_btn(1048);
    feed(EV_MOUSE_MOVE, 190, 110); frame_btn(1300); frame_btn(1600);
    check(aui_focus_id() != 0 && !aui_focus_visible(), "focus-ring: a click should focus without showing focus");
    long ch = 0;
    for (int y = 0; y < H->h; y++)
        for (int x = 0; x < H->w; x++)
            if (dist(x, y) != 0 && (a[(size_t)y * H->w + x] & 0xFFFFFFu) != px_at(x, y)) ch++;
    check(ch == 0, "focus-ring: %ld pixels outside the control changed after a mouse focus (ring shown without keyboard)", ch);
    free(a);
}

/* -------------------------------------------------------------------- card */

#define CX 20
#define CY 20
#define CW 120
#define CH 80
static void card_frame(int elev)
{
    aui_begin(AUI_BG);
    aui_host_stats_reset(H);
    aui_fx_nshadow = 0;
    aui_card(CX, CY, CW, CH, elev);
    aui_end(); aui_feed_done();
}
static void case_card_light(void)
{
    host(160, 120, 100, 1, 0);
    card_frame(AUI_ELEV_1);
    check(aui_fx_nshadow == 2 && aui_fx_log[0].dy == 256 && aui_fx_log[0].blur == 384 &&
          aui_fx_log[0].argb == 0x1A140F30u && aui_fx_log[1].blur == 1024 && aui_fx_log[1].argb == 0x0F140F30u,
          "card light: shadow is not E1 (%d SHADOW2, key dy %d sigma %d argb %08x)", aui_fx_nshadow,
          aui_fx_log[0].dy, aui_fx_log[0].blur, aui_fx_log[0].argb);
    check(px_at(CX + CW / 2, CY + CH / 2) == 0xFFFFFFu, "card light: fill %06x, want bg.card #FFFFFF", px_at(80, 60));
    check(H->calls[SYS_GUI_PATH] == 1, "card light: %llu PATH calls, want the one outline", H->calls[SYS_GUI_PATH]);
    card_frame(AUI_ELEV_0);
    unsigned o = px_at(CX + CW / 2, CY - 1), want = over(0x14141428u, 0xF4F4F8u);
    check(near(o, want, 2), "card light: outline above the top edge %06x, want #141428 @ 20 over bg.window %06x", o, want);
    check(px_at(CX + CW / 2, CY - 2) == 0xF4F4F8u, "card light: the outline is wider than 1 px");
}
static void case_card_dark(void)
{
    host(160, 120, 100, 1, 1);
    card_frame(AUI_ELEV_1);
    check(aui_fx_nshadow == 0, "card dark: %d SHADOW2 for a resting card, want 0 (E1 is light only)", aui_fx_nshadow);
    check(H->calls[SYS_GUI_PATH] == 1, "card dark: %llu PATH calls, want ONE rim", H->calls[SYS_GUI_PATH]);
    unsigned card = 0x26262Du;
    check(px_at(CX + CW / 2, CY + CH / 2) == card, "card dark: fill %06x, want bg.card #26262D", px_at(80, 60));
    check(px_at(4, 4) == 0x1E1E24u, "card dark: window %06x, want bg.window #1E1E24", px_at(4, 4));
    unsigned top = px_at(CX + CW / 2, CY), bot = px_at(CX + CW / 2, CY + CH - 1);
    unsigned midl = px_at(CX, CY + CH * 40 / 100);
    check(near(top, over(0x26FFFFFFu, card), 3), "card dark: top rim %06x, want #FFFFFF @ 38 over the card %06x", top, over(0x26FFFFFFu, card));
    check(near(midl, over(0x0AFFFFFFu, card), 3), "card dark: rim at 40 %% %06x, want @ 10 %06x", midl, over(0x0AFFFFFFu, card));
    check(near(bot, over(0x12FFFFFFu, card), 3), "card dark: bottom rim %06x, want @ 18 %06x", bot, over(0x12FFFFFFu, card));
    check(px_at(CX + CW / 2, CY + 1) == card && px_at(CX + CW / 2, CY - 1) == 0x1E1E24u,
          "card dark: the rim is not exactly one inner device px");
    card_frame(AUI_ELEV_3);
    check(aui_fx_nshadow == 2, "card dark: a raised card (AUI_ELEV_3) lost its shadow (%d)", aui_fx_nshadow);
}
static void case_card_150(void)
{
    host(160, 120, 150, 1, 1);
    card_frame(AUI_ELEV_1);
    int x = aui_host_pt(CX + CW / 2, 150), y0 = aui_host_pt(CY, 150);
    unsigned card = 0x26262Du;
    check(near(px_at(x, y0), over(0x26FFFFFFu, card), 3) && px_at(x, y0 + 1) == card,
          "card dark @150%%: the rim is not one device px (%06x %06x)", px_at(x, y0), px_at(x, y0 + 1));
}

/* ------------------------------------------------------------------- glass */

static void case_glass(void)
{
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG);
    aui_host_stats_reset(H);
    aui_glass(20, 20, 100, 60, 12);
    check(H->calls[SYS_GUI_GLASS2] == 1 && H->calls[SYS_GUI_GLASS] == 0,
          "glass: %llu GLASS2 and %llu legacy GLASS, want one GLASS2", H->calls[SYS_GUI_GLASS2], H->calls[SYS_GUI_GLASS]);
    aui_end(); aui_feed_done();
}
static void case_glass_rt(void)
{
    st_rt = 1;
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG);
    aui_host_stats_reset(H);
    aui_glass(20, 20, 100, 60, 12);
    check(H->calls[SYS_GUI_GLASS2] == 0 && H->calls[SYS_GUI_GLASS] == 0 && H->calls[SYS_GUI_PATH] == 1,
          "glass: Reduce Transparency drew %llu GLASS2 / %llu GLASS / %llu PATH, want the one RT solid",
          H->calls[SYS_GUI_GLASS2], H->calls[SYS_GUI_GLASS], H->calls[SYS_GUI_PATH]);
    aui_end(); aui_feed_done();
}

static void case_glass_flush(void)
{
    for (int dark = 0; dark < 2; dark++) {
        host(160, 120, 100, 1, dark);
        aui_begin(AUI_BG);
        aui_host_stats_reset(H);
        aui_glass(0, 0, 60, 120, 1);                       /* a sidebar flush with three window edges */
        check(H->calls[SYS_GUI_GLASS2] == 0 && H->calls[SYS_GUI_GLASS] == 0,
              "glass flush %s: %llu GLASS2 and %llu legacy GLASS, want none (a rimmed panel)",
              dark ? "dark" : "light", H->calls[SYS_GUI_GLASS2], H->calls[SYS_GUI_GLASS]);
        aui_end(); aui_feed_done();
        unsigned in = px_at(30, 60);
        unsigned want = over(dark ? 0x7822242Eu : 0x32FFFFFFu, aui_t.bg_window);
        check(near(in, want, 1), "glass flush %s: the band is %06x, want the REGULAR tint over bg.window %06x",
              dark ? "dark" : "light", in, want);
        int bad = 0;
        for (int y = 2; y < 118; y++) if (!near(px_at(0, y), in, 3) || !near(px_at(59, y), in, 3)) bad++;
        for (int x = 2; x < 58; x++) if (!near(px_at(x, 0), in, 3) || !near(px_at(x, 119), in, 3)) bad++;
        check(bad == 0, "glass flush %s: %d edge pixels differ from the interior %06x (a rim line; e.g. (0,60) %06x (30,119) %06x)",
              dark ? "dark" : "light", bad, in, px_at(0, 60), px_at(30, 119));
    }
    st_rt = 1;
    host(160, 120, 100, 1, 0);
    aui_begin(AUI_BG);
    aui_host_stats_reset(H);
    aui_glass(0, 0, 60, 120, 1);
    check(H->calls[SYS_GUI_GLASS2] == 0 && H->calls[SYS_GUI_GLASS] == 0 && H->calls[SYS_GUI_PATH] == 1,
          "glass flush: Reduce Transparency drew %llu GLASS2 / %llu GLASS / %llu PATH, want the one RT solid",
          H->calls[SYS_GUI_GLASS2], H->calls[SYS_GUI_GLASS], H->calls[SYS_GUI_PATH]);
    aui_end(); aui_feed_done();
}

/* A Secondary button's fill at a point clear of its label and its round ends. */
static unsigned snap_fill(void) { return px_at(BX + BH / 2 + 2, BY + BH / 2); }
static void case_theme_snap(void)
{
    host(200, 120, 100, 1, 0);
    frame_btn(1000); frame_btn(1016); frame_btn(1200);
    check(near(snap_fill(), 0xFFFFFFu, 2), "theme-snap: the light Secondary fill is %06x, want #FFFFFF", snap_fill());
    H->dark = 1; feed(EV_THEME, 0, 0);
    frame_btn(1216);                                   /* ONE frame, 16 ms after the flip */
    check(near(snap_fill(), 0x3A3A43u, 2),
          "theme-snap: one frame after EV_THEME (dark) the Secondary fill is %06x, want #3A3A43 at once", snap_fill());
    check(!aui_anim_active() && !aui_anim_due(),
          "theme-snap: a tween is still live after the flip (active %d due %d)", aui_anim_active(), aui_anim_due());
    frame_btn(1400);
    H->dark = 0; feed(EV_THEME, 0, 0);
    frame_btn(1416);
    check(near(snap_fill(), 0xFFFFFFu, 2),
          "theme-snap: one frame after EV_THEME (light) the Secondary fill is %06x, want #FFFFFF at once", snap_fill());
    check(!aui_anim_active() && !aui_anim_due(),
          "theme-snap: a tween is still live after the flip back (active %d due %d)", aui_anim_active(), aui_anim_due());
}

static void case_list_card(void)
{
    static const char *const items[3] = { "外观", "桌面", "网络" };
    int sel = 0, scroll = 0;
    host(240, 200, 100, 1, 1);
    aui_begin(AUI_BG);
    aui_fill(0, 0, 240, 200, aui_t.bg_raised);              /* a sidebar */
    aui_list(20, 20, 160, 140, items, 3, &sel, &scroll);
    aui_end(); aui_feed_done();
    check(near(px_at(100, 140), 0x26262Du, 3),
          "list-card: the dark inset list body is %06x, want bg.card #26262D (not bg.content #19191E)", px_at(100, 140));
    check(!near(px_at(20, 20), 0x26262Du, 3) && !near(px_at(20, 20), 0x19191Eu, 3),
          "list-card: the corner pixel (20,20) is %06x: the container is not rounded", px_at(20, 20));
}

static int ink_right(int x0, int y0, int x1, int y1)
{
    unsigned bg = px_at(x1 - 1, y0);
    int right = -1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (px_at(x, y) != bg && x > right) right = x;
    return right;
}
static void case_checkbox_off(void)
{
    int on = 1, off = 1;
    host(260, 120, 100, 1, 0);
    aui_begin(AUI_BG);
    aui_checkbox_ex(20, 20, "Disabled on", &on, 1);
    aui_end(); aui_feed_done();
    int re = ink_right(20, 14, 260, 42);
    aui_begin(AUI_BG);
    aui_checkbox_ex(20, 20, "Disabled on", &off, 0);
    aui_end(); aui_feed_done();
    int rd = ink_right(20, 14, 260, 42);
    check(re > 60 && rd >= re - 1, "checkbox-off: the disabled label ink ends at x %d, the enabled one at %d (a glyph cut)", rd, re);
}

int main(void)
{
    run("roles: legacy roles are their 3.1 tokens, light and dark, set_dark and EV_THEME", case_roles);
    run("default-accent: 靛 indigo with nothing stored", case_default_accent);
    run("default-accent: the pre-「澄」 store default 0x5E96FF reads as 靛", case_default_legacy_store);
    run("accent-store: ui.accent=#1F6FEB derives 蓝 (ink #1A62D6 / #79AEFF)", case_accent_store);
    run("accent-poll: <= one read per 500 ms, EV_THEME at once, previews survive", case_accent_poll);
    run("set-accent: aui_set_accent goes through derive", case_set_accent);
    run("metrics: type, heights, radii, disabled alpha", case_metrics_const);
    run("metrics: aui_label 14, aui_heading run2 22 / 600, aui_title 17 / 600, weighted widths", case_metrics_v2);
    run("metrics: aui_heading / aui_title bold face bit without TEXT_RUN2", case_metrics_info0);
    run("text-in: aui_text_in centres by the 3.6 metric rule", case_text_in);
    run("focus-ring: one 3 pt accent.focusRing ring 1 pt out, faded in", case_focus_ring);
    run("focus-ring: none after a mouse focus", case_focus_mouse);
    run("card light: bg.card, E1, outer outline", case_card_light);
    run("card dark: bg.card on bg.window, one LINEAR rim, no E1", case_card_dark);
    run("card dark @150%: the rim is one device px", case_card_150);
    run("glass: one GLASS2 (REGULAR)", case_glass);
    run("glass: Reduce Transparency is the RT solid", case_glass_rt);
    run("glass: a flush band is a rim-free solid (RT solid under RT)", case_glass_flush);
    run("theme-snap: EV_THEME lands a Secondary fill in one frame, no tween left", case_theme_snap);
    run("list-card: an inset aui_list is a rounded bg.card container", case_list_card);
    run("checkbox-off: the disabled label is not cut by its fade group", case_checkbox_off);
    printf(case_fails ? "FAIL ui-aui-theme: %d case(s) failed\n" : "ok   ui-aui-theme: all cases passed\n", case_fails);
    return case_fails ? 1 : 0;
}
