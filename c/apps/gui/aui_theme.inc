/* aui_theme.inc -- theme tokens: every aui_t role from the 「澄」 tokens
 * (logit_tokens.h) for the mode and the ui.accent base, aui_ensure, set_dark,
 * theme_override, is_dark, set_accent and the hsl/mix/shade helpers.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. */

/* ---------------------------------------------------------------- 2. theme */

static int theme_dark, theme_inited;
static int theme_override = -1;
struct aui_theme aui_t;
/* The 「澄」 token set behind aui_tok() and every aui_t role. */
static struct logit_tokens tok_cur;

/* The channel lerp lives in the engine (gfx_mix): a gradient strip built by
 * gfx and a two-tone token blended here have to land on the same byte, or a
 * card's fill and the top of its own gradient differ by one and show a seam. */
unsigned aui_mix(unsigned a, unsigned b, int t) { return gfx_mix(a, b, t); }

unsigned aui_shade(unsigned c, int d)
{
    int r = iclamp((int)((c >> 16) & 255) + d, 0, 255);
    int g = iclamp((int)((c >> 8) & 255) + d, 0, 255);
    int b = iclamp((int)(c & 255) + d, 0, 255);
    return rgb(r, g, b);
}

/* ---- the accent base (design 3.1.4, P3) ----
 * `ui.accent` is the user's base colour; every accent role is DERIVED from it
 * for the current mode by logit_tokens_derive() (the WM calls the same
 * function), so a base survives a theme flip and a custom colour is clamped
 * to white-on-fill >= 4.5:1. The base is read with the call Settings itself
 * uses (settings.c: setting_int("ui.accent", ...)).
 *
 * 0x5E96FF is the kernel schema's pre-「澄」 default (c/kernel/core/settings.c,
 * "ui.accent" row): a store nobody has written answers it, and it is not one
 * of the swatches anyone can pick, so it means "never chosen" and reads as the
 * new default, 靛 indigo. THE CONTROL: -DUI_THEME_NEGCTL keeps the old blue
 * default (and the old store value) and tests/ui-aui-theme.mk must report
 * 'FAIL default-accent'. */
#ifdef UI_THEME_NEGCTL
#define AUI_ACCENT_DEFAULT_ 0x4082F6u          /* (64,130,246): the pre-「澄」 light accent */
#else
#define AUI_ACCENT_DEFAULT_ LOGIT_ACCENT_DEFAULT
#endif
#define AUI_ACCENT_LEGACY_STORE 0x5E96FFu
#define AUI_ACCENT_POLL_MS 500                 /* aui_begin re-reads ui.accent at most this often */

static unsigned accent_base = AUI_ACCENT_DEFAULT_;
static unsigned accent_stored = 0xFFFFFFFFu;   /* the store's value at the last read; ~0 = never */
static unsigned accent_poll_ms;
static int accent_polled;

static unsigned accent_norm(unsigned c)
{
    c &= 0xFFFFFFu;
#ifndef UI_THEME_NEGCTL
    if (c == AUI_ACCENT_LEGACY_STORE) c = LOGIT_ACCENT_DEFAULT;
#endif
    return c;
}

/* Read ui.accent (one syscall) at `now`. Edge-triggered: the base moves only
 * when the STORED value changed since the last read, so an app that previews
 * its own accent with aui_set_accent (Settings, every frame) is not reset to
 * the store every 500 ms. Returns 1 when the base moved. */
static int accent_poll(unsigned now)
{
    unsigned v = accent_norm((unsigned)setting_int("ui.accent", (int)AUI_ACCENT_DEFAULT_));
    accent_polled = 1; accent_poll_ms = now;
    if (v == accent_stored) return 0;
    accent_stored = v;
    if (v == accent_base) return 0;
    accent_base = v;
    return 1;
}

/* A translucent token flattened over the surface it is specified against: the
 * legacy roles are opaque 0xRRGGBB (a window-surface write must keep the high
 * byte clear: it is the backdrop marker). */
