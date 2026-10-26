/* Filtered RGBA blit (stride, source rect, 24.8 destination, opacity,
 * NEAREST / BILINEAR / BOX) and glyph blit with alpha. See
 * openlogit_display_blit.h.
 *
 * One separable engine serves all three filters: each visible destination
 * column (row) gets a short list of source taps with 16.16 weights summing to
 * exactly 65536. NEAREST has 1 tap, BILINEAR 2, BOX the whole footprint of
 * the destination pixel with fractional end weights. Per destination row the
 * source rows are filtered horizontally into premultiplied sums, combined
 * vertically, then composited src-over. No file-scope state: tap tables and
 * accumulators live in display_scratch(OL_DS_BLIT) (per display = per ctx). */
#include "openlogit_display_blit.h"
#include "display_pixels.h"
#include <string.h>

#ifdef OL_DISPLAY_SCALE_NEAREST   /* negative control: BOX degrades to point sampling */
#define BX_BOX_FILTER OL_FILTER_NEAREST
#else
#define BX_BOX_FILTER OL_FILTER_BOX
#endif

struct bx_tap { int start, count; unsigned woff, m; };   /* m: opacity*coverage, 0..255 */

#define BX_D 16581375u    /* 255*255*255: full-scale denominator of the composite */

/* Taps for n destination pixels starting at absolute pixel pfirst. org/len is
 * the destination extent in 24.8; s0/slen the source extent in samples. Edge
 * pixels sample the covered part of the extent; t[i].m gets the coverage
 * (0..256). Returns weights used, or -1 when wcap would overflow. */
static long bx_axis(int filter, long org, long len, int pfirst, int n,
                    int s0, int slen, struct bx_tap *t, uint32_t *w, long wcap)
{
    long used = 0;
    int64_t lo_s = (int64_t)s0 << 16, hi_s = (int64_t)(s0 + slen - 1) << 16;
    int box = filter == OL_FILTER_BOX && (int64_t)slen * 256 > 2 * (int64_t)len;
    for (int i = 0; i < n; i++) {
        int64_t a = (int64_t)(pfirst + i) * 256, b = a + 256;
        if (a < org) a = org;
        if (b > org + len) b = org + len;
        struct bx_tap *k = &t[i];
        k->woff = (unsigned)used;
        if (a >= b) { k->start = s0; k->count = 1; k->m = 0; if (used + 1 > wcap) return -1; w[used++] = 65536; continue; }
        k->m = (unsigned)(b - a);
        int64_t sa = lo_s + ((a - org) * (int64_t)slen * 65536) / len;
        int64_t sb = lo_s + ((b - org) * (int64_t)slen * 65536) / len;
        if (box) {
            int64_t width = sb - sa;
            int k0 = (int)(sa >> 16), k1 = (int)((sb - 1) >> 16);
            if (k0 < s0) k0 = s0;
            if (k1 > s0 + slen - 1) k1 = s0 + slen - 1;
            if (k1 < k0) k1 = k0;
            int cnt = k1 - k0 + 1;
            if (width <= 0 || used + cnt > wcap) {
                if (width > 0) return -1;
                k->start = k0; k->count = 1; w[used++] = 65536; continue;
            }
            k->start = k0; k->count = cnt;
            uint32_t sum = 0, big = 0; int bi = 0;
            for (int q = 0; q < cnt; q++) {
                int64_t l = (int64_t)(k0 + q) << 16, h = l + 65536;
                if (l < sa) l = sa;
                if (h > sb) h = sb;
                uint32_t wq = h > l ? (uint32_t)(((h - l) << 16) / width) : 0;
                w[used + q] = wq; sum += wq;
                if (wq > big) { big = wq; bi = q; }
            }
            w[used + bi] += 65536u - sum;     /* weights sum to exactly 65536 */
            used += cnt;
        } else if (filter == OL_FILTER_NEAREST) {
            int64_t c = (sa + sb) >> 1;
            int idx = (int)(c >> 16);
            if (idx < s0) idx = s0;
            if (idx > s0 + slen - 1) idx = s0 + slen - 1;
            if (used + 1 > wcap) return -1;
            k->start = idx; k->count = 1; w[used++] = 65536;
        } else {                                 /* BILINEAR (also BOX at <= 2x) */
            int64_t c = ((sa + sb) >> 1) - 32768;
            if (c < lo_s) c = lo_s;
            if (c > hi_s) c = hi_s;
            int i0 = (int)(c >> 16);
            uint32_t f = (uint32_t)(c & 0xFFFF);
            if (used + 2 > wcap) return -1;
            k->start = i0;
            if (f == 0 || i0 >= s0 + slen - 1) { k->count = 1; w[used++] = 65536; }
            else { k->count = 2; w[used++] = 65536u - f; w[used++] = f; }
        }
    }
    return used;
}

