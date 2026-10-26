/* ol_display_shadow_ex: analytic gaussian shadow of a rounded rectangle.
 *
 * coverage(p) = RectCover(p) - sum over corners of CutCover_c(p)
 *
 * RectCover is the separable erf product Cx(px) * Cy(py) of the plain
 * (square-cornered) shape. Each rounded corner removes a small cut-out region
 * (the square minus the quarter disc), whose blurred coverage is the per-row
 * integral  sum_k wy_k(py) * H_k(px)  over the cut-out rows: H_k is the
 * horizontal erf difference of row k's cut-out extent (Evan Wallace's method),
 * wy_k the exact gaussian weight of that row cell. The H tables are built once
 * per corner and reused by every output row, so a pixel in a corner zone costs
 * one multiply-accumulate per sampled row.
 *
 * Freestanding: no libc, no float, no static mutable state. Temporary buffers
 * live in the per-display OL_DS_SHADOW scratch slot (per drawing context).
 * Fixed point: coordinates 24.8, coverage 0..65535 (16.16 fraction of 1.0).
 */
#include "openlogit_display_shadow.h"
#include "display_pixels.h"

typedef long long i64;

/* erf(i/256) * 65535 for i in [0,1024): u in [0,4). Beyond that erf is 1. */
static const uint16_t erf_lut[1024] = {
    0, 289, 578, 867, 1155, 1444, 1733, 2022, 2310, 2599, 2887, 3176,
    3464, 3752, 4040, 4328, 4616, 4903, 5191, 5478, 5765, 6052, 6339, 6626,
    6912, 7199, 7485, 7770, 8056, 8341, 8626, 8911, 9196, 9480, 9764, 10047,
    10331, 10614, 10897, 11179, 11461, 11743, 12024, 12305, 12586, 12866, 13146, 13425,
    13705, 13983, 14261, 14539, 14817, 15094, 15370, 15646, 15922, 16197, 16472, 16746,
    17019, 17293, 17565, 17837, 18109, 18380, 18651, 18921, 19190, 19459, 19727, 19995,
    20262, 20529, 20795, 21060, 21325, 21589, 21853, 22116, 22378, 22640, 22901, 23161,
    23421, 23680, 23938, 24196, 24453, 24709, 24965, 25220, 25474, 25728, 25980, 26232,
    26484, 26734, 26984, 27233, 27482, 27729, 27976, 28222, 28468, 28712, 28956, 29199,
    29441, 29682, 29923, 30163, 30402, 30640, 30877, 31113, 31349, 31584, 31818, 32051,
    32283, 32515, 32745, 32975, 33204, 33432, 33659, 33886, 34111, 34335, 34559, 34782,
    35004, 35225, 35445, 35664, 35882, 36100, 36316, 36532, 36746, 36960, 37173, 37385,
    37596, 37806, 38015, 38223, 38430, 38637, 38842, 39047, 39250, 39453, 39654, 39855,
    40055, 40253, 40451, 40648, 40844, 41039, 41233, 41426, 41618, 41809, 42000, 42189,
    42377, 42564, 42751, 42936, 43120, 43304, 43486, 43668, 43848, 44028, 44207, 44384,
    44561, 44737, 44911, 45085, 45258, 45430, 45601, 45771, 45940, 46107, 46274, 46441,
    46606, 46770, 46933, 47095, 47256, 47416, 47576, 47734, 47891, 48048, 48203, 48358,
    48511, 48664, 48816, 48966, 49116, 49265, 49413, 49560, 49706, 49851, 49995, 50138,
    50280, 50421, 50562, 50701, 50840, 50977, 51114, 51250, 51384, 51518, 51651, 51783,
    51914, 52045, 52174, 52302, 52430, 52556, 52682, 52807, 52931, 53054, 53176, 53297,
    53418, 53537, 53656, 53774, 53891, 54007, 54122, 54236, 54349, 54462, 54574, 54685,
    54795, 54904, 55012, 55120, 55226, 55332, 55437, 55541, 55645, 55747, 55849, 55950,
    56050, 56150, 56248, 56346, 56443, 56539, 56634, 56729, 56823, 56916, 57008, 57099,
    57190, 57280, 57369, 57458, 57545, 57632, 57719, 57804, 57889, 57973, 58056, 58139,
    58221, 58302, 58382, 58462, 58541, 58619, 58697, 58774, 58850, 58925, 59000, 59074,
    59148, 59221, 59293, 59364, 59435, 59505, 59575, 59644, 59712, 59780, 59847, 59913,
    59979, 60044, 60108, 60172, 60235, 60298, 60360, 60421, 60482, 60543, 60602, 60661,
    60720, 60778, 60835, 60892, 60948, 61004, 61059, 61113, 61167, 61221, 61274, 61326,
    61378, 61429, 61480, 61530, 61580, 61629, 61678, 61726, 61774, 61821, 61868, 61914,
    61960, 62005, 62050, 62094, 62138, 62182, 62225, 62267, 62309, 62351, 62392, 62432,
    62473, 62512, 62552, 62591, 62629, 62667, 62705, 62742, 62779, 62815, 62851, 62887,
    62922, 62956, 62991, 63025, 63058, 63092, 63124, 63157, 63189, 63221, 63252, 63283,
    63314, 63344, 63374, 63403, 63433, 63462, 63490, 63518, 63546, 63574, 63601, 63628,
    63654, 63681, 63706, 63732, 63757, 63782, 63807, 63831, 63855, 63879, 63903, 63926,
    63949, 63972, 63994, 64016, 64038, 64059, 64081, 64102, 64122, 64143, 64163, 64183,
    64203, 64222, 64241, 64260, 64279, 64297, 64316, 64334, 64351, 64369, 64386, 64403,
    64420, 64437, 64453, 64469, 64485, 64501, 64517, 64532, 64547, 64562, 64577, 64592,
    64606, 64620, 64634, 64648, 64662, 64675, 64688, 64701, 64714, 64727, 64739, 64752,
    64764, 64776, 64788, 64799, 64811, 64822, 64834, 64845, 64855, 64866, 64877, 64887,
    64897, 64908, 64918, 64927, 64937, 64947, 64956, 64966, 64975, 64984, 64993, 65001,
    65010, 65019, 65027, 65035, 65043, 65051, 65059, 65067, 65075, 65082, 65090, 65097,
    65105, 65112, 65119, 65126, 65132, 65139, 65146, 65152, 65159, 65165, 65171, 65177,
    65183, 65189, 65195, 65201, 65207, 65212, 65218, 65223, 65228, 65234, 65239, 65244,
    65249, 65254, 65259, 65264, 65268, 65273, 65277, 65282, 65286, 65291, 65295, 65299,
    65303, 65307, 65311, 65315, 65319, 65323, 65327, 65330, 65334, 65338, 65341, 65345,
    65348, 65351, 65355, 65358, 65361, 65364, 65367, 65370, 65373, 65376, 65379, 65382,
    65385, 65387, 65390, 65393, 65395, 65398, 65400, 65403, 65405, 65408, 65410, 65412,
    65415, 65417, 65419, 65421, 65423, 65426, 65428, 65430, 65432, 65434, 65435, 65437,
    65439, 65441, 65443, 65444, 65446, 65448, 65450, 65451, 65453, 65454, 65456, 65457,
    65459, 65460, 65462, 65463, 65465, 65466, 65467, 65469, 65470, 65471, 65472, 65474,
    65475, 65476, 65477, 65478, 65479, 65481, 65482, 65483, 65484, 65485, 65486, 65487,
    65488, 65489, 65490, 65490, 65491, 65492, 65493, 65494, 65495, 65496, 65496, 65497,
    65498, 65499, 65499, 65500, 65501, 65501, 65502, 65503, 65504, 65504, 65505, 65505,
    65506, 65507, 65507, 65508, 65508, 65509, 65509, 65510, 65510, 65511, 65511, 65512,
    65512, 65513, 65513, 65514, 65514, 65515, 65515, 65516, 65516, 65516, 65517, 65517,
    65518, 65518, 65518, 65519, 65519, 65519, 65520, 65520, 65520, 65521, 65521, 65521,
    65522, 65522, 65522, 65522, 65523, 65523, 65523, 65523, 65524, 65524, 65524, 65524,
    65525, 65525, 65525, 65525, 65526, 65526, 65526, 65526, 65526, 65527, 65527, 65527,
    65527, 65527, 65527, 65528, 65528, 65528, 65528, 65528, 65528, 65529, 65529, 65529,
    65529, 65529, 65529, 65529, 65530, 65530, 65530, 65530, 65530, 65530, 65530, 65530,
    65530, 65531, 65531, 65531, 65531, 65531, 65531, 65531, 65531, 65531, 65531, 65531,
    65532, 65532, 65532, 65532, 65532, 65532, 65532, 65532, 65532, 65532, 65532, 65532,
    65532, 65532, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533,
    65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65533, 65534,
    65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534,
    65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534,
    65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534, 65534,
    65534, 65534, 65534, 65534, 65534, 65534, 65534, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535,
};