static unsigned tok_over(unsigned argb, unsigned under)
{ return aui_mix(under & 0xFFFFFFu, argb & 0xFFFFFFu, (int)(argb >> 24)); }

/* Dark-mode legacy roles with no token of their own (design 3.1, ui-aui-theme). */
#define AUI_HI_D_ARGB  0x14FFFFFFu             /* hi:    #FFFFFF @ 20 */
#define AUI_THUMB_L    0xFFFFFFu               /* thumb: #FFFFFF light */
#define AUI_THUMB_D    0x5A5A64u               /*        #5A5A64 dark  */

/* The whole palette for (theme_dark, accent_base): the mode's token table, the
 * accent roles derived from the base, then every aui_t field from them. The
 * 23 legacy roles map onto the tokens (design 3.1, the W1 exit "every aui app
 * adopts the palette"):
 *   bg window | surface content | surface_2 raised | face/hover/active
 *   fill.control (+ fill.hover / fill.pressed over it) | text label.primary |
 *   muted label.secondary | border sep.strong over bg.content | hi white /
 *   white @ 20 | accent, focus accent | accent_text label.onAccent |
 *   success, warning, error 3.1.5 | track fill.control | thumb | disabled
 *   fill.control | disabled_tx label.quaternary | scrim black | shadow the
 *   shadow ink | selection accent.selText over bg.content.
 * The table is copied a word at a time through a volatile pointer: a struct
 * assignment may lower to a memcpy call, and aui.o links no libc. */
static void theme_load(void)
{
    const uint32_t *src = (const uint32_t *)logit_tok(theme_dark);
    volatile uint32_t *dst = (volatile uint32_t *)&tok_cur;
    for (unsigned i = 0; i < sizeof tok_cur / sizeof(uint32_t); i++) dst[i] = src[i];
    logit_tokens_derive(accent_base, theme_dark, &tok_cur);
    const struct logit_tokens *k = &tok_cur;
    aui_t.bg          = k->bg_window;
    aui_t.surface     = k->bg_content;
    aui_t.face        = k->fill_control;
    aui_t.text        = k->label_primary;
    aui_t.muted       = k->label_secondary;
    aui_t.border      = tok_over(k->sep_strong, k->bg_content);
    aui_t.hi          = theme_dark ? tok_over(AUI_HI_D_ARGB, k->bg_content) : 0xFFFFFFu;
    aui_t.accent      = k->accent;
    aui_t.accent_text = k->label_on_accent;
    aui_t.success     = k->success;
    aui_t.warning     = k->warning;
    aui_t.error       = k->error;               /* light (255,69,58): the AUI_ERROR pin */
    aui_t.focus       = k->accent;
    aui_t.surface_2   = k->bg_raised;
    aui_t.face_hover  = tok_over(k->fill_hover, k->fill_control);
    aui_t.face_active = tok_over(k->fill_pressed, k->fill_control);
    aui_t.track       = k->fill_control;
    aui_t.thumb       = theme_dark ? AUI_THUMB_D : AUI_THUMB_L;
    aui_t.disabled    = k->fill_control;
    aui_t.disabled_tx = k->label_quaternary;
    aui_t.scrim       = 0x000000u;
    aui_t.shadow      = k->shadow_ink;
    aui_t.selection   = tok_over(k->accent_sel_text, k->bg_content);
    aui_t.bg_window = k->bg_window;       aui_t.bg_content = k->bg_content;
    aui_t.bg_card = k->bg_card;           aui_t.bg_sidebar = k->bg_sidebar;
    aui_t.bg_inspector = k->bg_inspector; aui_t.bg_toolbar = k->bg_toolbar;
    aui_t.bg_raised = k->bg_raised;       aui_t.bg_sunken = k->bg_sunken;
    aui_t.bg_stage = k->bg_stage;
    aui_t.fill_control = k->fill_control; aui_t.fill_hover = k->fill_hover;
    aui_t.fill_pressed = k->fill_pressed; aui_t.fill_zebra = k->fill_zebra;
    aui_t.fill_inactive_sel = k->fill_inactive_sel;
    aui_t.label_primary = k->label_primary;       aui_t.label_secondary = k->label_secondary;
    aui_t.label_tertiary = k->label_tertiary;     aui_t.label_quaternary = k->label_quaternary;
    aui_t.label_on_accent = k->label_on_accent;
    aui_t.sep_hairline = k->sep_hairline; aui_t.sep_opaque = k->sep_opaque;
    aui_t.sep_strong = k->sep_strong;
    aui_t.accent_hover = k->accent_hover; aui_t.accent_pressed = k->accent_pressed;
    aui_t.accent_ink = k->accent_ink;     aui_t.accent_tint = k->accent_tint;
    aui_t.accent_focus_ring = k->accent_focus_ring;
    aui_t.accent_sel_text = k->accent_sel_text;
    aui_t.success_ink = k->success_ink;   aui_t.warning_ink = k->warning_ink;
    aui_t.error_ink = k->error_ink;       aui_t.destructive_fill = k->destructive_fill;
    aui_t.shadow_ink = k->shadow_ink;
}

