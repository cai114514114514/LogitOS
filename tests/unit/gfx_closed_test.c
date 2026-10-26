/* gfx-path-closed-bit: gfx_close() records closure in the sub[] high bit
 * (GFX_SUB_CLOSED) and the stroker uses it.
 *
 * Checks (each prints "ok <name>" or "FAIL <name>"; exit status is non-zero on
 * any failure):
 *   rect-join      a stroked gfx_path_rect (miter) agrees with an analytic
 *                  supersampled oracle (outer box minus inner box) at offsets
 *                  0, 0.25 and 0.5 px, and is IDENTICAL to a hand-closed
 *                  reference (move_to + 4 line_to back to the start). With the
 *                  bit ignored the corner has two butt caps instead of a join.
 *   rect-join-w3   the same at width 3 within the error budget.
 *   dash-seam-rect a dashed closed rect whose on-run spans the seam has a
 *                  joined (mitered) corner there, and still has its gap.
 *   dash-solid     a dash pattern longer than the contour strokes the whole
 *                  contour as one closed ring.
 *   dash-circle    a dashed circle follows the arc-length pattern and is
 *                  continuous across theta=0 (the legacy endpoint rule closes
 *                  it, so this holds with and without the bit).
 *   fill-crc       fills of rect / rrect / circle are bit-identical to the
 *                  pre-change output (CRCs taken on the base commit).
 *   path-ok        ol_cmd_fill accepts a closed path and still rejects a
 *                  corrupt sub[] entry.
 *   bit-set        gfx_close sets the bit, adds no point, sets it only once
 *                  a segment has opened the subpath.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gfx.h"
#include "openlogit.h"

static int fails, checks;

static void ck(int cond, const char *what, const char *detail)
{
    checks++;
    printf("%s %s%s%s\n", cond ? "ok  " : "FAIL", what, detail && *detail ? "  " : "",
           detail ? detail : "");
    if (!cond) fails++;
}

#define W 48
#define H 48
static int SPT[2048 * 2], SSUB[16], OPT[8192 * 2], OSUB[64];
static unsigned char M[W * H], M2[W * H];

static void src_begin(struct gfx_path *p)
{
    gfx_path_init(p, SPT, 2048, SSUB, 16);
    gfx_path_tolerance(p, GFX_ONE / 64);
}

static void stroke_into(unsigned char *m, const struct gfx_path *src, const struct gfx_stroke *st)
{
    struct gfx_path out;
    gfx_path_init(&out, OPT, 8192, OSUB, 64);
    memset(m, 0, W * H);
    if (!gfx_stroke_path(&out, src, st)) { printf("FAIL stroke refused\n"); fails++; return; }
    gfx_fill_mask_subs(&out, GFX_NONZERO, m, W, H, 0, 0, 16);
}

static void st_init(struct gfx_stroke *s, int width, int cap, int join)
{
    memset(s, 0, sizeof *s);
    s->width = width; s->cap = cap; s->join = join;
    s->miter_limit = 4 << 16;
}

#ifndef PRINT_CRC
/* ---- analytic oracle: ring between two axis-aligned boxes, 16x16 samples */
static double box_cov(int i, int j, double ox0, double oy0, double ox1, double oy1,
                      double ix0, double iy0, double ix1, double iy1)
{
    int n = 0;
    for (int sy = 0; sy < 16; sy++)
        for (int sx = 0; sx < 16; sx++) {
            double x = i + (sx + 0.5) / 16, y = j + (sy + 0.5) / 16;
            int in_out = x >= ox0 && x < ox1 && y >= oy0 && y < oy1;
            int in_in = x > ix0 && x < ix1 && y > iy0 && y < iy1;
            n += in_out && !in_in;
        }
    return n / 256.0;
}

static double ring_err(const unsigned char *m, double x0, double y0, double x1, double y1, double hw)
{
    double worst = 0;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            double r = box_cov(i, j, x0 - hw, y0 - hw, x1 + hw, y1 + hw,
                               x0 + hw, y0 + hw, x1 - hw, y1 - hw) * 255.0;
            double e = fabs(m[j * W + i] - r);
            if (e > worst) worst = e;
        }
    return worst;
}

#define BUDGET 8.0   /* 16-sample fill vs 16x16 oracle, 8-bit coverage */

static void build_rect_hand(struct gfx_path *p, int x, int y, int w, int h)
{
    gfx_move_to(p, x, y);
    gfx_line_to(p, x + w, y);
    gfx_line_to(p, x + w, y + h);
    gfx_line_to(p, x, y + h);
    gfx_line_to(p, x, y);        /* legacy closure: last point == first */
}

