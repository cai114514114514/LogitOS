/* tests/unit/ui_tokens_test.c -- the design headers in c/include/ui against
 * the claims design_language.md makes about them.
 *
 *   contrast   every WCAG ratio stated in 3.1.2 / 3.1.4 / 3.1.5, computed in
 *              double from the token values (a stated figure must hold to
 *              +-0.05, a ">= x" claim to -0.05);
 *   composite  every "≈" in 3.1.1 / 3.1.3 / 3.1.4: the ARGB token composited
 *              over the named surface lands on the stated colour (+-1);
 *   derive     logit_tokens_derive() reproduces each swatch row (+-2 per
 *              channel; the default indigo exactly), and the rule path for
 *              custom accents keeps its guarantees (INFO lines quantify how
 *              far the bare rule is from the tuned table);
 *   material / elevation / motion / apps / args / title: table structure and
 *              every value mirrored from openlogit_*.h.
 *
 * Output: `FAIL <check> <detail>` per failure, exit 1 if any. The negative
 * control (-DUI_TOKENS_NEGCTL: label.tertiary L = #BDBDC6) must fail ONLY
 * `contrast label_tertiary`. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/logit_tokens.h"
#include "ui/logit_elevation.h"
#include "ui/logit_motion.h"
#include "ui/logit_apps.h"

#include "openlogit_anim.h"
#include "openlogit_display.h"
#include "openlogit_glass_material.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ------------------------------------------------------------ colour */

