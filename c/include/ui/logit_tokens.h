/* logit_tokens.h -- the 「澄」 design tokens, one source for the WM, aui and
 * every app (design_language.md section 3, appendix A; prerequisite P3).
 *
 * FREESTANDING AND HEADER-ONLY. No libc, no float, no mutable storage: the
 * tables are `static const` and the helpers `static inline`, so the kernel
 * (CFLAGS) and ring 3 (UCFLAGS) include the same file and agree bit for bit.
 * Including it changes no object until something references a table.
 *
 * COLOUR ENCODING. A field without a note is opaque 0xRRGGBB with the high
 * byte 0 (safe to write into a window surface, whose high byte is the
 * backdrop marker). A field marked ARGB carries its alpha in the high byte,
 * exactly the "#RRGGBB @ A" of the design tables. Material colours
 * (struct logit_material) are always full ARGB, 0xFF = opaque.
 *
 * Tests: tests/unit/ui_tokens_test.c (WCAG contrast of every claim in 3.1.2,
 * 3.1.4 and 3.1.5, the "≈" composites, derive() per swatch, materials). */
#ifndef LOGIT_UI_TOKENS_H
#define LOGIT_UI_TOKENS_H

#include <stdint.h>

#define LOGIT_ARGB(a, rgb) ((((uint32_t)(a) & 0xFFu) << 24) | ((uint32_t)(rgb) & 0xFFFFFFu))
#define LOGIT_RGB_OF(c)    ((uint32_t)(c) & 0xFFFFFFu)
#define LOGIT_ALPHA_OF(c)  (((uint32_t)(c) >> 24) & 0xFFu)

/* ------------------------------------------------------------ 3.1 colour */

struct logit_tokens {               /* one instance for light, one for dark */
    uint32_t bg_window, bg_content, bg_card, bg_sidebar, bg_inspector,
             bg_toolbar, bg_raised, bg_sunken, bg_stage;
    uint32_t fill_control, fill_hover /*ARGB*/, fill_pressed /*ARGB*/,
             fill_zebra, fill_inactive_sel;
    uint32_t label_primary, label_secondary, label_tertiary,
             label_quaternary, label_on_accent;
    uint32_t sep_hairline /*ARGB*/, sep_opaque, sep_strong /*ARGB*/;
    uint32_t accent, accent_hover, accent_pressed, accent_ink,
             accent_tint /*ARGB*/, accent_focus_ring /*ARGB*/, accent_sel_text /*ARGB*/;
    uint32_t success, success_ink, warning, warning_ink, error, error_ink,
             destructive_fill;
    uint32_t shadow_ink;
};

/* The negative control of test-ui-tokens: label.tertiary L takes the
 * quaternary grey, which fails its 3.3:1 claim on #FFFFFF. */
#ifdef UI_TOKENS_NEGCTL
#define LOGIT__LABEL_TERTIARY_L 0xBDBDC6u
#else
#define LOGIT__LABEL_TERTIARY_L 0x8C8C97u
#endif

static const struct logit_tokens LOGIT_TOK_LIGHT = {
    /* 3.1.1 surfaces */
    .bg_window = 0xF4F4F8, .bg_content = 0xFFFFFF, .bg_card = 0xFFFFFF,
    .bg_sidebar = 0xECECF1, .bg_inspector = 0xF7F7FA, .bg_toolbar = 0xF7F7FA,
    .bg_raised = 0xFFFFFF, .bg_sunken = 0xEDEDF2, .bg_stage = 0x1C1C20,
    .fill_control = 0xE9E9EE, .fill_hover = 0x0D000000 /* #000000 @ 13 */,
    .fill_pressed = 0x1A000000 /* #000000 @ 26 */,
    .fill_zebra = 0xF7F7FA, .fill_inactive_sel = 0xE3E3E9,
    /* 3.1.2 labels */
    .label_primary = 0x1D1D23, .label_secondary = 0x5E5E6A,
    .label_tertiary = LOGIT__LABEL_TERTIARY_L, .label_quaternary = 0xBDBDC6,
    .label_on_accent = 0xFFFFFF,
    /* 3.1.3 separators */
    .sep_hairline = 0x1A141428 /* #141428 @ 26 */, .sep_opaque = 0xE4E4EA,
    .sep_strong = 0x2E141428 /* #141428 @ 46 */,
    /* 3.1.4 accent (default 靛 indigo) */
    .accent = 0x5E5CE6, .accent_hover = 0x5250DA, .accent_pressed = 0x4846C8,
    .accent_ink = 0x4D4BD4,
    .accent_tint = 0x295E5CE6 /* @ 41 */, .accent_focus_ring = 0x805E5CE6 /* @ 128 */,
    .accent_sel_text = 0x385E5CE6 /* @ 56 */,
    /* 3.1.5 semantic */
    .success = 0x1F9D55, .success_ink = 0x1B7F45, .warning = 0xE08A00,
    .warning_ink = 0xA85A00, .error = 0xFF453A, .error_ink = 0xD92D20,
    .destructive_fill = 0xD92D20,
    /* 3.1.6 */
    .shadow_ink = 0x140F30,
};