static void rect_join(void)
{
    static const int offs[3] = {0, 64, 128};     /* 0, .25, .5 px in 24.8 */
    int widths[2] = {GFX_PX(4), GFX_PX(3)};
    for (int wi = 0; wi < 2; wi++) {
        double hw = widths[wi] / 512.0, worst = 0;
        int same = 1;
        char d[96];
        for (int k = 0; k < 3; k++) {
            struct gfx_path a, b;
            struct gfx_stroke st;
            int ox = GFX_PX(10) + offs[k], oy = GFX_PX(9) + offs[k];
            st_init(&st, widths[wi], GFX_CAP_BUTT, GFX_JOIN_MITER);
            src_begin(&a); gfx_path_rect(&a, ox, oy, GFX_PX(26), GFX_PX(20));
            stroke_into(M, &a, &st);
            src_begin(&b); build_rect_hand(&b, ox, oy, GFX_PX(26), GFX_PX(20));
            stroke_into(M2, &b, &st);
            double e = ring_err(M, ox / 256.0, oy / 256.0, ox / 256.0 + 26, oy / 256.0 + 20, hw);
            if (e > worst) worst = e;
            if (memcmp(M, M2, sizeof M)) same = 0;
        }
        snprintf(d, sizeof d, "worst err %.1f/255 (budget %.0f)", worst, BUDGET);
        ck(worst <= BUDGET, wi ? "rect-join-w3 oracle" : "rect-join oracle", d);
        ck(same, wi ? "rect-join-w3 equals hand-closed" : "rect-join equals hand-closed", "");
    }
}

/* ---- dashed closed rect: seam at the top-left corner */
static void dash_seam_rect(void)
{
    struct gfx_path a;
    struct gfx_stroke st;
    int dash[2] = {GFX_PX(40), GFX_PX(10)};
    st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_MITER);
    st.dash = dash; st.ndash = 2;
    src_begin(&a);
    gfx_path_rect(&a, GFX_PX(8), GFX_PX(8), GFX_PX(24), GFX_PX(20));
    stroke_into(M, &a, &st);
    /* outer corner square [6,8)x[6,8) is covered only by a join */
    ck(M[6 * W + 6] >= 250 && M[7 * W + 7] >= 250 && M[6 * W + 7] >= 250,
       "dash-seam corner is joined", "");
    /* the off gap on the right edge (arc length 40..50 = y 24..28) */
    ck(M[26 * W + 32] < 8 && M[26 * W + 31] < 8, "dash-seam-rect gap kept", "");
    ck(M[16 * W + 32] >= 250, "dash-seam-rect on-run solid", "");
}

static void dash_solid(void)
{
    struct gfx_path a;
    struct gfx_stroke st;
    int dash[2] = {GFX_PX(1000), GFX_PX(1)};
    int bad = 0;
    st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_MITER);
    st.dash = dash; st.ndash = 2;
    src_begin(&a);
    gfx_path_rect(&a, GFX_PX(8), GFX_PX(8), GFX_PX(24), GFX_PX(20));
    stroke_into(M, &a, &st);
    for (int x = 8; x < 32; x++) if (M[8 * W + x] < 250 || M[27 * W + x] < 200) bad++;
    for (int y = 8; y < 28; y++) if (M[y * W + 8] < 250 || M[y * W + 31] < 250) bad++;
    if (M[6 * W + 6] < 250 || M[29 * W + 33] < 250) bad++;
    ck(!bad, "dash-solid whole contour is one closed ring", "");
}

static void dash_circle(void)
{
    struct gfx_path a;
    struct gfx_stroke st;
    int dash[2] = {GFX_PX(20), GFX_PX(10)};
    const double cx = 24, cy = 24, r = 16, circ = 2 * 3.14159265358979 * r;
    int bad = 0, n = 0, dir;
    st_init(&st, GFX_PX(4), GFX_CAP_BUTT, GFX_JOIN_MITER);
    st.dash = dash; st.ndash = 2;
    src_begin(&a);
    gfx_path_circle(&a, GFX_PX(24), GFX_PX(24), GFX_PX(16));
    dir = a.pt[3] > a.pt[1] ? 1 : -1;          /* which way the builder sweeps */
    stroke_into(M, &a, &st);
    /* Arc length s from the start (cx+r, cy). The pattern is 20 on / 10 off, so
     * the last, partial period (90..100.5, on) and the first on-run (0..20)
     * are ONE run across theta=0 -- there is no cap or gap at the seam. */
    for (double s = 0; s < circ; s += 0.5) {
        double ph = fmod(s, 30), th = dir * s / r;
        double dist = ph < 20 ? fmin(ph, 20 - ph) : fmin(ph - 20, 30 - ph);
        if (dist < 2.0) continue;                  /* near a dash edge */
        double x = cx + r * cos(th), y = cy + r * sin(th);
        int v = M[(int)y * W + (int)x];
        n++;
        if (ph < 20 ? v < 200 : v > 60) bad++;
    }
    ck(!bad && n > 100, "dash-circle follows the pattern, continuous at theta=0", "");
}

#endif /* !PRINT_CRC */