static double lin(int c)
{
    double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static double luma(uint32_t rgb)
{
    return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255);
}
static double contrast(uint32_t a, uint32_t b)
{
    double la = luma(a), lb = luma(b);
    if (la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}
static uint32_t over(uint32_t argb, uint32_t bg)
{
    int a = (int)(argb >> 24), out[3];
    for (int i = 0; i < 3; i++) {
        int s = 16 - 8 * i, f = (int)((argb >> s) & 255), g = (int)((bg >> s) & 255);
        out[i] = (int)floor((f * a + g * (255 - a)) / 255.0 + 0.5);
    }
    return ((uint32_t)out[0] << 16) | ((uint32_t)out[1] << 8) | (uint32_t)out[2];
}
static int chan_delta(uint32_t a, uint32_t b)
{
    int m = 0;
    for (int s = 0; s <= 16; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

enum { EQ, GE };
struct claim { const char *name, *mode; uint32_t fg, bg; double ratio; int kind; const char *where; };

static void report_contrast(const struct claim *c)
{
    double r = contrast(c->fg, c->bg);
    int ok = c->kind == EQ ? fabs(r - c->ratio) <= 0.05 + 1e-9 : r >= c->ratio - 0.05;
    printf("  contrast %-22s %-5s #%06X on #%06X  %5.2f  (%s %.2f, %s)\n", c->name, c->mode,
           c->fg, c->bg, r, c->kind == EQ ? "claim" : ">=", c->ratio, c->where);
    CHECK(ok, "contrast %s %s %.2f vs %s %.2f (%s)", c->name, c->mode, r,
          c->kind == EQ ? "claimed" : "at least", c->ratio, c->where);
}

static void t_contrast(void)
{
    const struct logit_tokens *L = &LOGIT_TOK_LIGHT, *D = &LOGIT_TOK_DARK;
    const struct claim C[] = {
        { "label_primary",   "light", L->label_primary,   L->bg_content, 16.8, EQ, "3.1.2" },
        { "label_primary",   "dark",  D->label_primary,   D->bg_content, 15.7, EQ, "3.1.2" },
        { "label_secondary", "light", L->label_secondary, L->bg_content, 6.4,  EQ, "3.1.2" },
        { "label_secondary", "dark",  D->label_secondary, D->bg_content, 7.9,  EQ, "3.1.2" },
        { "label_secondary", "light", L->label_secondary, L->bg_sidebar, 5.4,  EQ, "3.1.2 on bg.sidebar" },
        { "label_secondary", "dark",  D->label_secondary, D->bg_card,    6.8,  EQ, "3.1.2 on bg.card" },
        { "label_tertiary",  "light", L->label_tertiary,  L->bg_content, 3.3,  EQ, "3.1.2" },
        { "label_tertiary",  "dark",  D->label_tertiary,  D->bg_content, 4.3,  EQ, "3.1.2" },
        { "label_on_accent", "light", L->label_on_accent, L->accent,     4.5,  GE, "3.1.2" },
        { "label_on_accent", "dark",  D->label_on_accent, D->accent,     4.5,  GE, "3.1.2" },
        { "accent",          "light", 0xFFFFFF,           L->accent,     5.06, EQ, "3.1.4 white on fill" },
        { "accent",          "dark",  0xFFFFFF,           D->accent,     4.59, EQ, "3.1.4 white on fill" },
        { "accent_ink",      "light", L->accent_ink,      0xFFFFFF,      6.4,  EQ, "3.1.4 on white" },
        { "accent_ink",      "dark",  D->accent_ink,      D->bg_content, 7.7,  EQ, "3.1.4 on content" },
        { "error_ink",       "light", L->error_ink,       0xFFFFFF,      4.83, EQ, "3.1.5 on white" },
        { "destructive_fill","light", 0xFFFFFF,           L->destructive_fill, 4.83, EQ, "3.1.5 white" },
        { "destructive_fill","dark",  0xFFFFFF,           D->destructive_fill, 4.69, EQ, "3.1.5 white" },
        { "success",         "light", L->success,         L->bg_content, 3.0,  GE, "3.1.5 non-text" },
        { "success",         "dark",  D->success,         D->bg_content, 3.0,  GE, "3.1.5 non-text" },
    };
    for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) report_contrast(&C[i]);

    /* 3.1.4 swatches: white >= 4.5 on every fill; orange 4.67, green 5.04 as
     * stated; and the ink, being text on surfaces, >= 4.5 on bg.content. */
    for (int i = 0; i < LOGIT_SWATCH_COUNT; i++) {
        const struct logit_accent_swatch *s = &LOGIT_ACCENT_SWATCHES[i];
        char nm[32];
        snprintf(nm, sizeof nm, "swatch_%s", s->zh);
        struct claim c1 = { nm, "light", 0xFFFFFF, s->fill_l, 4.5, GE, "3.1.4 white on fill" };
        struct claim c2 = { nm, "dark",  0xFFFFFF, s->fill_d, 4.5, GE, "3.1.4 white on fill" };
        struct claim c3 = { nm, "light", s->ink_l, L->bg_content, 4.5, GE, "3.1.4 ink on content" };
        struct claim c4 = { nm, "dark",  s->ink_d, D->bg_content, 4.5, GE, "3.1.4 ink on content" };
        report_contrast(&c1); report_contrast(&c2); report_contrast(&c3); report_contrast(&c4);
    }
    struct claim orange = { "swatch_orange", "both", 0xFFFFFF, LOGIT_ACCENT_SWATCHES[LOGIT_SWATCH_ORANGE].fill_l, 4.67, EQ, "3.1.4" };
    struct claim green  = { "swatch_green",  "both", 0xFFFFFF, LOGIT_ACCENT_SWATCHES[LOGIT_SWATCH_GREEN].fill_l,  5.04, EQ, "3.1.4" };
    report_contrast(&orange); report_contrast(&green);

    /* Not a claim, recorded for the README: warning is a glyph colour with
     * no stated ratio; on white it is below the 3:1 non-text bar. */
    printf("  INFO warning light #%06X on #FFFFFF %.2f (3.1.5 states no ratio)\n",
           L->warning, contrast(L->warning, 0xFFFFFF));
}

/* The integer contrast predicate derive() relies on agrees with the double
 * one wherever the double ratio is not within 1e-3 of the threshold. */
static void t_contrast_integer(void)
{
    uint32_t x = 0x12345678u;
    int bad = 0, n = 0;
    for (int i = 0; i < 200000; i++) {
        x = x * 1664525u + 1013904223u; uint32_t a = x >> 8;
        x = x * 1664525u + 1013904223u; uint32_t b = x >> 8;
        double r = contrast(a, b);
        if (fabs(r - 4.5) < 1e-3) continue;
        n++;
        if (logit_contrast_at_least(a, b, 45, 10) != (r >= 4.5)) bad++;
    }
    CHECK(bad == 0, "contrast-int %d of %d pairs disagree with the double ratio", bad, n);
}

static void t_composite(void)
{
    const struct logit_tokens *L = &LOGIT_TOK_LIGHT, *D = &LOGIT_TOK_DARK;
    const struct { const char *name; uint32_t argb, bg, approx; } T[] = {
        { "fill_hover L",   L->fill_hover,   0xFFFFFF,      0xF2F2F2 },
        { "fill_hover D",   D->fill_hover,   D->bg_content, 0x27272B },
        { "sep_hairline L", L->sep_hairline, 0xFFFFFF,      0xE7E7E9 },
        { "sep_hairline D", D->sep_hairline, D->bg_content, 0x2F2F33 },
        { "accent_tint L",  L->accent_tint,  0xFFFFFF,      0xE5E5FB },
        { "accent_tint D",  D->accent_tint,  D->bg_content, 0x2D2C54 },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        uint32_t got = over(T[i].argb, T[i].bg);
        CHECK(chan_delta(got, T[i].approx) <= 1, "composite %s #%08X over #%06X = #%06X, design says ~#%06X",
              T[i].name, T[i].argb, T[i].bg, got, T[i].approx);
    }
    /* Every ARGB field really carries an alpha; every opaque one has none. */
    const struct logit_tokens *M[2] = { L, D };
    for (int m = 0; m < 2; m++) {
        const struct logit_tokens *t = M[m];
        uint32_t argb[] = { t->fill_hover, t->fill_pressed, t->sep_hairline, t->sep_strong,
                            t->accent_tint, t->accent_focus_ring, t->accent_sel_text };
        for (unsigned i = 0; i < sizeof argb / sizeof argb[0]; i++)
            CHECK(LOGIT_ALPHA_OF(argb[i]) != 0, "encoding ARGB field %u of mode %d has alpha 0", i, m);
        CHECK(LOGIT_ALPHA_OF(t->accent_tint) == (m ? 66u : 41u)
              && LOGIT_ALPHA_OF(t->accent_focus_ring) == (m ? 150u : 128u)
              && LOGIT_ALPHA_OF(t->accent_sel_text) == (m ? 96u : 56u)
              && LOGIT_RGB_OF(t->accent_tint) == t->accent && LOGIT_RGB_OF(t->accent_focus_ring) == t->accent
              && LOGIT_RGB_OF(t->accent_sel_text) == t->accent, "encoding accent alphas mode %d", m);
        CHECK((t->bg_window | t->bg_content | t->bg_card | t->bg_sidebar | t->bg_inspector | t->bg_toolbar
               | t->bg_raised | t->bg_sunken | t->bg_stage | t->fill_control | t->fill_zebra
               | t->fill_inactive_sel | t->label_primary | t->label_secondary | t->label_tertiary
               | t->label_quaternary | t->label_on_accent | t->sep_opaque | t->accent | t->accent_hover
               | t->accent_pressed | t->accent_ink | t->success | t->success_ink | t->warning
               | t->warning_ink | t->error | t->error_ink | t->destructive_fill | t->shadow_ink) >> 24 == 0,
              "encoding opaque token with a high byte in mode %d", m);
    }
    CHECK(LOGIT_TOK_LIGHT.error == LOGIT_PIN_AUI_ERROR, "pin AUI_ERROR (255,69,58) != error L");
    CHECK(LOGIT_TOK_LIGHT.bg_stage == LOGIT_PIN_IMG_BG && LOGIT_TOK_DARK.bg_stage == LOGIT_PIN_IMG_BG,
          "pin IMG_BG (28,28,32) != bg.stage");
    CHECK(LOGIT_TOK_LIGHT.shadow_ink == LOGIT_SHADOW_INK_L && LOGIT_TOK_DARK.shadow_ink == LOGIT_SHADOW_INK_D,
          "shadow ink tokens != logit_elevation.h");
}

/* ------------------------------------------------------------ derive */

static void t_derive(void)
{
    int maxd = 0;
    for (int i = 0; i < LOGIT_SWATCH_COUNT; i++) {
        const struct logit_accent_swatch *s = &LOGIT_ACCENT_SWATCHES[i];
        for (int dark = 0; dark < 2; dark++) {
            /* ui.accent holds the light fill; a dark fill as base is the same swatch */
            uint32_t bases[2] = { s->fill_l, s->fill_d };
            for (int b = 0; b < 2; b++) {
                struct logit_tokens t = *logit_tok(dark);
                logit_tokens_derive(bases[b], dark, &t);
                uint32_t wf = dark ? s->fill_d : s->fill_l, wi = dark ? s->ink_d : s->ink_l;
                int df = chan_delta(t.accent, wf), di = chan_delta(t.accent_ink, wi);
                if (df > maxd) maxd = df;
                if (di > maxd) maxd = di;
                CHECK(df <= 2 && di <= 2, "derive swatch %s %s base #%06X: fill #%06X ink #%06X, table #%06X #%06X",
                      s->zh, dark ? "dark" : "light", bases[b], t.accent, t.accent_ink, wf, wi);
                CHECK(LOGIT_RGB_OF(t.accent_tint) == t.accent
                      && LOGIT_ALPHA_OF(t.accent_tint) == (dark ? LOGIT_ACCENT_TINT_A_D : LOGIT_ACCENT_TINT_A_L)
                      && LOGIT_ALPHA_OF(t.accent_focus_ring) == (dark ? LOGIT_ACCENT_FOCUS_A_D : LOGIT_ACCENT_FOCUS_A_L)
                      && LOGIT_ALPHA_OF(t.accent_sel_text) == (dark ? LOGIT_ACCENT_SEL_A_D : LOGIT_ACCENT_SEL_A_L)
                      && LOGIT_RGB_OF(t.accent_sel_text) == t.accent,
                      "derive alphas swatch %s %s", s->zh, dark ? "dark" : "light");
            }
        }
    }
    printf("  derive: max channel delta vs the swatch table %d\n", maxd);

    /* The default indigo exactly (3.1.4 role table). */
    struct logit_tokens l = LOGIT_TOK_LIGHT, d = LOGIT_TOK_DARK;
    l.accent = l.accent_hover = l.accent_pressed = l.accent_ink = 0;
    d.accent = d.accent_hover = d.accent_pressed = d.accent_ink = 0;
    logit_tokens_derive(LOGIT_ACCENT_DEFAULT, 0, &l);
    logit_tokens_derive(LOGIT_ACCENT_DEFAULT, 1, &d);
    CHECK(l.accent == 0x5E5CE6 && l.accent_hover == 0x5250DA && l.accent_pressed == 0x4846C8 && l.accent_ink == 0x4D4BD4,
          "derive indigo light: #%06X #%06X #%06X #%06X", l.accent, l.accent_hover, l.accent_pressed, l.accent_ink);
    CHECK(d.accent == 0x6563EE && d.accent_hover == 0x716FF2 && d.accent_pressed == 0x5755DE && d.accent_ink == 0xA5A3FF,
          "derive indigo dark: #%06X #%06X #%06X #%06X", d.accent, d.accent_hover, d.accent_pressed, d.accent_ink);

    /* The rule (custom accents): its guarantees hold for every base. */
    const uint32_t custom[] = { 0xFFD60A, 0x00C7BE, 0xFF2D55, 0x808080, 0xFFFFFF, 0x000000, 0x30B0FF,
                                0xAF52DE, 0x34C759, 0xFF9500, 0x5E5CE6, 0x1F6FEB, 0x0E8577, 0x5F5F6B };
    for (unsigned i = 0; i < sizeof custom / sizeof custom[0]; i++) {
        for (int dark = 0; dark < 2; dark++) {
            struct logit_tokens t = *logit_tok(dark);
            logit_tokens_derive_rule(custom[i], dark, &t);
            double wr = contrast(0xFFFFFF, t.accent), ir = contrast(t.accent_ink, logit_tok(dark)->bg_content);
            int fill_l2 = logit_hsl_l2(t.accent), base_l2 = logit_hsl_l2(custom[i]);
            CHECK(wr >= 4.5 - 1e-9, "derive-rule fill #%06X %s: white %.2f < 4.5", custom[i], dark ? "dark" : "light", wr);
            CHECK(ir >= 4.5 - 1e-9, "derive-rule ink #%06X %s: %.2f < 4.5 on content", custom[i], dark ? "dark" : "light", ir);
            CHECK(fill_l2 <= base_l2, "derive-rule fill #%06X lightened", custom[i]);
            CHECK(!logit_contrast_at_least(0xFFFFFF, custom[i], 45, 10) || t.accent == custom[i],
                  "derive-rule #%06X changed a base that already passes", custom[i]);
            /* hover / pressed are plain RGB mixes (rounded half up) */
            uint32_t h = 0, p = 0;
            for (int s = 16; s >= 0; s -= 8) {
                int c = (int)((t.accent >> s) & 255);
                int hv = dark ? (int)floor(c * 0.92 + 255 * 0.08 + 0.5 + 1e-9) : (int)floor(c * 0.92 + 0.5 + 1e-9);
                int pv = (int)floor(c * 0.84 + 0.5 + 1e-9);
                h |= (uint32_t)hv << s; p |= (uint32_t)pv << s;
            }
            CHECK(t.accent_hover == h && t.accent_pressed == p, "derive-rule mix #%06X %s: hover #%06X want #%06X pressed #%06X want #%06X",
                  custom[i], dark ? "dark" : "light", t.accent_hover, h, t.accent_pressed, p);
        }
    }
    /* INFO for the README: the bare 3.1.4 rule against the tuned swatch rows. */
    for (int i = 0; i < LOGIT_SWATCH_COUNT; i++) {
        const struct logit_accent_swatch *s = &LOGIT_ACCENT_SWATCHES[i];
        struct logit_tokens a = LOGIT_TOK_LIGHT, b = LOGIT_TOK_DARK, c = LOGIT_TOK_LIGHT, e = LOGIT_TOK_DARK;
        logit_tokens_derive(s->fill_l, 0, &a);
        logit_tokens_derive(s->fill_l, 1, &b);
        logit_tokens_derive_rule(s->fill_l, 0, &c);
        logit_tokens_derive_rule(s->fill_d, 1, &e);
        printf("  INFO derive %-4s L fill #%06X hover #%06X pressed #%06X ink #%06X | D fill #%06X hover #%06X pressed #%06X ink #%06X"
               " | rule ink L #%06X (d%d) D #%06X (d%d)\n",
               s->zh, a.accent, a.accent_hover, a.accent_pressed, a.accent_ink,
               b.accent, b.accent_hover, b.accent_pressed, b.accent_ink,
               c.accent_ink, chan_delta(c.accent_ink, s->ink_l), e.accent_ink, chan_delta(e.accent_ink, s->ink_d));
    }
    /* HSL helpers: the lightness move keeps hue (channel order) and lands on l2. */
    uint32_t x = 0x9e3779b9u;
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t c = x >> 8;
        int l2 = (int)((x >> 3) % 511);
        uint32_t o = logit_hsl_with_l2(c, l2);
        if (abs(logit_hsl_l2(o) - l2) > 2) bad++;
    }
    CHECK(bad == 0, "derive-hsl %d of 20000 lightness moves miss their l2 by > 2", bad);
}

/* ------------------------------------------------------------ materials */

static void t_material(void)
{
    CHECK(LOGIT_TIER_REGULAR == OL_GLASS_BLUR_REGULAR && LOGIT_TIER_THIN == OL_GLASS_BLUR_THIN
          && LOGIT_TIER_THICK == OL_GLASS_BLUR_THICK && LOGIT_TIER_ULTRA == OL_GLASS_BLUR_ULTRA,
          "material tier mirror != OL_GLASS_BLUR_*");
    CHECK(LOGIT_RIM_DIRECTIONAL == OL_GLASS_RIM_DIRECTIONAL && LOGIT_RIM_SYMMETRIC == OL_GLASS_RIM_SYMMETRIC,
          "material rim mirror != OL_GLASS_RIM_*");
    CHECK(LOGIT_CUT_TOP == OL_GLASS_CUT_TOP && LOGIT_CUT_BOTTOM == OL_GLASS_CUT_BOTTOM
          && LOGIT_CUT_LEFT == OL_GLASS_CUT_LEFT && LOGIT_CUT_RIGHT == OL_GLASS_CUT_RIGHT,
          "material cut mirror != OL_GLASS_CUT_*");
    static const struct { int mat; int tier; int radius; int text; int transient; } T[] = {
        { LOGIT_MAT_MENUBAR,   LOGIT_TIER_REGULAR, 0,               1, 0 },
        { LOGIT_MAT_MENU,      LOGIT_TIER_THICK,   LOGIT_R_LG,      1, 0 },
        { LOGIT_MAT_DOCK,      LOGIT_TIER_THICK,   LOGIT_R_PANEL,   0, 0 },
        { LOGIT_MAT_SPOTLIGHT, LOGIT_TIER_THICK,   LOGIT_R_PANEL,   1, 0 },
        { LOGIT_MAT_ABOUT,     LOGIT_TIER_ULTRA,   LOGIT_R_PANEL,   1, 1 },
        { LOGIT_MAT_SWITCHER,  LOGIT_TIER_ULTRA,   24,              1, 1 },
        { LOGIT_MAT_SIDEBAR,   LOGIT_TIER_THICK,   LOGIT_R_LG,      1, 0 },
        { LOGIT_MAT_POPOVER,   LOGIT_TIER_THICK,   LOGIT_R_XL,      1, 0 },
        { LOGIT_MAT_HUD_DARK,  LOGIT_TIER_THIN,    LOGIT_R_CAPSULE, 0, 0 },
        { LOGIT_MAT_PENDING,   LOGIT_TIER_THIN,    LOGIT_R_CAPSULE, 0, 0 },
        { LOGIT_MAT_GREETER,   LOGIT_TIER_THIN,    LOGIT_R_CAPSULE, 0, 0 },
        { LOGIT_MAT_REGULAR,   LOGIT_TIER_REGULAR, LOGIT_R_CALLER,  0, 0 },
        { LOGIT_MAT_TITLE,     LOGIT_TIER_REGULAR, 10,              0, 0 },
    };
    CHECK(sizeof T / sizeof T[0] == LOGIT_MAT_COUNT, "material table rows %u != LOGIT_MAT_COUNT", (unsigned)(sizeof T / sizeof T[0]));
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        const struct logit_material *m = logit_material((enum logit_mat)T[i].mat);
        CHECK(m->tier == T[i].tier && m->radius_pt == T[i].radius, "material %u tier %d r %d", i, m->tier, m->radius_pt);
        CHECK(T[i].transient || m->tier != LOGIT_TIER_ULTRA, "material %u persistent glass is ULTRA (9.4)", i);
        CHECK(m->rim == (T[i].mat == LOGIT_MAT_TITLE ? LOGIT_RIM_DIRECTIONAL : LOGIT_RIM_SYMMETRIC),
              "material %u rim %d (principle 2: SYMMETRIC everywhere but today's titlebar)", i, m->rim);
        CHECK(LOGIT_ALPHA_OF(m->tint_l) && LOGIT_ALPHA_OF(m->tint_d), "material %u tint without alpha", i);
        if (T[i].text)   /* P16 fallback: text-bearing glass holds 4.5:1 at tint >= 176 */
            CHECK(LOGIT_ALPHA_OF(m->tint_l) >= 176 && LOGIT_ALPHA_OF(m->tint_d) >= 176,
                  "material %u text-bearing tint alpha %u/%u < 176", i, LOGIT_ALPHA_OF(m->tint_l), LOGIT_ALPHA_OF(m->tint_d));
        if (T[i].mat != LOGIT_MAT_GREETER)
            CHECK(LOGIT_ALPHA_OF(m->rt_l) == 255 && LOGIT_ALPHA_OF(m->rt_d) == 255, "material %u RT solid not opaque", i);
    }
    const struct logit_material *mb = logit_material(LOGIT_MAT_MENUBAR);
    CHECK(LOGIT_RGB_OF(mb->rt_d) == LOGIT_PIN_RT_MENUBAR_D && LOGIT_RGB_OF(mb->rt_d) == LOGIT_MENUBAR_RT_D
          && LOGIT_RGB_OF(mb->rt_l) == LOGIT_MENUBAR_RT_L, "material menubar RT != (24,24,32) / #F2F2F6");
    CHECK(mb->cut == (LOGIT_CUT_TOP | LOGIT_CUT_LEFT | LOGIT_CUT_RIGHT), "material menubar cut T/L/R");
    CHECK(LOGIT_RGB_OF(logit_material(LOGIT_MAT_SIDEBAR)->rt_l) == LOGIT_TOK_LIGHT.bg_sidebar
          && LOGIT_RGB_OF(logit_material(LOGIT_MAT_SIDEBAR)->rt_d) == LOGIT_TOK_DARK.bg_sidebar,
          "material sidebar RT != bg.sidebar");
    const int raised[] = { LOGIT_MAT_MENU, LOGIT_MAT_SPOTLIGHT, LOGIT_MAT_ABOUT, LOGIT_MAT_SWITCHER,
                           LOGIT_MAT_POPOVER, LOGIT_MAT_PENDING };
    for (unsigned i = 0; i < sizeof raised / sizeof raised[0]; i++) {
        const struct logit_material *m = logit_material((enum logit_mat)raised[i]);
        CHECK(LOGIT_RGB_OF(m->rt_l) == LOGIT_TOK_LIGHT.bg_raised && LOGIT_RGB_OF(m->rt_d) == LOGIT_TOK_DARK.bg_raised,
              "material %d RT != bg.raised", raised[i]);
    }
    /* Today's titlebar (wm.c draw_frame) and today's aui_glass (aui_paint.inc). */
    const struct logit_material *ti = logit_material(LOGIT_MAT_TITLE), *re = logit_material(LOGIT_MAT_REGULAR);
    CHECK(ti->tint_l == LOGIT_ARGB(104, 0xFAFAFF) && ti->tint_d == LOGIT_ARGB(150, 0x1E1E28)
          && LOGIT_MAT_TITLE_UNFOCUSED_A_L == 140 && LOGIT_MAT_TITLE_UNFOCUSED_A_D == 180,
          "material title != today's draw_frame values");
    CHECK(re->tint_l == LOGIT_ARGB(50, 0xFFFFFF) && re->tint_d == LOGIT_ARGB(120, 0x22242E),
          "material regular != today's aui_glass values");
    CHECK(logit_material((enum logit_mat)99) == re, "material out-of-range lookup");
}