/* Horizontal pass: one source row -> n*4 premultiplied sums (r*a, g*a, b*a,
 * a*255), each at scale 65536 relative to 0..65025. Weights sum to 65536, so
 * the largest value is 65025*65536 < 2^32. */
static void bx_hrow(const uint8_t *row, const struct bx_tap *xt, const uint32_t *xw,
                    int n, uint32_t *out)
{
    for (int i = 0; i < n; i++, out += 4) {
        const struct bx_tap *k = &xt[i];
        const uint32_t *w = xw + k->woff;
        const uint8_t *p = row + (long)k->start * 4;
        uint32_t r = 0, g = 0, b = 0, aa = 0;
        for (int q = 0; q < k->count; q++, p += 4) {
            uint32_t a = p[3];
            if (!a) continue;
            uint32_t wq = w[q];
            r += wq * (p[0] * a); g += wq * (p[1] * a); b += wq * (p[2] * a);
            aa += wq * (a * 255u);
        }
        out[0] = r; out[1] = g; out[2] = b; out[3] = aa;
    }
}

static inline uint32_t bx_ld(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Premultiplied sums (scale 65025, from bx_hrow's or the 4-tap path) over
 * dst, with m = opacity*coverage (0..255). */
static inline void bx_comp(struct ol_display *ctx, uint32_t *d, unsigned pr, unsigned pg,
                           unsigned pb, unsigned pa, unsigned m)
{
    int dr, dg, db; unpack(ctx, *d, &dr, &dg, &db);
    uint32_t keep = BX_D - pa * m;               /* D - alpha*m */
    uint32_t nr_ = ((uint32_t)dr * keep + pr * m * 255u + BX_D / 2) / BX_D;
    uint32_t ng_ = ((uint32_t)dg * keep + pg * m * 255u + BX_D / 2) / BX_D;
    uint32_t nb_ = ((uint32_t)db * keep + pb * m * 255u + BX_D / 2) / BX_D;
    if (nr_ > 255) nr_ = 255;
    if (ng_ > 255) ng_ = 255;
    if (nb_ > 255) nb_ = 255;
    *d = ol_display_rgb(ctx, (uint8_t)nr_, (uint8_t)ng_, (uint8_t)nb_);
}

/* Both axes have at most two taps per pixel (NEAREST / BILINEAR / BOX at
 * <= 2x): fetch the four source pixels directly, no row buffers. Opaque
 * quads skip premultiplication. */
struct bx_col { uint32_t o0, d, fx, m; };   /* byte offsets of tap 0 and the tap-1 delta (0 or 4) */

static void bx_direct(struct ol_display *ctx, const uint8_t *src, int sstride,
                      const struct bx_tap *xt, const uint32_t *xw, int nc,
                      const struct bx_tap *yt, const uint32_t *yw, int nr,
                      uint32_t *dst0, int dstride, struct bx_col *col)
{
    for (int i = 0; i < nc; i++) {
        col[i].o0 = (uint32_t)xt[i].start * 4;
        col[i].d = xt[i].count > 1 ? 4u : 0u;
        col[i].fx = xt[i].count > 1 ? xw[xt[i].woff + 1] : 0;
        col[i].m = xt[i].m;
    }
    const unsigned rp = ctx->rpos, gp = ctx->gpos, bp = ctx->bpos;
    for (int j = 0; j < nr; j++, dst0 += dstride) {
        const struct bx_tap *ky = &yt[j];
        unsigned my = ky->m;
        if (!my) continue;
        int two = ky->count > 1;
        const uint8_t *r0 = src + (long)ky->start * sstride, *r1 = two ? r0 + sstride : r0;
        int64_t fy = two ? yw[ky->woff + 1] : 0;
        int32_t fy12 = (int32_t)(fy >> 4);       /* vertical weight, 12 fractional bits */
        for (int i = 0; i < nc; i++) {
            const struct bx_col *kx = &col[i];
            unsigned m = my == 256 ? kx->m : (kx->m * my + 128) >> 8;
            if (!m) continue;
            uint32_t o0 = kx->o0, o1 = o0 + kx->d;
            int32_t fx = (int32_t)kx->fx;
            uint32_t p00 = bx_ld(r0 + o0), p10 = bx_ld(r0 + o1), p01 = bx_ld(r1 + o0), p11 = bx_ld(r1 + o1);
            if ((p00 & p10 & p01 & p11) >> 24 == 255u) {
                uint32_t c[3];
                for (int q = 0; q < 3; q++) {
                    unsigned sh = (unsigned)q * 8;
                    int32_t a0 = (int32_t)((p00 >> sh) & 255), a1 = (int32_t)((p10 >> sh) & 255);
                    int32_t b0 = (int32_t)((p01 >> sh) & 255), b1 = (int32_t)((p11 >> sh) & 255);
                    int32_t t0 = ((a0 << 16) + (a1 - a0) * fx) >> 8;
                    int32_t t1 = ((b0 << 16) + (b1 - b0) * fx) >> 8;
                    c[q] = (uint32_t)(((t0 << 12) + (t1 - t0) * fy12 + (1 << 19)) >> 20);
                }
                if (m >= 255) { dst0[i] = c[0] << rp | c[1] << gp | c[2] << bp; continue; }
                int dr, dg, db; unpack(ctx, dst0[i], &dr, &dg, &db);
                dst0[i] = ol_display_rgb(ctx, (uint8_t)((dr * (255 - m) + c[0] * m + 127) / 255),
                                         (uint8_t)((dg * (255 - m) + c[1] * m + 127) / 255),
                                         (uint8_t)((db * (255 - m) + c[2] * m + 127) / 255));
                continue;
            }
            uint32_t gx = 65536u - (uint32_t)fx, gy = 65536u - (uint32_t)fy;
            uint32_t w00 = (uint32_t)(((uint64_t)gx * gy) >> 16), w10 = (uint32_t)(((uint64_t)(uint32_t)fx * gy) >> 16);
            uint32_t w01 = (uint32_t)(((uint64_t)gx * (uint32_t)fy) >> 16), w11 = 65536u - w00 - w10 - w01;
            uint32_t pr = 0, pg = 0, pb = 0, pa = 0;
            const uint32_t pp[4] = { p00, p10, p01, p11 }, ww[4] = { w00, w10, w01, w11 };
            for (int q = 0; q < 4; q++) {
                uint32_t a = pp[q] >> 24;
                if (!a) continue;
                pr += ww[q] * ((pp[q] & 255) * a); pg += ww[q] * (((pp[q] >> 8) & 255) * a);
                pb += ww[q] * (((pp[q] >> 16) & 255) * a); pa += ww[q] * (a * 255u);
            }
            pr = (pr + 32768) >> 16; pg = (pg + 32768) >> 16; pb = (pb + 32768) >> 16; pa = (pa + 32768) >> 16;
            if (!pa) continue;
            bx_comp(ctx, &dst0[i], pr, pg, pb, pa, m);
        }
    }
}

int ol_display_blit_rgba_ex(struct ol_display *ctx, const uint8_t *src,
                            int sw, int sh, int sstride,
                            int src_x, int src_y, int src_w, int src_h,
                            int dx24, int dy24, int dw24, int dh24,
                            int opacity, enum ol_filter filter)
{
    if (!ctx || !src || !ctx->target.px) return OL_ARGUMENT;
    if (sw <= 0 || sh <= 0 || sw > 32768 || sh > 32768 || sstride < sw * 4 ||
        src_x < 0 || src_y < 0 || src_w <= 0 || src_h <= 0 ||
        src_x > sw - src_w || src_y > sh - src_h || dw24 <= 0 || dh24 <= 0)
        return OL_ARGUMENT;
    if (opacity <= 0) return OL_OK;
    if (opacity > 255) opacity = 255;
    if (filter == OL_FILTER_BOX) filter = BX_BOX_FILTER;

    long X0 = (long)(((int64_t)dx24) >> 8), X1 = (long)(((int64_t)dx24 + dw24 + 255) >> 8);
    long Y0 = (long)(((int64_t)dy24) >> 8), Y1 = (long)(((int64_t)dy24 + dh24 + 255) >> 8);
    if (X1 - X0 > 0x3fffffff || Y1 - Y0 > 0x3fffffff) return OL_ARGUMENT;
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, (int)X0, (int)Y0, (int)(X1 - X0), (int)(Y1 - Y0), &i0, &j0, &i1, &j1)) return OL_OK;
    int nc = i1 - i0, nr = j1 - j0;
    long xcap = src_w + 2L * nc + 4, ycap = src_h + 2L * nr + 4;

    /* [xt][yt][xw][yw][hbuf nc*4 u32][acc nc*4 u64], 8-byte aligned. */
    unsigned long bytes = (unsigned long)nc * sizeof(struct bx_tap) + (unsigned long)nr * sizeof(struct bx_tap) +
                          (unsigned long)(xcap + ycap) * 4 + (unsigned long)nc * 16 + (unsigned long)nc * 32 + 64;
    uint8_t *mem = display_scratch(ctx, OL_DS_BLIT, bytes);
    if (!mem) return OL_LIMIT;
    uintptr_t al = ((uintptr_t)mem + 7) & ~(uintptr_t)7;
    uint8_t *q = (uint8_t *)al;
    struct bx_tap *xt = (struct bx_tap *)q;      q += (unsigned long)nc * sizeof *xt;
    struct bx_tap *yt = (struct bx_tap *)q;      q += (unsigned long)nr * sizeof *yt;
    uint32_t *xw = (uint32_t *)q;                q += (unsigned long)xcap * 4;
    uint32_t *yw = (uint32_t *)q;                q += (unsigned long)ycap * 4;
    q = (uint8_t *)(((uintptr_t)q + 7) & ~(uintptr_t)7);
    uint32_t *hb = (uint32_t *)q;                q += (unsigned long)nc * 16;
    uint64_t *acc = (uint64_t *)q;

    if (bx_axis(filter, dx24, dw24, (int)X0 + i0, nc, src_x, src_w, xt, xw, xcap) < 0 ||
        bx_axis(filter, dy24, dh24, (int)Y0 + j0, nr, src_y, src_h, yt, yw, ycap) < 0)
        return OL_ARGUMENT;
    unsigned op = (unsigned)opacity;
    for (int i = 0; i < nc; i++) xt[i].m = (op * xt[i].m + 128) >> 8;   /* 0..255 */

    struct ol_pixel_target *t = &ctx->target;
    int two_tap = 1;
    for (int i = 0; i < nc && two_tap; i++) if (xt[i].count > 2) two_tap = 0;
    for (int j = 0; j < nr && two_tap; j++) if (yt[j].count > 2) two_tap = 0;
    if (two_tap) {
        bx_direct(ctx, src, sstride, xt, xw, nc, yt, yw, nr,
                  t->px + (long)(Y0 + j0) * t->w + (X0 + i0), t->w, (struct bx_col *)hb);
        return OL_OK;
    }
    for (int j = 0; j < nr; j++) {
        const struct bx_tap *ky = &yt[j];
        unsigned my = ky->m;
        if (!my) continue;
        const uint32_t *wy = yw + ky->woff;
        const uint32_t *P;                      /* nc*4 sums at scale 65536 */
        int single = ky->count == 1;
        if (single) {
            bx_hrow(src + (long)ky->start * sstride, xt, xw, nc, hb);
            P = hb;
        } else {
            for (int z = 0; z < nc * 4; z++) acc[z] = 0;
            for (int r = 0; r < ky->count; r++) {
                bx_hrow(src + (long)(ky->start + r) * sstride, xt, xw, nc, hb);
                uint64_t wr = wy[r];
                for (int z = 0; z < nc * 4; z++) acc[z] += wr * hb[z];
            }
            P = 0;
        }
        uint32_t *drow = t->px + (long)(Y0 + j0 + j) * t->w + (X0 + i0);
        for (int i = 0; i < nc; i++) {
            unsigned m = (xt[i].m * my + 128) >> 8;      /* opacity*coverage, 0..255 */
            if (!m) continue;
            unsigned pr, pg, pb, pa;
            if (single) {
                const uint32_t *s = P + i * 4;
                pr = (s[0] + 32768) >> 16; pg = (s[1] + 32768) >> 16;
                pb = (s[2] + 32768) >> 16; pa = (s[3] + 32768) >> 16;
            } else {
                const uint64_t *s = acc + i * 4;
                pr = (unsigned)((s[0] + (1u << 31)) >> 32); pg = (unsigned)((s[1] + (1u << 31)) >> 32);
                pb = (unsigned)((s[2] + (1u << 31)) >> 32); pa = (unsigned)((s[3] + (1u << 31)) >> 32);
            }
            if (!pa) continue;
            if (pa >= 65025u && m >= 255) {              /* opaque and fully covered */
                drow[i] = ol_display_rgb(ctx, (uint8_t)((pr + 127) / 255), (uint8_t)((pg + 127) / 255),
                                         (uint8_t)((pb + 127) / 255));
                continue;
            }
            bx_comp(ctx, &drow[i], pr, pg, pb, pa, m);
        }
    }
    return OL_OK;
}

void ol_display_blit_glyph_a(struct ol_display *ctx, int x, int y,
                             const uint8_t *cov, int w, int h, int stride,
                             uint32_t color, int alpha)
{
    if (!ctx || !cov || alpha <= 0 || stride < w) return;
    if (alpha > 255) alpha = 255;
    int cr, cg, cb; unpack(ctx, color, &cr, &cg, &cb);
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
    struct ol_pixel_target *s = &ctx->target;
    for (int j = j0; j < j1; j++) {
        const uint8_t *crow = cov + (long)j * stride;
        uint32_t *drow = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) {
            int a = crow[i];
            if (alpha < 255) a = (a * alpha + 127) / 255;
            if (!a) continue;
            if (a >= 255) { drow[i] = color; continue; }
            int br, bg, bb; unpack(ctx, drow[i], &br, &bg, &bb);
            int nr = (cr * a + br * (255 - a)) / 255;
            int ng = (cg * a + bg * (255 - a)) / 255;
            int nb = (cb * a + bb * (255 - a)) / 255;
            drow[i] = ol_display_rgb(ctx, (uint8_t)nr, (uint8_t)ng, (uint8_t)nb);
        }
    }
}