/* ---- fills bit-identical: CRCs recorded on the base commit */
static unsigned crc32(const unsigned char *p, int n)
{
    unsigned c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

/* CRCs of the three fills, recorded on the base commit (before the closed bit
 * existed): build with -DPRINT_CRC against the old sources to regenerate. */
#define EXPECT_RECT 1014107995u
#define EXPECT_RRECT 3115470858u
#define EXPECT_CIRCLE 1102276031u

static unsigned fill_crc(int kind, int off)
{
    struct gfx_path p;
    src_begin(&p);
    if (kind == 0) gfx_path_rect(&p, GFX_PX(5) + off, GFX_PX(6) + off, GFX_PX(30), GFX_PX(25));
    else if (kind == 1) gfx_path_rrect(&p, GFX_PX(5) + off, GFX_PX(6) + off, GFX_PX(30), GFX_PX(25), GFX_PX(7));
    else gfx_path_circle(&p, GFX_PX(22) + off, GFX_PX(23) + off, GFX_PX(15));
    memset(M, 0, sizeof M);
    gfx_fill_mask_subs(&p, GFX_NONZERO, M, W, H, 0, 0, 16);
    return crc32(M, sizeof M);
}

static void fill_crcs(void)
{
    static const int offs[3] = {0, 77, 128};
    unsigned got[3];
    for (int kind = 0; kind < 3; kind++) {
        unsigned c = 0;
        for (int k = 0; k < 3; k++) c = c * 31u + fill_crc(kind, offs[k]);
        got[kind] = c;
    }
#ifdef PRINT_CRC
    printf("#define EXPECT_RECT %uu\n#define EXPECT_RRECT %uu\n#define EXPECT_CIRCLE %uu\n", got[0], got[1], got[2]);
#else
    ck(got[0] == EXPECT_RECT, "fill-crc rect", "");
    ck(got[1] == EXPECT_RRECT, "fill-crc rrect", "");
    ck(got[2] == EXPECT_CIRCLE, "fill-crc circle", "");
#endif
}

#ifndef PRINT_CRC
static void path_ok(void)
{
    void *dm = calloc(1, ol_device_size()), *lm = calloc(1, 65536);
    struct ol_device *d;
    struct ol_list *l;
    struct gfx_path p;
    struct gfx_paint paint;
    int r1, r2, r3;
    memset(&paint, 0, sizeof paint);
    paint.kind = GFX_SOLID; paint.alpha = 255; paint.global_alpha = 255;
    if (ol_device_create(dm, ol_device_size(), OL_API_VERSION, OL_CAP_ATOMIC_FRAME, &d) ||
        ol_list_create(d, lm, 65536, &l)) abort();
    src_begin(&p);
    gfx_path_rect(&p, GFX_PX(2), GFX_PX(2), GFX_PX(8), GFX_PX(8));
    gfx_move_to(&p, GFX_PX(20), GFX_PX(20));
    gfx_line_to(&p, GFX_PX(30), GFX_PX(20));
    gfx_line_to(&p, GFX_PX(30), GFX_PX(30));
    gfx_close(&p);
    r1 = ol_cmd_fill(l, &p, GFX_NONZERO, &paint, 0, 4);
    ck(r1 == OL_OK && (p.sub[0] & GFX_SUB_CLOSED) && (p.sub[1] & GFX_SUB_CLOSED),
       "path-ok accepts closed paths", "");
    p.sub[1] = GFX_SUB_CLOSED | 0x20000000;            /* index far past npt */
    r2 = ol_cmd_fill(l, &p, GFX_NONZERO, &paint, 0, 4);
    p.sub[1] = GFX_SUB_CLOSED | gfx_path_sub_start(&p, 0);   /* not increasing */
    r3 = ol_cmd_fill(l, &p, GFX_NONZERO, &paint, 0, 4);
    ck(r2 != OL_OK && r3 != OL_OK, "path-ok rejects corrupt sub entries", "");
    ol_list_destroy(l); ol_device_destroy(d); free(lm); free(dm);
}

static void bit_set(void)
{
    struct gfx_path p;
    src_begin(&p);
    gfx_move_to(&p, 0, 0);
    gfx_close(&p);                               /* nothing open yet */
    ck(p.nsub == 0, "bit-set close before any segment is a no-op", "");
    gfx_path_rect(&p, 0, 0, GFX_PX(4), GFX_PX(4));
    ck(p.npt == 4 && p.nsub == 1 && gfx_path_sub_closed(&p, 0) && gfx_path_sub_start(&p, 0) == 0,
       "bit-set rect: 4 points, closed, start masked", "");
    gfx_move_to(&p, GFX_PX(9), GFX_PX(9));
    gfx_line_to(&p, GFX_PX(12), GFX_PX(9));
    gfx_line_to(&p, GFX_PX(12), GFX_PX(12));
    ck(!gfx_path_sub_closed(&p, 1) && gfx_path_sub_start(&p, 1) == 4,
       "bit-set open subpath stays open", "");
}

#endif

int main(void)
{
#ifndef PRINT_CRC
    bit_set();
    rect_join();
    dash_seam_rect();
    dash_solid();
    dash_circle();
#endif
    fill_crcs();
#ifndef PRINT_CRC
    path_ok();
    printf("%d checks, %d failed\n", checks, fails);
#endif
    return fails != 0;
}