/* ------------------------------------------------------------ elevation */

static void t_elevation(void)
{
    const struct logit_elevation *e0 = logit_elevation(LOGIT_E0);
    CHECK(e0->n == 0 && logit_elevation_reach24(LOGIT_E0) == 0, "elevation E0 is not flat");
    for (int e = LOGIT_E1; e < LOGIT_ELEV_COUNT; e++) {
        const struct logit_elevation *v = logit_elevation((enum logit_elev)e);
        CHECK(v->n == 2, "elevation %d has %d layers", e, v->n);
        CHECK(v->key.sigma_pt <= LOGIT_SHADOW_SIGMA_MAX && v->ambient.sigma_pt <= LOGIT_SHADOW_SIGMA_MAX,
              "elevation %d sigma above 30 pt", e);
        CHECK(v->key.a_dark > v->key.a_light && v->ambient.a_dark > v->ambient.a_light,
              "elevation %d dark alpha not above light", e);
        CHECK(logit_elevation_reach24((enum logit_elev)e) >= 3 * v->ambient.sigma_pt + v->ambient.spread,
              "elevation %d reach", e);
    }
    /* design numbers, exact (3.3) */
    const struct logit_elevation *e1 = logit_elevation(LOGIT_E1), *e5 = logit_elevation(LOGIT_E5);
    CHECK(e1->key.dy == 256 && e1->key.sigma_pt == 384 && e1->key.a_light == 26 && e1->key.a_dark == 77
          && e1->ambient.sigma_pt == 1024, "elevation E1 != {0,1,1.5,0,26/77} {0,1,4,0,15/51}");
    CHECK(e5->key.dy == 8 * 256 && e5->key.sigma_pt == 12 * 256 && e5->ambient.dy == 20 * 256
          && e5->ambient.sigma_pt == 30 * 256 && e5->ambient.spread == -4 * 256
          && e5->ambient.a_light == 41 && e5->ambient.a_dark == 102, "elevation E5 numbers");
    int ladder[2][3] = { { LOGIT_E1, LOGIT_E2, LOGIT_E3 }, { LOGIT_E4, LOGIT_E5, LOGIT_E6 } };
    for (int r = 0; r < 2; r++)
        for (int k = 1; k < 3; k++) {
            const struct logit_elevation *a = logit_elevation((enum logit_elev)ladder[r][k - 1]),
                                         *b = logit_elevation((enum logit_elev)ladder[r][k]);
            CHECK(b->key.a_light > a->key.a_light && b->key.sigma_pt > a->key.sigma_pt,
                  "elevation ladder %d -> %d not deeper", ladder[r][k - 1], ladder[r][k]);
        }
    CHECK(logit_shadow_argb(&e5->key, 0) == LOGIT_ARGB(56, 0x140F30) && logit_shadow_argb(&e5->key, 1) == LOGIT_ARGB(128, 0),
          "elevation shadow ink");
}