/* u is erf's argument in 16.16, signed. Result in -65535..65535. */
static int erf16(i64 u)
{
    int neg = u < 0, v;
    if (neg) u = -u;
    if (u >= 4 * 65536) v = 65535;
    else {
        int i = (int)(u >> 8), f = (int)(u & 255);
        int a = erf_lut[i], b = i < 1023 ? erf_lut[i + 1] : 65535;
        v = a + (((b - a) * f) >> 8);
    }
    return neg ? -v : v;
}

/* Gaussian CDF of a 24.8 distance d, K = 2^24*256/sqrt2 / sigma24. 0..65535. */
static inline int phi(i64 d, i64 K) { return (65536 + erf16((d * K) >> 16)) >> 1; }

static i64 isqrt64(i64 v)
{
    if (v <= 0) return 0;
    unsigned long long x = (unsigned long long)v, r = 0, bit = 1ull << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (i64)r;
}

static inline int div255(int v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

/* Rounded rectangle in 24.8; radii order TL, TR, BR, BL. */
struct shp { int empty; i64 x0, y0, x1, y1, r[4]; };

static void shp_make(struct shp *s, int x, int y, int w, int h, const int *rad,
                     i64 ox, i64 oy, i64 grow)
{
    s->x0 = ((i64)x << 8) + ox - grow;
    s->y0 = ((i64)y << 8) + oy - grow;
    s->x1 = ((i64)(x + w) << 8) + ox + grow;
    s->y1 = ((i64)(y + h) << 8) + oy + grow;
    s->empty = s->x1 <= s->x0 || s->y1 <= s->y0;
    for (int i = 0; i < 4; i++) {
        i64 r = rad && rad[i] > 0 ? (i64)rad[i] << 8 : 0;
        if (r > 0) { r += grow; if (r < 0) r = 0; }
        s->r[i] = r;
    }
    if (s->empty) return;
    /* CSS-style fit: scale every radius by the tightest side. */
    static const unsigned char pr[4][2] = { {0, 1}, {3, 2}, {0, 3}, {1, 2} };
    i64 n = 1, d = 1, W = s->x1 - s->x0, H = s->y1 - s->y0;
    for (int k = 0; k < 4; k++) {
        i64 side = k < 2 ? W : H, sum = s->r[pr[k][0]] + s->r[pr[k][1]];
        if (sum * d > side * n) { n = side; d = sum; }
    }
    if (n != d)
        for (int i = 0; i < 4; i++) s->r[i] = s->r[i] * n / d;
}

static inline int ceil8(i64 v) { return (int)((v + 255) >> 8); }

/* Per-corner cut-out table. */
#define CN_KMAX 128
struct cn {
    int act, left, top, xa, xb, ya, yb, K, Wc;
    i64 s24, rc, xe, ye, b[CN_KMAX + 1];
    uint32_t *tab;
};

static void cn_prep(struct cn *c, const struct shp *S, int idx, i64 sig24, int M,
                    int px0, int px1)
{
    i64 rc = S->r[idx];
    c->act = 0;
    if (S->empty || rc <= 0) return;
    c->left = idx == 0 || idx == 3;
    c->top = idx == 0 || idx == 1;
    c->rc = rc;
    c->xe = c->left ? S->x0 : S->x1;
    c->ye = c->top ? S->y0 : S->y1;
    c->xa = c->left ? (int)(c->xe >> 8) - M : (int)((c->xe - rc) >> 8) - M;
    c->xb = c->left ? ceil8(c->xe + rc) + M : ceil8(c->xe) + M;
    c->ya = c->top ? (int)(c->ye >> 8) - M : (int)((c->ye - rc) >> 8) - M;
    c->yb = c->top ? ceil8(c->ye + rc) + M : ceil8(c->ye) + M;
    if (c->xa < px0) c->xa = px0;
    if (c->xb > px1) c->xb = px1;
    if (c->xa >= c->xb) return;
    /* Row cell height: half a sigma, at most CN_KMAX cells per corner. */
    i64 s = sig24 / 2;
    if (s < 256) s = 256;
    if ((rc + s - 1) / s > CN_KMAX) s = (rc + CN_KMAX - 1) / CN_KMAX;
    c->s24 = s;
    c->K = (int)((rc + s - 1) / s);
    for (int j = 0; j <= c->K; j++) { i64 v = j * s; c->b[j] = v > rc ? rc : v; }
    c->Wc = c->xb - c->xa;
    c->act = 1;
}

static i64 cn_edge(const struct cn *c, i64 a)
{
    i64 t = c->rc - a;
    return c->rc - isqrt64(c->rc * c->rc - t * t);
}

static void cn_fill(struct cn *c, i64 K, i64 sig)
{
    /* Each cell's H is the mean over n sub-rows. The first rows of a big
     * radius move the edge by many pixels per row, so n grows with how far the
     * edge travels across the cell relative to sigma (1..16). */
    for (int k = 0; k < c->K; k++) {
        i64 lo = c->b[k], span = c->b[k + 1] - lo, e[16];
        i64 de = cn_edge(c, lo) - cn_edge(c, lo + span);
        int n = (int)(1 + 4 * (de < 0 ? -de : de) / sig);
        if (n > 16) n = 16;
        for (int j = 0; j < n; j++) e[j] = cn_edge(c, lo + span * (2 * j + 1) / (2 * n));
        uint32_t *row = c->tab + (long)k * c->Wc;
        for (int i = 0; i < c->Wc; i++) {
            i64 pc = ((i64)(c->xa + i) << 8) + 128, acc = 0;
            for (int j = 0; j < n; j++)
                acc += c->left ? phi(c->xe + e[j] - pc, K) - phi(c->xe - pc, K)
                               : phi(c->xe - pc, K) - phi(c->xe - e[j] - pc, K);
            row[i] = (uint32_t)(acc / n);
        }
    }
}

/* Antialiased coverage 0..255 of the caster rrect at pixel (px,py). */
static int box_cov(const struct shp *b, int px, int py)
{
    i64 pxc = ((i64)px << 8) + 128, pyc = ((i64)py << 8) + 128;
    for (int i = 0; i < 4; i++) {
        i64 r = b->r[i], cx, cy;
        if (r <= 0) continue;
        cx = (i == 0 || i == 3) ? b->x0 + r : b->x1 - r;
        cy = (i < 2) ? b->y0 + r : b->y1 - r;
        if ((i == 0 || i == 3) ? pxc >= cx : pxc <= cx) continue;
        if ((i < 2) ? pyc >= cy : pyc <= cy) continue;
        i64 dx = pxc - cx, dy = pyc - cy;
        i64 v = r - isqrt64(dx * dx + dy * dy) + 128;
        return v <= 0 ? 0 : v >= 256 ? 255 : (int)v;
    }
    return 255;
}

/* Blend colour c over d with 0..255 alpha, two channels per multiply (the red
 * and blue bytes sit 16 bits apart in every packed layout we bind). */
static inline uint32_t blend_sw(uint32_t d, uint32_t crb, uint32_t cg, unsigned a)
{
    unsigned ap = a + (a >> 7), ia = 256 - ap;
    uint32_t rb = (((d & 0xFF00FFu) * ia + crb * ap) >> 8) & 0xFF00FFu;
    uint32_t g = ((((d >> 8) & 255u) * ia + cg * ap) >> 8) & 255u;
    return rb | (g << 8);
}

static inline uint32_t blend_gen(struct ol_display *ctx, uint32_t d, int cr, int cg,
                                 int cb, int a)
{
    int dr, dg, db;
    unpack(ctx, d, &dr, &dg, &db);
    return display_rgb(ctx, (uint8_t)div255(cr * a + dr * (255 - a)),
                       (uint8_t)div255(cg * a + dg * (255 - a)),
                       (uint8_t)div255(cb * a + db * (255 - a)));
}

/* Smallest reach in px past which alpha * (1 - cdf) < 0.5 level, plus one. */
static int reach_px(i64 sig, i64 K, int alpha)
{
    for (i64 d = sig; d < 8 * sig; d += sig / 16 + 1)
        if ((i64)alpha * (65536 - phi(d, K)) < 32768) return (int)(d >> 8) + 2;
    return (int)((8 * sig) >> 8) + 2;
}

int ol_display_shadow_ex_opt(struct ol_display *ctx, int x, int y, int w, int h,
                             const int radius[4], int dx24, int dy24, int sigma24,
                             int spread24, uint32_t rgb, int alpha, int inset,
                             unsigned flags)
{
    if (!ctx || !ctx->target.px || w <= 0 || h <= 0) return 0;
    if (alpha > 255) alpha = 255;
    if (alpha <= 0) return 0;

    i64 sig = sigma24 < 32 ? 32 : sigma24;      /* below 1/8 px is a hard edge */
#ifdef OL_SHADOW_EX_NEGCTL_CLAMP
    /* Control only: the legacy 72px mask budget, i.e. 4 sigma <= 72. */
    if (sig > 18 * 256) sig = 18 * 256;
#endif
    i64 K = 3037000500LL / sig;
    /* Gaussian reach: beyond M px the shadow rounds to zero alpha, so the
     * bounding box, corner tables and inset skip all stop there. */
    int M = reach_px(sig, K, alpha);

    struct shp box, S;
    shp_make(&box, x, y, w, h, radius, 0, 0, 0);
    shp_make(&S, x, y, w, h, radius, dx24, dy24, inset ? -(i64)spread24 : spread24);
    if (!inset && S.empty) return 0;

    int bx, by, bw, bh;
    if (inset) { bx = x; by = y; bw = w; bh = h; }
    else {
        bx = (int)(S.x0 >> 8) - M; by = (int)(S.y0 >> 8) - M;
        bw = ceil8(S.x1) + M - bx; bh = ceil8(S.y1) + M - by;
    }
    int i0, j0, i1, j1;
    if (!clip_ij(ctx, bx, by, bw, bh, &i0, &j0, &i1, &j1)) return 0;
    int px0 = bx + i0, px1 = bx + i1, py0 = by + j0, py1 = by + j1;
    int W = px1 - px0;

    struct cn cn[4];
    unsigned long words = 3ul * W;
    for (int c = 0; c < 4; c++) {
        cn_prep(&cn[c], &S, c, sig, M, px0, px1);
        if (cn[c].act) words += (unsigned long)cn[c].K * cn[c].Wc;
    }
    uint32_t *mem = display_scratch(ctx, OL_DS_SHADOW, words * 4);
    if (!mem) return OL_LIMIT;
    uint32_t *cx = mem, *cxa = mem + W, *line = mem + 2ul * W, *next = mem + 3ul * W;
    for (int c = 0; c < 4; c++)
        if (cn[c].act) {
            cn[c].tab = next;
            next += (unsigned long)cn[c].K * cn[c].Wc;
            cn_fill(&cn[c], K, sig);
        }
    for (int i = 0; i < W; i++) {
        i64 pc = ((i64)(px0 + i) << 8) + 128;
        cx[i] = S.empty ? 0 : (uint32_t)(phi(S.x1 - pc, K) - phi(S.x0 - pc, K));
        cxa[i] = (uint32_t)(alpha * cx[i]) >> 8;
    }

    /* Skip crosses: two rects, the second spans rows the first does not. */
    int ax0 = 0, ax1 = 0, ay0 = 0, ay1 = 0, sx0 = 0, sx1 = 0, sy0 = 0, sy1 = 0;
    {
        const struct shp *q = inset ? &S : &box;
        int m = inset ? M : 0;
        if (!q->empty && !(!inset && (flags & OL_SHADOW_EX_BODY))) {
            int qx0 = inset ? ceil8(q->x0) : x, qx1 = inset ? (int)(q->x1 >> 8) : x + w;
            int qy0 = inset ? ceil8(q->y0) : y, qy1 = inset ? (int)(q->y1 >> 8) : y + h;
            int rt = ceil8(q->r[0] > q->r[1] ? q->r[0] : q->r[1]);
            int rb = ceil8(q->r[3] > q->r[2] ? q->r[3] : q->r[2]);
            int rl = ceil8(q->r[0] > q->r[3] ? q->r[0] : q->r[3]);
            int rr = ceil8(q->r[1] > q->r[2] ? q->r[1] : q->r[2]);
            ax0 = qx0 + m; ax1 = qx1 - m; ay0 = qy0 + rt + m; ay1 = qy1 - rb - m;
            sx0 = qx0 + rl + m; sx1 = qx1 - rr - m; sy0 = qy0 + m; sy1 = qy1 - m;
        }
    }

    struct ol_pixel_target *t = &ctx->target;
    int cr = (rgb >> 16) & 255, cg = (rgb >> 8) & 255, cb = rgb & 255;
    uint32_t packed = display_rgb(ctx, (uint8_t)cr, (uint8_t)cg, (uint8_t)cb);
    int sw = ctx->gpos == 8 && ((ctx->rpos == 0 && ctx->bpos == 16) || (ctx->rpos == 16 && ctx->bpos == 0));
    uint32_t crb = packed & 0xFF00FFu, cgp = (packed >> 8) & 255u;

    for (int py = py0; py < py1; py++) {
        i64 pyc = ((i64)py << 8) + 128;
        uint32_t cy = S.empty ? 0 : (uint32_t)(phi(S.y1 - pyc, K) - phi(S.y0 - pyc, K));
        if (!inset && cy == 0) continue;
        uint32_t wk[4][CN_KMAX];
        int any[4];
        for (int c = 0; c < 4; c++) {
            struct cn *q = &cn[c];
            any[c] = 0;
            if (!q->act || py < q->ya || py >= q->yb) continue;
            int P[CN_KMAX + 1];
            for (int j = 0; j <= q->K; j++)
                P[j] = phi((q->top ? q->ye + q->b[j] : q->ye - q->b[j]) - pyc, K);
            for (int k = 0; k < q->K; k++) {
                int d = q->top ? P[k + 1] - P[k] : P[k] - P[k + 1];
                wk[c][k] = d > 0 ? (uint32_t)d : 0;
                any[c] |= d > 0;
            }
        }

        int s0 = px1, s1 = px1;
        if (py >= ay0 && py < ay1 && ax0 < ax1) { s0 = ax0; s1 = ax1; }
        else if (py >= sy0 && py < sy1 && sx0 < sx1) { s0 = sx0; s1 = sx1; }
        uint32_t *drow = t->px + (long)py * t->w;
        for (int seg = 0; seg < 2; seg++) {
            int a = seg ? (s1 > px0 ? s1 : px0) : px0;
            int b = seg ? px1 : (s0 < px1 ? s0 : px1);
            if (a >= b) continue;
            /* Pixels a corner table touches take the two-pass path; the rest
             * (and, outside inset, every straight-edge pixel) are separable. */
            int ra[4], rb[4], nr = 0;
            for (int c = 0; c < 4; c++) {
                if (!any[c]) continue;
                int lo = a > cn[c].xa ? a : cn[c].xa, hi = b < cn[c].xb ? b : cn[c].xb;
                if (lo >= hi) continue;
                int p = nr++;
                while (p > 0 && ra[p - 1] > lo) { ra[p] = ra[p - 1]; rb[p] = rb[p - 1]; p--; }
                ra[p] = lo; rb[p] = hi;
            }
            if (inset) { nr = 1; ra[0] = a; rb[0] = b; }
            int n2 = 0, ma[4], mb[4];                   /* merge into disjoint ranges */
            for (int p = 0; p < nr; p++) {
                if (n2 && ra[p] <= mb[n2 - 1]) { if (rb[p] > mb[n2 - 1]) mb[n2 - 1] = rb[p]; }
                else { ma[n2] = ra[p]; mb[n2] = rb[p]; n2++; }
            }
            int pos = a;
            for (int g = 0; g <= n2; g++) {
                int ga = pos, gb = g < n2 ? ma[g] : b;
                if (sw) {                                    /* separable pixels */
                    for (int i = ga; i < gb; i++) {
                        unsigned al = (cxa[i - px0] * cy + (1u << 23)) >> 24;
                        if (al) drow[i] = blend_sw(drow[i], crb, cgp, al);
                    }
                } else {
                    for (int i = ga; i < gb; i++) {
                        unsigned al = (cxa[i - px0] * cy + (1u << 23)) >> 24;
                        if (al) drow[i] = blend_gen(ctx, drow[i], cr, cg, cb, (int)al);
                    }
                }
                if (g == n2) break;
                int ca = ma[g], cbn = mb[g];
                pos = cbn;
                for (int i = ca; i < cbn; i++)
                    line[i - px0] = (uint32_t)(((unsigned long long)cx[i - px0] * cy) >> 16);
                for (int c = 0; c < 4; c++) {
                    struct cn *q = &cn[c];
                    if (!any[c]) continue;
                    int lo = ca > q->xa ? ca : q->xa, hi = cbn < q->xb ? cbn : q->xb;
                    for (int k = 0; k < q->K && lo < hi; k++) {
                        uint32_t wv = wk[c][k];
                        const uint32_t *tr = q->tab + (long)k * q->Wc - q->xa;
                        if (!wv) continue;
                        for (int i = lo; i < hi; i++) {
                            uint32_t sub = (wv * tr[i]) >> 16, *l = &line[i - px0];
                            *l = *l > sub ? *l - sub : 0;
                        }
                    }
                }
                for (int i = ca; i < cbn; i++) {
                    uint32_t cv = line[i - px0];
                    int A = (int)(cv > 65535 ? 65535 : cv), al;
                    if (inset) {
                        A = 65535 - A;
                        al = (alpha * A + 32768) >> 16;
                        if (al) {
                            int bc = box_cov(&box, i, py);
                            if (bc != 255) al = div255(al * bc);
                        }
                    } else al = (alpha * A + 32768) >> 16;
                    if (al <= 0) continue;
                    drow[i] = sw ? blend_sw(drow[i], crb, cgp, (unsigned)al)
                                 : blend_gen(ctx, drow[i], cr, cg, cb, al);
                }
            }
        }
    }
    return 0;
}

int ol_display_shadow_ex(struct ol_display *ctx, int x, int y, int w, int h,
                         const int radius[4], int dx24, int dy24, int sigma24,
                         int spread24, uint32_t rgb, int alpha, int inset)
{
    return ol_display_shadow_ex_opt(ctx, x, y, w, h, radius, dx24, dy24, sigma24,
                                    spread24, rgb, alpha, inset, 0);
}