static const struct logit_tokens LOGIT_TOK_DARK = {
    .bg_window = 0x1E1E24, .bg_content = 0x19191E, .bg_card = 0x26262D,
    .bg_sidebar = 0x26262D, .bg_inspector = 0x202026, .bg_toolbar = 0x232329,
    .bg_raised = 0x2C2C34, .bg_sunken = 0x141418, .bg_stage = 0x1C1C20,
    .fill_control = 0x33333B, .fill_hover = 0x0FFFFFFF /* #FFFFFF @ 15 */,
    .fill_pressed = 0x1FFFFFFF /* #FFFFFF @ 31 */,
    .fill_zebra = 0x1D1D23, .fill_inactive_sel = 0x34343C,
    .label_primary = 0xF2F2F6, .label_secondary = 0xADADB8,
    .label_tertiary = 0x7D7D89, .label_quaternary = 0x4E4E58,
    .label_on_accent = 0xFFFFFF,
    .sep_hairline = 0x18FFFFFF /* #FFFFFF @ 24 */, .sep_opaque = 0x34343C,
    .sep_strong = 0x24FFFFFF /* #FFFFFF @ 36 */,
    .accent = 0x6563EE, .accent_hover = 0x716FF2, .accent_pressed = 0x5755DE,
    .accent_ink = 0xA5A3FF,
    .accent_tint = 0x426563EE /* @ 66 */, .accent_focus_ring = 0x966563EE /* @ 150 */,
    .accent_sel_text = 0x606563EE /* @ 96 */,
    .success = 0x34D17A, .success_ink = 0x5FD98C, .warning = 0xFFB340,
    .warning_ink = 0xFFB340, .error = 0xFF6961, .error_ink = 0xFF6961,
    .destructive_fill = 0xD9342A,
    .shadow_ink = 0x000000,
};

static inline const struct logit_tokens *logit_tok(int dark)
{
    return dark ? &LOGIT_TOK_DARK : &LOGIT_TOK_LIGHT;
}

/* `destructive` (3.1.5) is the same colour as error.ink. */
#define LOGIT_DESTRUCTIVE(t) ((t)->error_ink)
/* `*.tint` badge backgrounds: the colour @ 31 (light) / @ 51 (dark). */
#define LOGIT_SEM_TINT_A_L 31u
#define LOGIT_SEM_TINT_A_D 51u
static inline uint32_t logit_sem_tint(uint32_t rgb, int dark)
{
    return LOGIT_ARGB(dark ? LOGIT_SEM_TINT_A_D : LOGIT_SEM_TINT_A_L, rgb);
}

/* Accent alphas (3.1.4), shared by the table above and logit_tokens_derive. */
#define LOGIT_ACCENT_TINT_A_L   41u
#define LOGIT_ACCENT_TINT_A_D   66u
#define LOGIT_ACCENT_FOCUS_A_L 128u
#define LOGIT_ACCENT_FOCUS_A_D 150u
#define LOGIT_ACCENT_SEL_A_L    56u
#define LOGIT_ACCENT_SEL_A_D    96u

/* Pinned shell constants (3.1.5, design 8): tests find them by colour. */
#define LOGIT_PIN_LIGHT_CLOSE   0xFF5F56u   /* (255,95,86)  */
#define LOGIT_PIN_LIGHT_MIN     0xFEBC2Eu   /* (254,188,46) */
#define LOGIT_PIN_LIGHT_ZOOM    0x28C840u   /* (40,200,64)  */
#define LOGIT_PIN_NOTIFY_INFO   0x4094FFu   /* (64,148,255) */
#define LOGIT_PIN_NOTIFY_WARN   0xFFAC38u   /* (255,172,56) */
#define LOGIT_PIN_NOTIFY_ERROR  0xFF4C5Cu   /* (255,76,92)  */
#define LOGIT_PIN_RT_MENUBAR_D  0x181820u   /* (24,24,32)   */
#define LOGIT_PIN_IMG_BG        0x1C1C20u   /* (28,28,32), Preview stage */
#define LOGIT_PIN_STUDIO_SEL_D  0x263E58u   /* (38,62,88)   */
#define LOGIT_PIN_CURSOR        0x14141Au   /* (20,20,26)   */
#define LOGIT_PIN_AUI_ERROR     0xFF453Au   /* (255,69,58) == error L */

/* 3.1.6 brand: the Logit gradient, linear 135°, three stops (pos in %).
 * Only for the Logit mark tile, the Logit 助手 icon, AI buttons, the
 * Assistant orb, the Greeter avatar ring and the About hero tile. */
#define LOGIT_BRAND_ANGLE_DEG 135
#define LOGIT_BRAND_STOP0 0x7C5CFFu
#define LOGIT_BRAND_STOP1 0x4F7BFFu
#define LOGIT_BRAND_STOP1_POS 55
#define LOGIT_BRAND_STOP2 0x38A8FFu