/* ------------------------------------------------------------ motion */

static void t_motion(void)
{
    CHECK(LOGIT_CURVE_LINEAR == OL_LINEAR && LOGIT_CURVE_CUBIC_BEZIER == OL_CUBIC_BEZIER
          && LOGIT_CURVE_IN_CUBIC == OL_EASE_IN_CUBIC, "motion curve mirror != enum ol_curve");
    CHECK(sizeof(struct logit_bezier) == sizeof(struct ol_bezier)
          && offsetof(struct logit_bezier, y2) == offsetof(struct ol_bezier, y2),
          "motion struct logit_bezier is not layout-compatible with ol_bezier");
    static const struct { int id; double omega, zeta, overshoot_pct, tol_pct; } S[] = {
        { LOGIT_SPRING_SNAPPY, 38, 0.86, 0.4, 0.5 },
        { LOGIT_SPRING_SMOOTH, 26, 1.00, 0.0, 0.05 },
        { LOGIT_SPRING_BOUNCY, 28, 0.62, 8.0, 1.0 },
        { LOGIT_SPRING_GENTLE, 18, 0.95, 0.0, 0.05 },
        { LOGIT_SPRING_TICK,   60, 0.45, 20.0, 2.0 },
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) {
        const struct logit_spring_preset *p = &LOGIT_SPRINGS[S[i].id];
        double c = 2 * S[i].zeta * S[i].omega * 256;
        CHECK(p->k == (int)(S[i].omega * S[i].omega), "motion spring %u k %u != omega^2", i, p->k);
        CHECK(fabs(p->c_q8 - c) <= 1.0, "motion spring %u c_q8 %u != 2 zeta omega 256 = %.1f", i, p->c_q8, c);
        /* the preset's own zeta, recovered from {k, c_q8}, gives the stated overshoot */
        double z = p->c_q8 / (512.0 * sqrt(p->k));
        double os = z >= 1 ? 0 : 100 * exp(-z * 3.14159265358979323846 / sqrt(1 - z * z));
        CHECK(fabs(os - S[i].overshoot_pct) <= S[i].tol_pct, "motion spring %u overshoot %.2f %% vs %.1f %%", i, os, S[i].overshoot_pct);
    }
    CHECK(LOGIT_SETTLE_D24 == 64 && LOGIT_SETTLE_V24_S == 128 && logit_spring_settled(63, -127)
          && !logit_spring_settled(64, 0) && !logit_spring_settled(0, 128), "motion settle thresholds");
    const struct { int id, ms, curve; double b[4]; } T[] = {
        { LOGIT_T_HOVER_IN, 90, OL_LINEAR, { 0 } },
        { LOGIT_T_HOVER_OUT, 140, OL_LINEAR, { 0 } },
        { LOGIT_T_FADE_IN, 180, OL_CUBIC_BEZIER, { .2, 0, 0, 1 } },
        { LOGIT_T_FADE_OUT, 120, OL_EASE_IN_CUBIC, { 0 } },
        { LOGIT_T_REVEAL, 420, OL_CUBIC_BEZIER, { .05, .7, .1, 1 } },
        { LOGIT_T_PRESS, 60, OL_LINEAR, { 0 } },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        const struct logit_timed *t = &LOGIT_TIMED[T[i].id];
        CHECK(t->ms == T[i].ms && t->curve == T[i].curve, "motion timed %u: %u ms curve %u", i, t->ms, t->curve);
        if (T[i].curve == OL_CUBIC_BEZIER)
            CHECK(t->bez.x1 == OL_FX(T[i].b[0]) && t->bez.y1 == OL_FX(T[i].b[1])
                  && t->bez.x2 == OL_FX(T[i].b[2]) && t->bez.y2 == OL_FX(T[i].b[3]),
                  "motion timed %u bezier != OL_FX of the design points", i);
    }
    CHECK(T_HOVER_IN == 90 && T_HOVER_OUT == 140 && T_FADE_IN == 180 && T_FADE_OUT == 120 && T_REVEAL == 420
          && T_PRESS == 60 && T_FADE_OUT < T_FADE_IN, "motion timed macros");
    CHECK(LOGIT_PRESS_SCALE_Q8 == (int)(0.97 * 256 + 0.5) && LOGIT_PRESS_SCALE_DOCK_Q8 == (int)(0.92 * 256 + 0.5),
          "motion press scales");
}

