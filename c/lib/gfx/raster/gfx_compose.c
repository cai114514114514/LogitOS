/* Open Logit -- blend modes and Porter-Duff operators on straight RGBA.
 * See gfx_blend.h. Integer only, no libc: kernel-safe, pure per-pixel, no state.
 *
 * Colours are 0..255. A result is `num / den`: num is premultiplied colour and den
 * the result alpha in a common fixed unit, so the straight colour costs ONE rounded
 * division and no intermediate rounding. Source alpha*coverage is kept unrounded
 * (1/65025 units) for the blend modes and PLUS_LIGHTER: rounding it to 8 bits first
 * costs up to 2/255 of colour when the destination is faint. */
#include "gfx.h"

#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))

/* W3C soft-light D(Cb) = ((16Cb-12)Cb+4)Cb for Cb <= 1/4, else sqrt(Cb), scaled to 0..255.
 * A table: it replaces a per-pixel integer square root. */
static const uint8_t soft_d[256] = {
    0, 4, 8, 12, 15, 19, 22, 26, 29, 32, 36, 39, 42, 45, 47, 50,
    53, 56, 58, 61, 63, 66, 68, 70, 72, 74, 77, 79, 81, 82, 84, 86,
    88, 90, 91, 93, 94, 96, 98, 99, 100, 102, 103, 105, 106, 107, 108, 110,
    111, 112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 135, 136, 137, 138, 139, 140, 141, 142,
    143, 144, 145, 145, 146, 147, 148, 149, 150, 151, 151, 152, 153, 154, 155, 156,
    156, 157, 158, 159, 160, 160, 161, 162, 163, 164, 164, 165, 166, 167, 167, 168,
    169, 170, 170, 171, 172, 173, 173, 174, 175, 176, 176, 177, 178, 179, 179, 180,
    181, 181, 182, 183, 183, 184, 185, 186, 186, 187, 188, 188, 189, 190, 190, 191,
    192, 192, 193, 194, 194, 195, 196, 196, 197, 198, 198, 199, 199, 200, 201, 201,
    202, 203, 203, 204, 204, 205, 206, 206, 207, 208, 208, 209, 209, 210, 211, 211,
    212, 212, 213, 214, 214, 215, 215, 216, 217, 217, 218, 218, 219, 220, 220, 221,
    221, 222, 222, 223, 224, 224, 225, 225, 226, 226, 227, 228, 228, 229, 229, 230,
    230, 231, 231, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238,
    239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247,
    247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255,
};

static inline __attribute__((always_inline)) int mul255(int a, int b) { return (a * b + 127) / 255; }

/* The separable blend function B(Cb, Cs), all in 0..255. */
static inline __attribute__((always_inline)) int blend_fn(int op, int cb, int cs)
{
    switch (op) {
    case GFX_OP_MULTIPLY: return mul255(cb, cs);
    case GFX_OP_SCREEN:   return cb + cs - mul255(cb, cs);
    case GFX_OP_OVERLAY:  /* hard-light with the roles swapped */
        if (2 * cb <= 255) return mul255(cs, 2 * cb);
        { int b2 = 2 * cb - 255; return cs + b2 - mul255(cs, b2); }
    case GFX_OP_DARKEN:   return cb < cs ? cb : cs;
    case GFX_OP_LIGHTEN:  return cb > cs ? cb : cs;
    case GFX_OP_COLOR_DODGE:
        if (cb == 0) return 0;
        if (cs == 255) return 255;
        { int v = (cb * 255 + (255 - cs) / 2) / (255 - cs); return v > 255 ? 255 : v; }
    case GFX_OP_SOFT_LIGHT: {
        if (2 * cs <= 255)
            return cb - ((255 - 2 * cs) * cb * (255 - cb) + 32512) / 65025;
        int d = soft_d[cb];
        return cb + ((2 * cs - 255) * (d - cb) + 127) / 255;
    }
    }
    return cs;
}

/* Opaque-destination blend with `op` a compile-time constant at every call site, so
 * the blend function is specialised and the channel loop carries no op dispatch. */
static inline __attribute__((always_inline))
void opaque_blend(int op, uint8_t *dst, const int *sc, uint32_t s16)
{
    for (int i = 0; i < 3; i++) {
        int cb = dst[i];
        int b = CL(blend_fn(op, cb, sc[i]));
        dst[i] = (unsigned char)((s16 * (uint32_t)b + (65025u - s16) * (uint32_t)cb + 32512u) / 65025u);
    }
}