/* First use: adopt the current SYSTEM theme (SYS_UI_DARK) and the stored
 * ui.accent, so even the first token read / window clear matches the desktop. */
void aui_ensure(void)
{
    if (!theme_inited) {
        theme_dark = sys_ui_dark(-1) > 0;
        theme_inited = 1;
        accent_poll((unsigned)monotonic_ms());
        theme_load();
    }
}

void aui_set_dark(int on)
{
    if (!theme_inited) { theme_inited = 1; accent_poll((unsigned)monotonic_ms()); }
    theme_dark = !!on;
    theme_load();
}

const struct logit_tokens *aui_tok(void) { aui_ensure(); return &tok_cur; }
int aui_is_dark(void) { return theme_dark; }
void aui_theme_override(int mode)
{
    theme_override = mode < 0 ? -1 : !!mode;
    aui_set_dark(theme_override < 0 ? sys_ui_dark(-1) > 0 : theme_override);
}

/* HSL -> packed rgb, integer-only. h:0..359, s,l:0..100. Channels are carried in
 * "percent" (0..100) through the standard piecewise formula, then scaled to 8-bit:
 *   C = (1-|2L-1|)*S,  X = C*(triangular wave over the sextant),  m = L - C/2. */
unsigned aui_hsl(int h, int s, int l)
{
    h = iclamp(h, 0, 359); s = iclamp(s, 0, 100); l = iclamp(l, 0, 100);
    int a = (l >= 50) ? (2 * l - 100) : (100 - 2 * l);   /* |2L-1| in % */
    int C = (100 - a) * s / 100;                          /* chroma, 0..100 */
    int seg = h / 60, frac = h % 60;                      /* sextant + position */
    int X = (seg & 1) ? C * (60 - frac) / 60 : C * frac / 60;
    int r1 = 0, g1 = 0, b1 = 0;
    switch (seg) {
        case 0: r1 = C; g1 = X; break;
        case 1: r1 = X; g1 = C; break;
        case 2: g1 = C; b1 = X; break;
        case 3: g1 = X; b1 = C; break;
        case 4: r1 = X; b1 = C; break;
        default: r1 = C; b1 = X; break;
    }
    int m = l - C / 2;                                    /* lightness offset, % */
    return rgb(iclamp((r1 + m) * 255 / 100, 0, 255),
               iclamp((g1 + m) * 255 / 100, 0, 255),
               iclamp((b1 + m) * 255 / 100, 0, 255));
}

/* An app's own accent (Settings previews the one being edited). It becomes
 * the base and goes through logit_tokens_derive like ui.accent: the accent
 * and focus fields are the DERIVED fill for the mode, never the raw colour. */
void aui_set_accent(unsigned color)
{
    aui_ensure();
    color = accent_norm(color);
    if (color == accent_base) return;   /* Settings calls this every frame */
    accent_base = color;
    theme_load();
}