/* --------------------------------------------- 3.1.4 accent swatch table */

struct logit_accent_swatch {
    const char *zh;                 /* UTF-8 name shown in Settings → 外观 */
    uint32_t fill_l, fill_d, ink_l, ink_d;
};
enum { LOGIT_SWATCH_INDIGO, LOGIT_SWATCH_BLUE, LOGIT_SWATCH_PURPLE, LOGIT_SWATCH_PINK,
       LOGIT_SWATCH_RED, LOGIT_SWATCH_ORANGE, LOGIT_SWATCH_GREEN, LOGIT_SWATCH_TEAL,
       LOGIT_SWATCH_GRAPHITE, LOGIT_SWATCH_COUNT };
#define LOGIT_ACCENT_DEFAULT 0x5E5CE6u   /* 靛, the ui.accent default */

static const struct logit_accent_swatch LOGIT_ACCENT_SWATCHES[LOGIT_SWATCH_COUNT] = {
    { "靛",   0x5E5CE6, 0x6563EE, 0x4D4BD4, 0xA5A3FF },
    { "蓝",   0x1F6FEB, 0x1F6FEB, 0x1A62D6, 0x79AEFF },
    { "紫",   0x8A4FE0, 0x8A4FE0, 0x7A3FD0, 0xC3A2FF },
    { "粉",   0xD6336C, 0xD6336C, 0xC22A5E, 0xFF8CB4 },
    { "红",   0xD9342A, 0xD9342A, 0xC6312A, 0xFF8A80 },
    { "橙",   0xB25E00, 0xB25E00, 0xA85A00, 0xFFB45C },
    { "绿",   0x1B7F45, 0x1B7F45, 0x18793F, 0x5FD98C },
    { "青",   0x0E8577, 0x0E8577, 0x0B7468, 0x4FD6C4 },
    { "石墨", 0x5F5F6B, 0x6A6A76, 0x55555F, 0xB8B8C4 },
};

/* ------------------------------------------- integer colour arithmetic */

/* sRGB 8-bit -> linear light, 0..65535 (WCAG 2 transfer function). */
static const uint16_t LOGIT_SRGB_LIN[256] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219,
    241, 264, 288, 313, 340, 367, 396, 427, 458, 491, 526, 562,
    599, 637, 677, 718, 761, 805, 851, 898, 947, 997, 1048, 1101,
    1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
    1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866,
    2961, 3058, 3157, 3258, 3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129,
    4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124, 5257, 5392, 5530, 5669,
    5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
    7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635,
    9828, 10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874,
    15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481,
    21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542,
    29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138,
    39572, 40009, 40449, 40891, 41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359, 48850, 49344, 49841, 50341,
    50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221,
    63795, 64372, 64952, 65535,
};

/* Relative luminance in units of 1/(10000*65535): 0 .. 655,350,000. */
static inline uint32_t logit_luma_raw(uint32_t rgb)
{
    return 2126u * LOGIT_SRGB_LIN[(rgb >> 16) & 0xFF] + 7152u * LOGIT_SRGB_LIN[(rgb >> 8) & 0xFF]
         + 722u * LOGIT_SRGB_LIN[rgb & 0xFF];
}
/* WCAG contrast(a, b) >= num/den, exact in integers ((L1+.05)/(L2+.05)). */
static inline int logit_contrast_at_least(uint32_t a, uint32_t b, unsigned num, unsigned den)
{
    uint64_t la = logit_luma_raw(a), lb = logit_luma_raw(b);
    const uint64_t k = 32767500u;          /* 0.05 in the same units */
    if (la < lb) { uint64_t t = la; la = lb; lb = t; }
    return (la + k) * den >= (lb + k) * num;
}

