/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wm_tok() (ui-wm-abi; design P3, 3.1, 3.1.4): c/kernel/gui/wm_theme.c as
 * shipped, against a stubbed theme flag and settings store.
 *
 *   wm-tok-flag     before any refresh, and after one with the default accent,
 *                   wm_tok() is exactly LOGIT_TOK_LIGHT / LOGIT_TOK_DARK by
 *                   the theme flag, every field
 *   wm-accent       ui.accent = #D6336C (粉) gives the swatch row's fill and
 *                   ink in both themes (+-2 per channel); hover/pressed derived
 *                   from it; the pre-「澄」 store value 0x5E96FF reads as 靛;
 *                   a custom colour is clamped to white-on-fill >= 4.5:1
 *   wm-tok-fields   an accent change touches the seven accent roles only
 *   wm-tok-stable   a refresh with an unchanged accent republishes nothing,
 *                   and a published table is never rewritten in place (the
 *                   pointer a band worker holds stays whole)
 *
 * Prints "FAIL <check> <detail>" and exits 1. THE CONTROL (-DUI_WMTOK_NEGCTL
 * in wm_theme.c) never derives the accent and must log only 'FAIL wm-accent'
 * lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wm_theme.h"
#include "wm.h"
#include "settings.h"

static int g_dark;
static unsigned g_accent = 0x5E96FFu;     /* the kernel schema's default */
static int g_reads;
int wm_dark(void) { return g_dark; }
unsigned settings_get_color(const char *key, unsigned def)
{
    (void)def;
    if (strcmp(key, "ui.accent")) { printf("FAIL wm-tok-key read %s\n", key); exit(1); }
    g_reads++;
    return g_accent;
}

static int bad;
static int near(unsigned a, unsigned b)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < -2 || d > 2) return 0;
    }
    return 1;
}
static void check(int ok, const char *name, const char *fmt, unsigned a, unsigned b)
{
    if (ok) return;
    printf("FAIL %s ", name);
    printf(fmt, a, b);
    printf("\n");
    bad = 1;
}
/* every field but the seven accent roles */
static int same_non_accent(const struct logit_tokens *a, const struct logit_tokens *b)
{
    struct logit_tokens x = *a, y = *b;
    x.accent = y.accent = 0; x.accent_hover = y.accent_hover = 0;
    x.accent_pressed = y.accent_pressed = 0; x.accent_ink = y.accent_ink = 0;
    x.accent_tint = y.accent_tint = 0; x.accent_focus_ring = y.accent_focus_ring = 0;
    x.accent_sel_text = y.accent_sel_text = 0;
    return !memcmp(&x, &y, sizeof x);
}

