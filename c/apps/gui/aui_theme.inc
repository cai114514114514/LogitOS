/* aui_theme.inc -- theme tokens: the light/dark tables, aui_ensure, set_dark,
 * theme_override, is_dark, set_accent and the hsl/mix/shade helpers.
 * Unity-included by aui.c (one translation unit): every static it names is
 * aui.c's, and it may use anything aui.c defines before the include. Code
 * moved verbatim from aui.c (ui-aui-core); new code is marked where added. */

/* ---------------------------------------------------------------- 2. theme */

static int theme_dark, theme_inited;
static int theme_override = -1;
struct aui_theme aui_t;

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

/* First use: adopt the current SYSTEM theme (SYS_UI_DARK) so even the first token
 * read / window clear matches a desktop that is already dark. */
void aui_ensure(void)
{
    if (!theme_inited) {
        theme_dark = sys_ui_dark(-1) > 0;
        theme_dark ? load_dark() : load_light();
        theme_inited = 1;
    }
}

void aui_set_dark(int on)
{
    theme_dark = on; theme_inited = 1;
    if (on) load_dark(); else load_light();
}
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

void aui_set_accent(unsigned color) { aui_ensure(); aui_t.accent = color; aui_t.focus = color; }