/* ------------------------------------------------------------ type, radii */

static void t_type(void)
{
    for (int i = 0; i < LOGIT_TS_COUNT; i++) {
        int in = 0;
        for (int k = 0; k < LOGIT_TYPE_SIZE_COUNT; k++) in |= LOGIT_TYPE[i].px == LOGIT_TYPE_SIZES[k];
        CHECK(in, "type style %d px %d outside the closed size set", i, LOGIT_TYPE[i].px);
        CHECK(LOGIT_TYPE[i].weight == 400 || LOGIT_TYPE[i].weight == 500 || LOGIT_TYPE[i].weight == 600,
              "type style %d weight %d", i, LOGIT_TYPE[i].weight);
        CHECK(LOGIT_TYPE[i].px >= LOGIT_CJK_MIN_PX || (LOGIT_TYPE[i].flags & LOGIT_TS_F_LATIN_ONLY),
              "type style %d below 12 px without LATIN_ONLY", i);
    }
    CHECK(LOGIT_TYPE[LOGIT_TS_BODY].px == 14 && LOGIT_TYPE[LOGIT_TS_BODY].line == 20
          && LOGIT_TYPE[LOGIT_TS_TITLE2].weight == 600 && LOGIT_TYPE[LOGIT_TS_DISPLAY].px == 64
          && LOGIT_TYPE[LOGIT_TS_WINDOW_TITLE].px == 13, "type styles 3.6");
    /* baseline = top + (h - 1.448 px)/2 + 1.160 px, against double, 24.8 */
    int worst = 0;
    for (int px = 11; px <= 72; px++)
        for (int h = 0; h <= 96; h += 2) {
            double ref = (h - 1.448 * px) / 2 + 1.160 * px;
            int got = logit_text_baseline24(0, h * 256, px);
            int d = abs(got - (int)lround(ref * 256));
            if (d > worst) worst = d;
        }
    CHECK(worst <= 1, "type baseline24 off by %d/256 pt", worst);
    CHECK(LOGIT_FONT_ASCENT == 1160 && LOGIT_FONT_DESCENT == -288 && LOGIT_FONT_UPEM == 1000, "type metrics");
    /* principle 3: window 20 -> sidebar 12 at inset 8 -> selection pill 6 at inset 6 */
    CHECK(LOGIT_R_WINDOW - LOGIT_SIDEBAR_INSET == LOGIT_R_LG && LOGIT_R_LG - 6 == LOGIT_R_SM,
          "radii concentric 20 -> 12 @ 8 -> 6 @ 6");
    CHECK(logit_r_icon(256) == 58 && logit_r_icon(64) == 14 && LOGIT_SQUIRCLE_Q8 == 154, "radii icon");
    CHECK(LOGIT_SP(4) == LOGIT_SP4 && LOGIT_SP16 == 64 && LOGIT_ROW_LIST == 28 && LOGIT_ROW_LIST_DENSE == 26
          && LOGIT_CTL_REGULAR == 28 && LOGIT_TOOLBAR_H == 38 && LOGIT_TITLEBAR_H == 30 && LOGIT_MENUBAR_H == 24,
          "spacing 3.5");
}