int main(void)
{
    /* before the first refresh: the tables, by flag */
    g_dark = 0;
    check(!memcmp(wm_tok(), &LOGIT_TOK_LIGHT, sizeof LOGIT_TOK_LIGHT), "wm-tok-flag", "light before refresh %x %x", 0, 0);
    g_dark = 1;
    check(!memcmp(wm_tok(), &LOGIT_TOK_DARK, sizeof LOGIT_TOK_DARK), "wm-tok-flag", "dark before refresh %x %x", 0, 0);

    /* the unwritten store (0x5E96FF) derives to 靛: the tables exactly */
    wm_theme_refresh();
    g_dark = 0;
    const struct logit_tokens *L = wm_tok();
    check(!memcmp(L, &LOGIT_TOK_LIGHT, sizeof *L), "wm-tok-flag", "light, default accent: accent %06x (want %06x)",
          L->accent, LOGIT_TOK_LIGHT.accent);
    check(L->accent == 0x5E5CE6u, "wm-accent", "legacy store 0x5E96FF reads %06x, want 靛 %06x", L->accent, 0x5E5CE6u);
    g_dark = 1;
    const struct logit_tokens *D = wm_tok();
    check(!memcmp(D, &LOGIT_TOK_DARK, sizeof *D), "wm-tok-flag", "dark, default accent: accent %06x (want %06x)",
          D->accent, LOGIT_TOK_DARK.accent);

    /* unchanged accent: no republish */
    int reads = g_reads;
    const struct logit_tokens *before = wm_tok();
    wm_theme_refresh();
    check(wm_tok() == before, "wm-tok-stable", "republished on an unchanged accent (%x %x)", 0, 0);
    check(g_reads == reads + 1, "wm-tok-stable", "refresh read the store %u times, want %u", (unsigned)(g_reads - reads), 1);

    /* 粉: the swatch row, both themes */
    struct logit_tokens held = *before;          /* what a band worker might hold */
    g_accent = 0xD6336Cu;
    wm_theme_refresh();
    check(!memcmp(before, &held, sizeof held), "wm-tok-stable", "the previous generation was rewritten in place %x %x", 0, 0);
    g_dark = 0;
    L = wm_tok();
    check(near(L->accent, 0xD6336Cu), "wm-accent", "粉 light fill %06x, want %06x", L->accent, 0xD6336Cu);
    check(near(L->accent_ink, 0xC22A5Eu), "wm-accent", "粉 light ink %06x, want %06x", L->accent_ink, 0xC22A5Eu);
    check(near(L->accent_hover, logit_rgb_mix(0xD6336Cu, 0x000000, 8)), "wm-accent", "粉 light hover %06x, want %06x",
          L->accent_hover, logit_rgb_mix(0xD6336Cu, 0x000000, 8));
    check(near(L->accent_pressed, logit_rgb_mix(0xD6336Cu, 0x000000, 16)), "wm-accent", "粉 light pressed %06x, want %06x",
          L->accent_pressed, logit_rgb_mix(0xD6336Cu, 0x000000, 16));
    check(L->accent_tint == LOGIT_ARGB(LOGIT_ACCENT_TINT_A_L, L->accent), "wm-accent", "粉 light tint %08x, want %08x",
          L->accent_tint, LOGIT_ARGB(LOGIT_ACCENT_TINT_A_L, L->accent));
    check(same_non_accent(L, &LOGIT_TOK_LIGHT), "wm-tok-fields", "light: a non-accent role moved (%x %x)", 0, 0);
    g_dark = 1;
    D = wm_tok();
    check(near(D->accent, 0xD6336Cu), "wm-accent", "粉 dark fill %06x, want %06x", D->accent, 0xD6336Cu);
    check(near(D->accent_ink, 0xFF8CB4u), "wm-accent", "粉 dark ink %06x, want %06x", D->accent_ink, 0xFF8CB4u);
    check(near(D->accent_hover, logit_rgb_mix(0xD6336Cu, 0xFFFFFF, 8)), "wm-accent", "粉 dark hover %06x, want %06x",
          D->accent_hover, logit_rgb_mix(0xD6336Cu, 0xFFFFFF, 8));
    check(D->accent_focus_ring == LOGIT_ARGB(LOGIT_ACCENT_FOCUS_A_D, D->accent), "wm-accent",
          "粉 dark focus ring %08x, want %08x", D->accent_focus_ring, LOGIT_ARGB(LOGIT_ACCENT_FOCUS_A_D, D->accent));
    check(same_non_accent(D, &LOGIT_TOK_DARK), "wm-tok-fields", "dark: a non-accent role moved (%x %x)", 0, 0);

    /* 石墨 has two fills: each theme takes its own */
    g_accent = 0x5F5F6Bu;
    wm_theme_refresh();
    g_dark = 1;
    check(near(wm_tok()->accent, 0x6A6A76u), "wm-accent", "石墨 dark fill %06x, want %06x", wm_tok()->accent, 0x6A6A76u);

    /* a custom light colour: clamped until white on it is >= 4.5:1 */
    g_accent = 0x7FD4FFu;
    wm_theme_refresh();
    for (int d = 0; d < 2; d++) {
        g_dark = d;
        unsigned f = wm_tok()->accent;
        check(logit_contrast_at_least(0xFFFFFF, f, 45, 10), "wm-accent", "custom %06x: white on %06x below 4.5:1",
              0x7FD4FFu, f);
    }

    /* back to the default: the tables again */
    g_accent = 0x5E5CE6u;
    wm_theme_refresh();
    g_dark = 0;
    check(!memcmp(wm_tok(), &LOGIT_TOK_LIGHT, sizeof LOGIT_TOK_LIGHT), "wm-tok-flag", "light after reset %x %x", 0, 0);

    if (bad) { printf("ui-wm-abi-theme: FAILED\n"); return 1; }
    printf("ui-wm-abi-theme: ok (flag, 粉 row, 石墨, custom clamp, legacy default, generations)\n");
    return 0;
}