void gfx_compose(enum gfx_op op, uint8_t *dst, uint32_t src_rgb, int src_a, int cov)
{
    if ((unsigned)op >= GFX_OP_COUNT) op = GFX_OP_SRC_OVER;
#ifdef GFX_BLEND_NEGCTL
    op = GFX_OP_SRC_OVER;       /* control only: every operator degrades to src-over */
#endif
    if (op == GFX_OP_SRC_OVER) {
        gfx_over(dst, GFX_R(src_rgb), GFX_G(src_rgb), GFX_B(src_rgb), src_a, cov);
        return;
    }
    if (cov <= 0) return;
    if (cov > 255) cov = 255;
    if (src_a < 0) src_a = 0;
    if (src_a > 255) src_a = 255;
    int sc[3] = { GFX_R(src_rgb), GFX_G(src_rgb), GFX_B(src_rgb) };
    int ba = dst[3];

    if (op >= GFX_OP_MULTIPLY) {
        uint32_t s16 = (uint32_t)(src_a * cov);                       /* 1/65025 */
        if (!s16) return;
        if (ba == 255) {        /* opaque destination: the common case, one constant divide */
            switch (op) {
#define OPAQUE_CASE(O) case O: opaque_blend(O, dst, sc, s16); return;
            OPAQUE_CASE(GFX_OP_MULTIPLY) OPAQUE_CASE(GFX_OP_SCREEN)
            OPAQUE_CASE(GFX_OP_OVERLAY) OPAQUE_CASE(GFX_OP_DARKEN)
            OPAQUE_CASE(GFX_OP_LIGHTEN) OPAQUE_CASE(GFX_OP_SOFT_LIGHT)
            default: opaque_blend(GFX_OP_COLOR_DODGE, dst, sc, s16); return;
#undef OPAQUE_CASE
            }
        }
        uint32_t den = s16 * 255u + (uint32_t)ba * (65025u - s16);    /* 1/(255*65025) */
        for (int i = 0; i < 3; i++) {
            int cb = dst[i];
            int b = CL(blend_fn(op, cb, sc[i]));
            uint32_t num = s16 * (uint32_t)(255 - ba) * (uint32_t)sc[i]
                         + s16 * (uint32_t)ba * (uint32_t)b
                         + (65025u - s16) * (uint32_t)ba * (uint32_t)cb;
            dst[i] = (unsigned char)((num + den / 2) / den);
        }
        uint32_t ar = (den + 32512u) / 65025u;
        dst[3] = (unsigned char)(ar > 255 ? 255 : ar);
        return;
    }
    if (op == GFX_OP_PLUS_LIGHTER) {
        uint32_t s16 = (uint32_t)(src_a * cov);
        if (!s16) return;
        uint32_t den = s16 * 255u + (uint32_t)ba * 65025u;
        uint32_t dc = den > 16581375u ? 16581375u : den;              /* alpha clamps at 1 */
        for (int i = 0; i < 3; i++) {
            /* the two terms can sum past 32 bits before the clamp */
            unsigned long long num = (unsigned long long)s16 * 255u * (uint32_t)sc[i]
                                   + (unsigned long long)ba * 65025u * (uint32_t)dst[i];
            if (num > 255ull * 16581375u) num = 255ull * 16581375u;
            uint32_t v = (uint32_t)(num + dc / 2) / dc;
            dst[i] = (unsigned char)(v > 255 ? 255 : v);
        }
        uint32_t ar = (dc + 32512u) / 65025u;
        dst[3] = (unsigned char)(ar > 255 ? 255 : ar);
        return;
    }

    /* Porter-Duff: aR = aS Fa + aB Fb in 1/65025 units. Coverage then blends the
     * full-coverage result with the untouched destination (premultiplied lerp). */
    int sa = src_a, fa, fb;
    switch (op) {
    case GFX_OP_SRC:      fa = 255;      fb = 0;        break;
    case GFX_OP_CLEAR:    fa = 0;        fb = 0;        break;
    case GFX_OP_DST_IN:   fa = 0;        fb = sa;       break;
    case GFX_OP_DST_OUT:  fa = 0;        fb = 255 - sa; break;
    case GFX_OP_SRC_IN:   fa = ba;       fb = 0;        break;
    case GFX_OP_SRC_ATOP: fa = ba;       fb = 255 - sa; break;
    default: /* XOR */    fa = 255 - ba; fb = 255 - sa; break;
    }
    int den = sa * fa + ba * fb;
    int ar = 0, col[3] = { 0, 0, 0 };
    if (den > 0) {
        for (int i = 0; i < 3; i++)
            col[i] = CL((sa * fa * sc[i] + ba * fb * dst[i] + den / 2) / den);
        ar = (den + 127) / 255;
    }
    if (cov < 255) {
        int w0 = ba * (255 - cov), w1 = ar * cov, wt = w0 + w1;
        if (wt <= 0) { dst[0] = dst[1] = dst[2] = dst[3] = 0; return; }
        for (int i = 0; i < 3; i++)
            dst[i] = (unsigned char)((w0 * dst[i] + w1 * col[i] + wt / 2) / wt);
        ar = (wt + 127) / 255;
    } else {
        for (int i = 0; i < 3; i++) dst[i] = (unsigned char)col[i];
    }
    dst[3] = (unsigned char)(ar > 255 ? 255 : ar);
}