/* ------------------------------------------------------------ apps */

static void t_apps(void)
{
    static const struct { const char *stem, *zh, *en; } A[] = {
        { "files", "文件管理", "Finder" }, { "settings", "设置", "Settings" }, { "browser", "浏览器", "Browser" },
        { "clock", "时钟", "Clock" }, { "monitor", "活动监视器", "Monitor" }, { "preview", "预览", "Preview" },
        { "studio", "代码工作室", "Studio" }, { "assistant", "Logit 助手", "Assistant" },
        { "terminal", "终端", "Terminal" }, { "textedit", "文本编辑", "TextEdit" },
        { "gallery", "组件库", "Gallery" }, { "widgets", "小组件", "Widgets" }, { "ch", "对话", "Chat" },
        { "greeter", "登录", "Login" }, { "spotlight", "搜索", "Spotlight" },
    };
    CHECK(LOGIT_APP_COUNT == (int)(sizeof A / sizeof A[0]), "apps count %d", LOGIT_APP_COUNT);
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++) {
        const struct logit_app *a = logit_app_lookup(A[i].stem);
        CHECK(a && !strcmp(a->stem, A[i].stem) && !strcmp(a->zh, A[i].zh) && !strcmp(a->en, A[i].en),
              "apps lookup %s -> %s", A[i].stem, a ? a->zh : "NULL");
        if (!a) continue;
        CHECK(a->glyph < LOGIT_GLYPH_COUNT && a->glyph_name && a->glyph_name[0], "apps %s glyph", a->stem);
        CHECK(a->grad_top != a->grad_bottom, "apps %s flat gradient", a->stem);
        CHECK(((a->flags & LOGIT_APP_F_LIGHT_TILE) ? LOGIT_APP_INK_DARK : LOGIT_APP_INK_WHITE) == a->glyph_ink,
              "apps %s glyph ink vs tile", a->stem);
        if (a->flags & LOGIT_APP_F_BRAND_GRADIENT)
            CHECK(a->grad_top == LOGIT_BRAND_STOP0 && a->grad_bottom == LOGIT_BRAND_STOP2, "apps %s brand stops", a->stem);
        CHECK(strchr(a->zh, '/') == NULL && logit_title_ok(a->zh), "apps %s display name is not a valid title", a->stem);
    }
    /* 3.7.4 rows, spot-checked */
    CHECK(logit_app_lookup("files")->grad_top == 0x62C6FF && logit_app_lookup("files")->grad_bottom == 0x1E7CF2
          && logit_app_lookup("browser")->glyph_accent == 0xFF5F56 && logit_app_lookup("clock")->glyph_accent == 0xFF9F0A
          && logit_app_lookup("terminal")->grad_bottom == 0x131318, "apps icon rows 3.7.4");
    static const struct { const char *in, *stem; } V[] = {
        { "browser-nofetch", "browser" }, { "gallery_noaa", "gallery" }, { "monitor_negctl", "monitor" },
        { "files.aex", "files" }, { "/sbin/greeter.aex", "greeter" }, { "/sbin/spotlight.aex", "spotlight" },
        { "browser-nofetch.aex", "browser" }, { "ch", "ch" },
        { "nope", NULL }, { "", NULL }, { "filesx", NULL }, { "file", NULL }, { "-files", NULL },
        { "files.txt", NULL }, { "c", NULL }, { "chat", NULL }, { "/", NULL },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        const struct logit_app *a = logit_app_lookup(V[i].in);
        CHECK(V[i].stem ? (a && !strcmp(a->stem, V[i].stem)) : a == NULL, "apps variant \"%s\" -> %s",
              V[i].in, a ? a->stem : "NULL");
    }
    CHECK(logit_app_lookup(NULL) == NULL, "apps lookup NULL");
    CHECK(!strcmp(logit_app_display_name("monitor_negctl", "x"), "活动监视器")
          && !strcmp(logit_app_display_name("unknown", "原标题"), "原标题"), "apps display_name fallback");
    for (int i = 0; i < LOGIT_APP_COUNT; i++)
        for (int j = i + 1; j < LOGIT_APP_COUNT; j++)
            CHECK(strcmp(LOGIT_APPS[i].stem, LOGIT_APPS[j].stem) && strcmp(LOGIT_APPS[i].zh, LOGIT_APPS[j].zh),
                  "apps duplicate %s / %s", LOGIT_APPS[i].stem, LOGIT_APPS[j].stem);
}