static inline uint32_t logit_rgb_pack(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
/* Straight RGB mix: pct % of `with` over `c`, rounded half up. */
static inline uint32_t logit_rgb_mix(uint32_t c, uint32_t with, int pct)
{
    int out[3];
    for (int i = 0; i < 3; i++) {
        int s = 16 - 8 * i;
        int a = (int)((c >> s) & 0xFF), b = (int)((with >> s) & 0xFF);
        out[i] = (a * (100 - pct) + b * pct + 50) / 100;
    }
    return logit_rgb_pack(out[0], out[1], out[2]);
}
/* Integer HSL lightness, as l2 = max + min (0..510; HSL L = l2 / 510). */
static inline int logit_hsl_l2(uint32_t rgb)
{
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx + mn;
}
/* The colour with the same HSL hue and saturation at lightness l2. Chroma
 * scales with (255 - |l2 - 255|), the HSL saturation denominator; each
 * channel keeps its position between min and max, so hue is unchanged. */
static inline uint32_t logit_hsl_with_l2(uint32_t rgb, int l2)
{
    int c[3] = { (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF) };
    int mx = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
    int mn = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
    int l2o = mx + mn, chroma = mx - mn;
    if (l2 < 0) l2 = 0;
    if (l2 > 510) l2 = 510;
    int den = 255 - (l2o > 255 ? l2o - 255 : 255 - l2o);
    int num = 255 - (l2 > 255 ? l2 - 255 : 255 - l2);
    int64_t c2x2 = den > 0 ? (int64_t)2 * chroma * num / den : 0;  /* 2 * new chroma */
    int out[3];
    for (int i = 0; i < 3; i++) {
        /* new = min' + (c - min) * C' / C, with min' = (l2 - C') / 2, in halves */
        int64_t pos = chroma > 0 ? (int64_t)(c[i] - mn) * c2x2 / chroma : 0;
        int64_t v2 = (int64_t)l2 - c2x2 / 2 + pos;   /* twice the channel */
        out[i] = (int)((v2 + 1) / 2);
    }
    return logit_rgb_pack(out[0], out[1], out[2]);
}

/* -------------------------------------- 3.1.4 accent derivation (ui.accent) */

/* The derivation column of 3.1.4 for an arbitrary base, in integer HSL:
 *   accent       base, HSL lightness lowered until white on it is >= 4.5:1;
 *   hover        L: mix 8 % black      D: mix 8 % white;
 *   pressed      mix 16 % black;
 *   ink          L: fill - 6 % L, then lowered until >= 4.5:1 on bg.content;
 *                D: fill + 22 % L, then raised until >= 4.5:1 on bg.content;
 *   tint / focusRing / selText   accent @ 41/66, 128/150, 56/96.
 * The nine named swatches carry designer-tuned values that this rule does not
 * reproduce (see logit_tokens_derive); the rule is for custom accents. */
static inline void logit_tokens_derive_rule(uint32_t base_rgb, int dark, struct logit_tokens *out)
{
    const struct logit_tokens *t = logit_tok(dark);
    uint32_t fill = LOGIT_RGB_OF(base_rgb);
    int l2 = logit_hsl_l2(fill);
    while (!logit_contrast_at_least(0xFFFFFF, fill, 45, 10) && l2 > 0)
        fill = logit_hsl_with_l2(base_rgb, --l2);
    int fl2 = logit_hsl_l2(fill);
    uint32_t ink;
    if (dark) {
        int il2 = fl2 + (22 * 510 + 50) / 100;
        ink = logit_hsl_with_l2(fill, il2);
        while (!logit_contrast_at_least(ink, t->bg_content, 45, 10) && il2 < 510)
            ink = logit_hsl_with_l2(fill, ++il2);
    } else {
        int il2 = fl2 - (6 * 510 + 50) / 100;
        ink = logit_hsl_with_l2(fill, il2);
        while (!logit_contrast_at_least(ink, t->bg_content, 45, 10) && il2 > 0)
            ink = logit_hsl_with_l2(fill, --il2);
    }
    out->accent = fill;
    out->accent_hover = dark ? logit_rgb_mix(fill, 0xFFFFFF, 8) : logit_rgb_mix(fill, 0x000000, 8);
    out->accent_pressed = logit_rgb_mix(fill, 0x000000, 16);
    out->accent_ink = ink;
    out->accent_tint = LOGIT_ARGB(dark ? LOGIT_ACCENT_TINT_A_D : LOGIT_ACCENT_TINT_A_L, fill);
    out->accent_focus_ring = LOGIT_ARGB(dark ? LOGIT_ACCENT_FOCUS_A_D : LOGIT_ACCENT_FOCUS_A_L, fill);
    out->accent_sel_text = LOGIT_ARGB(dark ? LOGIT_ACCENT_SEL_A_D : LOGIT_ACCENT_SEL_A_L, fill);
}

/* Index of the swatch whose light or dark fill is base_rgb, or -1. */
static inline int logit_accent_swatch_of(uint32_t base_rgb)
{
    uint32_t b = LOGIT_RGB_OF(base_rgb);
    for (int i = 0; i < LOGIT_SWATCH_COUNT; i++)
        if (LOGIT_ACCENT_SWATCHES[i].fill_l == b || LOGIT_ACCENT_SWATCHES[i].fill_d == b) return i;
    return -1;
}

/* Fills the seven accent_* roles of *out for ui.accent = base_rgb in the
 * given mode (the other fields are left alone, so callers copy the mode's
 * table first). A named swatch (either of its fills) returns the swatch's
 * tuned fill and ink for the mode; the default indigo returns the full 3.1.4
 * role table. Any other base takes logit_tokens_derive_rule. The WM and aui
 * call this same function. */
static inline void logit_tokens_derive(uint32_t base_rgb, int dark, struct logit_tokens *out)
{
    int i = logit_accent_swatch_of(base_rgb);
    if (i < 0) { logit_tokens_derive_rule(base_rgb, dark, out); return; }
    const struct logit_accent_swatch *s = &LOGIT_ACCENT_SWATCHES[i];
    uint32_t fill = dark ? s->fill_d : s->fill_l;
    if (i == LOGIT_SWATCH_INDIGO) {
        const struct logit_tokens *t = logit_tok(dark);
        out->accent = t->accent; out->accent_hover = t->accent_hover;
        out->accent_pressed = t->accent_pressed; out->accent_ink = t->accent_ink;
        out->accent_tint = t->accent_tint; out->accent_focus_ring = t->accent_focus_ring;
        out->accent_sel_text = t->accent_sel_text;
        return;
    }
    logit_tokens_derive_rule(fill, dark, out);   /* hover, pressed, alphas */
    out->accent = fill;                           /* tuned values win */
    out->accent_ink = dark ? s->ink_d : s->ink_l;
}

/* ------------------------------------------------------- 3.2 materials */

enum logit_mat {
    LOGIT_MAT_MENUBAR,    /* menu bar, glass variant (a window touches the band) */
    LOGIT_MAT_MENU,       /* menus, context menus, select popups */
    LOGIT_MAT_DOCK,
    LOGIT_MAT_SPOTLIGHT,
    LOGIT_MAT_ABOUT,
    LOGIT_MAT_SWITCHER,   /* Cmd-Tab */
    LOGIT_MAT_SIDEBAR,    /* floating sidebar (BACKDROP id 1) */
    LOGIT_MAT_POPOVER,
    LOGIT_MAT_HUD_DARK,   /* Preview viewer media HUD */
    LOGIT_MAT_PENDING,    /* Settings pending bar */
    LOGIT_MAT_GREETER,    /* Greeter password capsule */
    LOGIT_MAT_REGULAR,    /* legacy aui_glass() (its tint today), rim SYMMETRIC */
    LOGIT_MAT_TITLE,      /* today's WM titlebar glass, until ui-wm-chrome */
    LOGIT_MAT_COUNT
};

/* Blur tiers and rims mirror openlogit_glass_material.h numerically
 * (OL_GLASS_BLUR_*, OL_GLASS_RIM_*); cut bits mirror OL_GLASS_CUT_*. The
 * unit test asserts the equality, so this header needs no gfx include. */
enum { LOGIT_TIER_REGULAR = 0, LOGIT_TIER_THIN = 1, LOGIT_TIER_THICK = 2, LOGIT_TIER_ULTRA = 3 };
enum { LOGIT_RIM_DIRECTIONAL = 0, LOGIT_RIM_SYMMETRIC = 1 };
#define LOGIT_CUT_TOP    1u
#define LOGIT_CUT_BOTTOM 2u
#define LOGIT_CUT_LEFT   4u
#define LOGIT_CUT_RIGHT  8u
/* radius_pt sentinels */
#define LOGIT_R_CAPSULE  (-1)     /* h / 2 */
#define LOGIT_R_CALLER   (-2)     /* the caller's radius (legacy aui_glass) */

/* The material edge ("Edge" column): a 1 dev-px line in ARGB. */
enum logit_edge_kind {
    LOGIT_EDGE_NONE,
    LOGIT_EDGE_INNER,         /* inner stroke all round */
    LOGIT_EDGE_INNER_TOP,     /* inner highlight along the top only */
    LOGIT_EDGE_BOTTOM,        /* one line under the bottom edge */
};
struct logit_edge {
    uint8_t kind_l, kind_d;   /* enum logit_edge_kind per mode */
    uint32_t argb_l, argb_d;
};

struct logit_material {
    uint8_t tier;             /* LOGIT_TIER_* */
    uint8_t rim;              /* LOGIT_RIM_*: SYMMETRIC everywhere except TITLE */
    uint8_t cut;              /* LOGIT_CUT_* bits */
    uint32_t tint_l, tint_d;  /* ARGB: alpha = material opacity over the blur */
    int16_t radius_pt;        /* pt, or LOGIT_R_CAPSULE / LOGIT_R_CALLER */
    uint32_t rt_l, rt_d;      /* Reduce Transparency solid, ARGB (0xFF = opaque) */
    struct logit_edge edge;
};

/* Today's titlebar alpha when the window is NOT focused (draw_frame). */
#define LOGIT_MAT_TITLE_UNFOCUSED_A_L 140u
#define LOGIT_MAT_TITLE_UNFOCUSED_A_D 180u

#define LOGIT__EDGE_AS_MENUS     { LOGIT_EDGE_INNER, LOGIT_EDGE_INNER, 0x1A000000u, 0x1AFFFFFFu }
#define LOGIT__EDGE_AS_SPOTLIGHT { LOGIT_EDGE_INNER_TOP, LOGIT_EDGE_INNER, 0x5AFFFFFFu, 0x1AFFFFFFu }

static const struct logit_material LOGIT_MATERIALS[LOGIT_MAT_COUNT] = {
    [LOGIT_MAT_MENUBAR]   = { LOGIT_TIER_REGULAR, LOGIT_RIM_SYMMETRIC,
                              LOGIT_CUT_TOP | LOGIT_CUT_LEFT | LOGIT_CUT_RIGHT,
                              0xB0F6F6FAu, 0xB016161Eu, 0, 0xFFF2F2F6u, 0xFF181820u,
                              { LOGIT_EDGE_BOTTOM, LOGIT_EDGE_BOTTOM, 0x1A141428u, 0x18FFFFFFu } },
    [LOGIT_MAT_MENU]      = { LOGIT_TIER_THICK, LOGIT_RIM_SYMMETRIC, 0,
                              0xD0FAFAFCu, 0xD024242Cu, 12, 0xFFFFFFFFu, 0xFF2C2C34u,
                              LOGIT__EDGE_AS_MENUS },
    [LOGIT_MAT_DOCK]      = { LOGIT_TIER_THICK, LOGIT_RIM_SYMMETRIC, 0,
                              0x58FFFFFFu, 0x801C1C24u, 22, 0xFFE8E8EEu, 0xFF26262Du,
                              { LOGIT_EDGE_NONE, LOGIT_EDGE_NONE, 0, 0 } },
    [LOGIT_MAT_SPOTLIGHT] = { LOGIT_TIER_THICK, LOGIT_RIM_SYMMETRIC, 0,
                              0xC4FAFAFDu, 0xC81E1E26u, 22, 0xFFFFFFFFu, 0xFF2C2C34u,
                              LOGIT__EDGE_AS_SPOTLIGHT },
    [LOGIT_MAT_ABOUT]     = { LOGIT_TIER_ULTRA, LOGIT_RIM_SYMMETRIC, 0,
                              0xCCFBFBFDu, 0xD0202028u, 22, 0xFFFFFFFFu, 0xFF2C2C34u,
                              LOGIT__EDGE_AS_SPOTLIGHT },
    [LOGIT_MAT_SWITCHER]  = { LOGIT_TIER_ULTRA, LOGIT_RIM_SYMMETRIC, 0,
                              0xC4F8F8FBu, 0xCC1E1E26u, 24, 0xFFFFFFFFu, 0xFF2C2C34u,
                              LOGIT__EDGE_AS_SPOTLIGHT },
    [LOGIT_MAT_SIDEBAR]   = { LOGIT_TIER_THICK, LOGIT_RIM_SYMMETRIC, 0,
                              0xB0F0F0F6u, 0xBA22222Au, 12, 0xFFECECF1u, 0xFF26262Du,
                              { LOGIT_EDGE_INNER, LOGIT_EDGE_INNER, 0x1A141428u, 0x18FFFFFFu } },
    [LOGIT_MAT_POPOVER]   = { LOGIT_TIER_THICK, LOGIT_RIM_SYMMETRIC, 0,
                              0xD6FAFAFCu, 0xD626262Eu, 14, 0xFFFFFFFFu, 0xFF2C2C34u,
                              LOGIT__EDGE_AS_MENUS },
    [LOGIT_MAT_HUD_DARK]  = { LOGIT_TIER_THIN, LOGIT_RIM_SYMMETRIC, 0,
                              0xB81C1C20u, 0xB81C1C20u, LOGIT_R_CAPSULE, 0xFF1C1C20u, 0xFF1C1C20u,
                              { LOGIT_EDGE_INNER, LOGIT_EDGE_INNER, 0x1AFFFFFFu, 0x1AFFFFFFu } },
    [LOGIT_MAT_PENDING]   = { LOGIT_TIER_THIN, LOGIT_RIM_SYMMETRIC, 0,
                              0xC8FFFFFFu, 0xC82C2C34u, LOGIT_R_CAPSULE, 0xFFFFFFFFu, 0xFF2C2C34u,
                              { LOGIT_EDGE_INNER, LOGIT_EDGE_INNER, 0x14000000u, 0x1AFFFFFFu } },
    [LOGIT_MAT_GREETER]   = { LOGIT_TIER_THIN, LOGIT_RIM_SYMMETRIC, 0,
                              0x40FFFFFFu, 0x40FFFFFFu, LOGIT_R_CAPSULE,
                              0x28FFFFFFu, 0x28FFFFFFu,   /* #FFFFFF @ 40 over a static blur */
                              { LOGIT_EDGE_INNER, LOGIT_EDGE_INNER, 0x2EFFFFFFu, 0x2EFFFFFFu } },
    [LOGIT_MAT_REGULAR]   = { LOGIT_TIER_REGULAR, LOGIT_RIM_SYMMETRIC, 0,
                              0x32FFFFFFu, 0x7822242Eu, LOGIT_R_CALLER, 0xFFFFFFFFu, 0xFF2C2C34u,
                              { LOGIT_EDGE_NONE, LOGIT_EDGE_NONE, 0, 0 } },
    /* Today's draw_frame: REGULAR, directional rim, r 10, focused alpha
     * 104 (L) / 150 (D) over (250,250,255) / (30,30,40); RT draws the tint
     * opaque; the separator draw_titlebar_sep is (214,214,220) / (60,60,70). */
    [LOGIT_MAT_TITLE]     = { LOGIT_TIER_REGULAR, LOGIT_RIM_DIRECTIONAL, 0,
                              0x68FAFAFFu, 0x961E1E28u, 10, 0xFFFAFAFFu, 0xFF1E1E28u,
                              { LOGIT_EDGE_BOTTOM, LOGIT_EDGE_BOTTOM, 0xFFD6D6DCu, 0xFF3C3C46u } },
};

static inline const struct logit_material *logit_material(enum logit_mat m)
{
    return (unsigned)m < LOGIT_MAT_COUNT ? &LOGIT_MATERIALS[m] : &LOGIT_MATERIALS[LOGIT_MAT_REGULAR];
}

/* The menu bar's CLEAR default has no material: RT solids as the glass. */
#define LOGIT_MENUBAR_RT_L 0xF2F2F6u
#define LOGIT_MENUBAR_RT_D 0x181820u
/* Notification card: opaque, not glass (3.2). ARGB. */
#define LOGIT_NOTIFY_CARD_L 0xF2FCFCFEu
#define LOGIT_NOTIFY_CARD_D 0xF226262Du
#define LOGIT_NOTIFY_TOP_EDGE 0x28FFFFFFu   /* top inner 1 dev #FFFFFF @ 40 */

/* ------------------------------------------------------------ 3.4 radii */

#define LOGIT_R_XS      4
#define LOGIT_R_SM      6
#define LOGIT_R_MD      8
#define LOGIT_R_LG     12   /* squircle when opaque */
#define LOGIT_R_XL     14
#define LOGIT_R_2XL    18
#define LOGIT_R_PANEL  22
#define LOGIT_R_WINDOW 20   /* 0 when zoomed */
#define LOGIT_R_TEXTEDIT_PAGE 6
/* r.icon = 0.225 x size, squircle smoothing q8 154 (gfx_path_squircle). */
#define LOGIT_R_ICON_PERMILLE 225
#define LOGIT_SQUIRCLE_Q8     154
static inline int logit_r_icon(int size) { return (size * LOGIT_R_ICON_PERMILLE + 500) / 1000; }

/* ----------------------------------------------- 3.5 spacing and metrics */

#define LOGIT_SP1   4
#define LOGIT_SP2   8
#define LOGIT_SP3  12
#define LOGIT_SP4  16
#define LOGIT_SP5  20
#define LOGIT_SP6  24
#define LOGIT_SP8  32
#define LOGIT_SP10 40
#define LOGIT_SP12 48
#define LOGIT_SP16 64
#define LOGIT_SP(n) (4 * (n))

#define LOGIT_MARGIN_DOC       20   /* forms and documents */
#define LOGIT_MARGIN_LIST      12   /* toolbars and lists */
#define LOGIT_SIDEBAR_INSET     8
#define LOGIT_CARD_PAD_H       16
#define LOGIT_CARD_PAD_V       12
#define LOGIT_GAP_CARDS        12
#define LOGIT_GAP_SECTIONS     24
#define LOGIT_SECTION_LABEL_GAP 8

#define LOGIT_CTL_MINI     20
#define LOGIT_CTL_SMALL    24
#define LOGIT_CTL_REGULAR  28
#define LOGIT_CTL_LARGE    32
#define LOGIT_CTL_XLARGE   36
#define LOGIT_CTL_HERO     44

#define LOGIT_ROW_SIDEBAR       30
#define LOGIT_ROW_SIDEBAR_PILL  28
#define LOGIT_ROW_LIST          28
#define LOGIT_ROW_LIST_DENSE    26   /* Monitor */
#define LOGIT_ROW_FORM          44
#define LOGIT_ROW_FORM_SUB      56
#define LOGIT_ROW_MENU          24
#define LOGIT_ROW_SPOTLIGHT     44
#define LOGIT_ROW_SPOTLIGHT_TOP 52
#define LOGIT_ROW_TWO_LINE      48

#define LOGIT_TOOLBAR_H         38   /* Mode A */
#define LOGIT_TOOLBAR_CTL_Y      5
#define LOGIT_TOOLBAR_H_MODEB   52
#define LOGIT_HIT_MIN           24

/* Pinned shell geometry (design 8), for reference by WM items. */
#define LOGIT_TITLEBAR_H 30
#define LOGIT_MENUBAR_H  24

/* --------------------------------------------------------------- 3.6 type */

enum logit_type_style {
    LOGIT_TS_DISPLAY,          /* 64/500/72 tabular, digits only (Clock) */
    LOGIT_TS_DISPLAY_GREETER,  /* 72/500/80 tabular, digits only */
    LOGIT_TS_LARGE_TITLE,      /* 26/600/34 */
    LOGIT_TS_TITLE1,           /* 22/600/30 */
    LOGIT_TS_TITLE2,           /* 17/600/24 */
    LOGIT_TS_HEADLINE,         /* 14/600/20 */
    LOGIT_TS_BODY,             /* 14/400/20 */
    LOGIT_TS_BODY_PROSE,       /* 14/400/22 */
    LOGIT_TS_CALLOUT,          /* 13/400/18 */
    LOGIT_TS_CALLOUT_BUTTON,   /* 13/500/18 button labels */
    LOGIT_TS_FOOTNOTE,         /* 12/400/16 */
    LOGIT_TS_FOOTNOTE_COLUMN,  /* 12/500/16 column header */
    LOGIT_TS_FOOTNOTE_SECTION, /* 12/600/16 section labels */
    LOGIT_TS_CAPTION,          /* 11/500/14, Latin and digits only */
    LOGIT_TS_WINDOW_TITLE,     /* 13/600 */
    LOGIT_TS_MENUBAR,          /* 14/400 */
    LOGIT_TS_MENUBAR_APP,      /* 14/600 */
    LOGIT_TS_DOCUMENT,         /* 18/400/30 */
    LOGIT_TS_MONO,             /* 13/400/20 Studio */
    LOGIT_TS_MONO_TERM,        /* 14/400/20 Terminal */
    LOGIT_TS_MONO_SMALL,       /* 12/400/16 */
    LOGIT_TS_COUNT
};
#define LOGIT_TS_F_TNUM       1u   /* tabular digits */
#define LOGIT_TS_F_LATIN_ONLY 2u   /* never CJK (caption 11) */
#define LOGIT_TS_F_MONO       4u   /* mono*.ttf face */
#define LOGIT_TS_F_DIGITS     8u   /* digits only (display sizes) */
struct logit_type {
    uint8_t px;
    uint16_t weight;
    uint8_t line;    /* line height px; 0 = single-line, centred by metrics */
    uint8_t flags;   /* LOGIT_TS_F_* */
};
static const struct logit_type LOGIT_TYPE[LOGIT_TS_COUNT] = {
    [LOGIT_TS_DISPLAY]          = { 64, 500, 72, LOGIT_TS_F_TNUM | LOGIT_TS_F_DIGITS },
    [LOGIT_TS_DISPLAY_GREETER]  = { 72, 500, 80, LOGIT_TS_F_TNUM | LOGIT_TS_F_DIGITS },
    [LOGIT_TS_LARGE_TITLE]      = { 26, 600, 34, 0 },
    [LOGIT_TS_TITLE1]           = { 22, 600, 30, 0 },
    [LOGIT_TS_TITLE2]           = { 17, 600, 24, 0 },
    [LOGIT_TS_HEADLINE]         = { 14, 600, 20, 0 },
    [LOGIT_TS_BODY]             = { 14, 400, 20, 0 },
    [LOGIT_TS_BODY_PROSE]       = { 14, 400, 22, 0 },
    [LOGIT_TS_CALLOUT]          = { 13, 400, 18, 0 },
    [LOGIT_TS_CALLOUT_BUTTON]   = { 13, 500, 18, 0 },
    [LOGIT_TS_FOOTNOTE]         = { 12, 400, 16, 0 },
    [LOGIT_TS_FOOTNOTE_COLUMN]  = { 12, 500, 16, 0 },
    [LOGIT_TS_FOOTNOTE_SECTION] = { 12, 600, 16, 0 },
    [LOGIT_TS_CAPTION]          = { 11, 500, 14, LOGIT_TS_F_LATIN_ONLY },
    [LOGIT_TS_WINDOW_TITLE]     = { 13, 600, 0, 0 },
    [LOGIT_TS_MENUBAR]          = { 14, 400, 0, LOGIT_TS_F_TNUM },
    [LOGIT_TS_MENUBAR_APP]      = { 14, 600, 0, 0 },
    [LOGIT_TS_DOCUMENT]         = { 18, 400, 30, 0 },
    [LOGIT_TS_MONO]             = { 13, 400, 20, LOGIT_TS_F_MONO },
    [LOGIT_TS_MONO_TERM]        = { 14, 400, 20, LOGIT_TS_F_MONO },
    [LOGIT_TS_MONO_SMALL]       = { 12, 400, 16, LOGIT_TS_F_MONO },
};
/* The closed size set (glyph-cache warmth): every px above is one of these. */
static const uint8_t LOGIT_TYPE_SIZES[] = { 11, 12, 13, 14, 17, 18, 22, 26, 64, 72 };
#define LOGIT_TYPE_SIZE_COUNT 10
#define LOGIT_CJK_MIN_PX 12

/* Font metrics of the ui*.ttf family for vertical centring (3.6). */
#define LOGIT_FONT_ASCENT   1160
#define LOGIT_FONT_DESCENT  (-288)
#define LOGIT_FONT_UPEM     1000
/* baseline = top + (h - 1.448 px) / 2 + 1.160 px, everything in 24.8 pt
 * except px (an integer pixel size in the same point space). */
static inline int logit_text_baseline24(int top24, int h24, int px)
{
    int span24 = (px * (LOGIT_FONT_ASCENT - LOGIT_FONT_DESCENT) * 256 + LOGIT_FONT_UPEM / 2) / LOGIT_FONT_UPEM;
    int asc24 = (px * LOGIT_FONT_ASCENT * 256 + LOGIT_FONT_UPEM / 2) / LOGIT_FONT_UPEM;
    int d = h24 - span24;
    return top24 + (d >= 0 ? d / 2 : -((-d + 1) / 2)) + asc24;
}

#endif /* LOGIT_UI_TOKENS_H */
