/* aui_theme.inc -- theme tokens: the light/dark tables, aui_ensure, set_dark,
 * theme_override, is_dark, set_accent and the hsl/mix/shade helpers.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* ---------------------------------------------------------------- 2. theme */

static int theme_dark, theme_inited;
static int theme_override = -1;
struct aui_theme aui_t;
/* The 「澄」 token set behind aui_tok() and the appended aui_t roles. */
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

static void load_light(void)
{
    aui_t.bg          = rgb(244, 245, 248);
    aui_t.surface     = rgb(255, 255, 255);
    aui_t.face        = rgb(232, 234, 240);
    aui_t.text        = rgb(40, 42, 50);
    aui_t.muted       = rgb(140, 144, 154);
    aui_t.border      = rgb(206, 208, 216);
    aui_t.hi          = rgb(255, 255, 255);
    aui_t.accent      = rgb(64, 130, 246);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(52, 199, 89);
    aui_t.warning     = rgb(255, 179, 64);
    aui_t.error       = rgb(255, 69, 58);
    aui_t.focus       = rgb(64, 130, 246);
    aui_t.surface_2   = rgb(255, 255, 255);
    aui_t.face_hover  = rgb(222, 225, 233);
    aui_t.face_active = rgb(206, 210, 220);
    aui_t.track       = rgb(219, 222, 230);
    aui_t.thumb       = rgb(255, 255, 255);
    aui_t.disabled    = rgb(236, 237, 241);
    aui_t.disabled_tx = rgb(178, 181, 190);
    aui_t.scrim       = rgb(18, 20, 28);
    aui_t.shadow      = rgb(28, 32, 48);
    aui_t.selection   = rgb(180, 208, 255);
}

static void load_dark(void)
{
    aui_t.bg          = rgb(28, 28, 32);
    aui_t.surface     = rgb(44, 44, 52);
    aui_t.face        = rgb(58, 58, 68);
    aui_t.text        = rgb(236, 237, 242);
    aui_t.muted       = rgb(146, 148, 160);
    aui_t.border      = rgb(72, 74, 86);
    aui_t.hi          = rgb(84, 86, 98);
    aui_t.accent      = rgb(94, 150, 255);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(48, 209, 88);
    aui_t.warning     = rgb(255, 190, 84);
    aui_t.error       = rgb(255, 92, 82);
    aui_t.focus       = rgb(94, 150, 255);
    aui_t.surface_2   = rgb(56, 57, 66);
    aui_t.face_hover  = rgb(72, 73, 85);
    aui_t.face_active = rgb(88, 90, 104);
    aui_t.track       = rgb(70, 72, 84);
    aui_t.thumb       = rgb(226, 228, 236);
    aui_t.disabled    = rgb(48, 49, 57);
    aui_t.disabled_tx = rgb(104, 106, 118);
    aui_t.scrim       = rgb(0, 0, 0);
    aui_t.shadow      = rgb(0, 0, 0);
    aui_t.selection   = rgb(46, 86, 152);
}

/* The new roles (added by ui-aui-core): the mode's table, then the accent
 * roles derived from the CURRENT aui_t.accent, then the appended aui_t fields.
 * The table is copied a word at a time through a volatile pointer: a struct
 * assignment may lower to a memcpy call, and aui.o links no libc. */
static void tok_refresh(void)
{
    const uint32_t *src = (const uint32_t *)logit_tok(theme_dark);
    volatile uint32_t *dst = (volatile uint32_t *)&tok_cur;
    for (unsigned i = 0; i < sizeof tok_cur / sizeof(uint32_t); i++) dst[i] = src[i];
    logit_tokens_derive(aui_t.accent, theme_dark, &tok_cur);
    const struct logit_tokens *k = &tok_cur;
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

/* First use: adopt the current SYSTEM theme (SYS_UI_DARK) so even the first token
 * read / window clear matches a desktop that is already dark. */
void aui_ensure(void)
{
    if (!theme_inited) {
        theme_dark = sys_ui_dark(-1) > 0;
        theme_dark ? load_dark() : load_light();
        theme_inited = 1;
        tok_refresh();
    }
}

void aui_set_dark(int on)
{
    theme_dark = on; theme_inited = 1;
    if (on) load_dark(); else load_light();
    tok_refresh();
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

void aui_set_accent(unsigned color)
{
    aui_ensure(); aui_t.accent = color; aui_t.focus = color;
    tok_refresh();                      /* the derived accent roles follow (added) */
}