static void t_args(void)
{
    static const char *const keys[] = { "appearance", "desktop", "dock", "input", "time", "network", "system", "all" };
    CHECK(sizeof keys / sizeof keys[0] == LOGIT_PANE_COUNT, "args pane count");
    for (int i = 0; i < LOGIT_PANE_COUNT; i++) {
        char b[64];
        int n = logit_app_arg_pane(b, sizeof b, i);
        struct logit_app_arg a;
        char want[64];
        snprintf(want, sizeof want, "--pane=%s", keys[i]);
        CHECK(n == (int)strlen(want) && !strcmp(b, want), "args pane format %d \"%s\"", i, b);
        CHECK(logit_app_arg_parse(b, &a) == LOGIT_ARG_KIND_PANE && a.pane == i, "args pane parse %s", b);
        CHECK(!strcmp(LOGIT_PANES[i].key, keys[i]) && LOGIT_PANES[i].zh[0], "args pane table %d", i);
    }
    struct logit_app_arg a;
    CHECK(logit_app_arg_parse("--pane=bogus", &a) == LOGIT_ARG_INVALID && a.pane == -1, "args pane unknown");
    CHECK(logit_app_arg_parse("--pane=", &a) == LOGIT_ARG_INVALID, "args pane empty");
    CHECK(logit_app_arg_parse("--pane=dockx", &a) == LOGIT_ARG_INVALID, "args pane prefix match");
    CHECK(logit_app_arg_parse("--reveal=/docs/报告.txt", &a) == LOGIT_ARG_KIND_REVEAL && a.path_len == (int)strlen("/docs/报告.txt")
          && !strncmp(a.path, "/docs/报告.txt", (size_t)a.path_len), "args reveal parse");
    CHECK(logit_app_arg_parse("--reveal=docs/a", &a) == LOGIT_ARG_INVALID, "args reveal relative");
    CHECK(logit_app_arg_parse("--reveal=", &a) == LOGIT_ARG_INVALID, "args reveal empty");
    CHECK(logit_app_arg_parse("/docs/a.txt", &a) == LOGIT_ARG_PATH && a.path_len == 11, "args bare path");
    CHECK(logit_app_arg_parse("", &a) == LOGIT_ARG_NONE && logit_app_arg_parse(NULL, &a) == LOGIT_ARG_NONE, "args empty");
    CHECK(logit_app_arg_parse("--frobnicate", &a) == LOGIT_ARG_INVALID, "args unknown long option");
    char b[16];
    CHECK(logit_app_arg_reveal(b, sizeof b, "docs") == -1 && b[0] == 0, "args reveal format relative");
    CHECK(logit_app_arg_reveal(b, sizeof b, "/a/very/long/path") == -1 && b[0] == 0, "args reveal format overflow");
    CHECK(logit_app_arg_pane(b, sizeof b, LOGIT_PANE_COUNT) == -1, "args pane format range");
    char big[64];
    CHECK(logit_app_arg_reveal(big, sizeof big, "/docs") == 14 && !strcmp(big, "--reveal=/docs"), "args reveal format");
}

static void t_title(void)
{
    char b[64];
    CHECK(!logit_title_ok("/untitled.txt") && !logit_title_ok("docs/a") && !logit_title_ok("") && !logit_title_ok(NULL),
          "title paths are never titles");
    CHECK(logit_title_ok("未命名") && logit_title_ok("报告.txt"), "title plain names");
    CHECK(logit_title_from_path(b, sizeof b, "/docs/报告.txt") == (int)strlen("报告.txt") && !strcmp(b, "报告.txt"), "title from path");
    CHECK(logit_title_from_path(b, sizeof b, "") > 0 && !strcmp(b, "未命名"), "title empty -> 未命名");
    CHECK(logit_title_from_path(b, sizeof b, "/docs/") > 0 && !strcmp(b, LOGIT_TITLE_UNTITLED), "title dir -> 未命名");
    CHECK(logit_title_from_path(b, 4, "/abcdef") == -1 && b[0] == 0, "title overflow");
    CHECK(!strcmp(LOGIT_TITLE_EDITED_SUFFIX, " — 已编辑"), "title edited suffix");
}

int main(void)
{
    t_contrast();
    t_contrast_integer();
    t_composite();
    t_derive();
    t_material();
    t_elevation();
    t_motion();
    t_type();
    t_apps();
    t_args();
    t_title();
    if (fails) { printf("ui_tokens_test: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("ui_tokens_test: all %d checks ok\n", checks);
    return 0;
}
