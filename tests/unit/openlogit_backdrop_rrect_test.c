/* ol_cmd_backdrop_rrect: rounded frosted panels. Oracles are independent of
 * the implementation: a direct box convolution, a supersampled rounded-rect
 * coverage, and a discrete gaussian. */
#include "openlogit.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define W 96
#define H 80
static int checks, failed;
#define CHECK(c, s) do { checks++; if (!(c)) { failed++; printf("FAIL %s\n", s); } else printf("PASS %s\n", s); } while (0)
static void *allocate(unsigned long n) { void *p = calloc(1, n); if (!p) exit(2); return p; }

struct rig {
    struct ol_device *d; struct ol_surface *s; struct ol_list *l;
    unsigned char front[W * H * 4], work[W * H * 4];
};
static void rig_init(struct rig *g)
{
    struct ol_surface_desc sd = {sizeof sd, OL_FORMAT_RGBA8_STRAIGHT, W, H, W * 4, g->front, g->work,
                                 sizeof g->front, sizeof g->work};
    void *dm = allocate(ol_device_size()), *sm = allocate(ol_surface_size()), *lm = allocate(65536);
    if (ol_device_create(dm, ol_device_size(), OL_API_VERSION, OL_CAP_LAYER | OL_CAP_BLUR | OL_CAP_BACKDROP, &g->d) ||
        ol_surface_create(g->d, sm, ol_surface_size(), &sd, &g->s) || ol_list_create(g->d, lm, 65536, &g->l)) exit(2);
}
static void paint(struct rig *g, int mode)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        unsigned char *p = g->front + (y * W + x) * 4;
        if (mode == 0) { p[0] = p[1] = p[2] = 0; }
        else if (mode == 1) { p[0] = (x * 37 + y * 11) & 255; p[1] = (x * 5 + y * 29) & 255; p[2] = ((x ^ y) * 13) & 255; }
        else { p[0] = p[1] = p[2] = (x >= 46 && x < 54) ? 255 : 0; }
        p[3] = 255;
    }
}
static int submit(struct rig *g)
{
    unsigned long need = 0; ol_list_close(g->l);
    if (ol_submit_workspace_size(g->l, g->s, &need)) return -1;
    void *sc = allocate(need + 16);
    int r = ol_submit_workspace(g->d, g->l, g->s, sc, need, 0);
    free(sc); ol_list_reset(g->l); return r;
}
/* Supersampled coverage of pixel (x,y) of a w*h rounded rect, radii TL,TR,BR,BL. */
static int ss_cov(const int cr[4], int w, int h, int x, int y)
{
    const int N = 32; int in = 0;
    for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
        double px = x + (i + 0.5) / N, py = y + (j + 0.5) / N, cx = -1, cy = -1, r = 0;
        if (px < cr[0] && py < cr[0]) { cx = cr[0]; cy = cr[0]; r = cr[0]; }
        else if (px > w - cr[1] && py < cr[1]) { cx = w - cr[1]; cy = cr[1]; r = cr[1]; }
        else if (px > w - cr[2] && py > h - cr[2]) { cx = w - cr[2]; cy = h - cr[2]; r = cr[2]; }
        else if (px < cr[3] && py > h - cr[3]) { cx = cr[3]; cy = h - cr[3]; r = cr[3]; }
        if (r == 0 || hypot(px - cx, py - cy) <= r) in++;
    }
    return (in * 255 + N * N / 2) / (N * N);
}
static int pix(const struct rig *g, int x, int y, int k) { return g->front[(y * W + x) * 4 + k]; }

int main(void)
{
    static struct rig g, ref; rig_init(&g); rig_init(&ref);
    const struct gfx_rect R = {24, 20, 48, 40};
    const int c16[4] = {16, 16, 16, 16}, c0[4] = {0, 0, 0, 0};

    /* corner: flat black source, opaque white tint => pixel == coverage. */
    paint(&g, 0);
    int rc = ol_cmd_backdrop_rrect(g.l, R, c16, 4, 1, 0xffffff, 255);
    CHECK(rc == OL_OK && submit(&g) == OL_OK, "rrect backdrop records and submits");
    int worst = 0, outside_ok = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int in = x >= R.x && x < R.x + R.w && y >= R.y && y < R.y + R.h;
        int want = in ? ss_cov(c16, R.w, R.h, x - R.x, y - R.y) : 0;
        for (int k = 0; k < 3; k++) {
            int e = abs(pix(&g, x, y, k) - want);
            if (in && e > worst) worst = e;
            if (!in && e) outside_ok = 0;
        }
    }
    printf("corner coverage max error %d/255\n", worst);
    CHECK(worst <= 2, "corner coverage within 2/255 of supersampled");
    CHECK(pix(&g, R.x, R.y, 0) == 0 && pix(&g, R.x + 24, R.y + 20, 0) == 255 && pix(&g, R.x + 24, R.y, 0) == 255,
          "corner pixel is untouched and straight edges are fully covered");
    CHECK(outside_ok, "outside the shape is untouched");

    /* interior/outside on a textured source with a translucent tint */
    paint(&g, 1); paint(&ref, 1);
    ol_cmd_backdrop_rrect(g.l, R, c16, 4, 1, 0x204080, 100); submit(&g);
    int interior_err = 0, exact_out = 1, n_in = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int cov = (x >= R.x && x < R.x + R.w && y >= R.y && y < R.y + R.h)
                      ? ss_cov(c16, R.w, R.h, x - R.x, y - R.y) : 0;
        if (cov == 255) {
            n_in++;
            for (int k = 0; k < 3; k++) {
                long s = 0;
                for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) s += pix(&ref, x + dx, y + dy, k);
                double b = s / 81.0, t = (k == 0 ? 0x20 : k == 1 ? 0x40 : 0x80);
                double want = b + (t - b) * 100 / 255.0;
                int e = (int)fabs(pix(&g, x, y, k) - want);
                if (e > interior_err) interior_err = e;
            }
        } else if (cov == 0)
            for (int k = 0; k < 4; k++) if (pix(&g, x, y, k) != pix(&ref, x, y, k)) exact_out = 0;
    }
    printf("interior max error %d (%d px)\n", interior_err, n_in);
    CHECK(n_in > 1000 && interior_err <= 2, "interior is blurred and tinted (independent convolution)");
    CHECK(exact_out, "source outside the shape is bit-exact");

    /* zero corners, one pass == legacy */
    paint(&g, 1); paint(&ref, 1);
    ol_cmd_backdrop(ref.l, R, 6, 0x335577, 140); submit(&ref);
    ol_cmd_backdrop_rrect(g.l, R, c0, 6, 1, 0x335577, 140); submit(&g);
    CHECK(!memcmp(g.front, ref.front, sizeof g.front), "zero corners with 1 pass are bit-identical to legacy");
    paint(&g, 1);
    ol_cmd_backdrop_rrect(g.l, R, c16, 6, 1, 0x335577, 140); submit(&g);
    CHECK(memcmp(g.front, ref.front, sizeof g.front) != 0, "rounded corners differ from the legacy rect");

    /* 3 passes ~ gaussian: 8px white band on black, radius 9 => 3 boxes of r=3 */
    const struct gfx_rect Q = {12, 8, 72, 64};
    paint(&g, 2);
    ol_cmd_backdrop_rrect(g.l, Q, c0, 9, 3, 0, 0); submit(&g);
    double sigma2 = 3 * ((7.0 * 7.0) - 1) / 12.0, l1 = 0, tot = 0, kern[200];
    int row = 40;
    double ks = 0;
    for (int i = -30; i <= 30; i++) { kern[i + 30] = exp(-i * i / (2 * sigma2)); ks += kern[i + 30]; }
    for (int x = Q.x; x < Q.x + Q.w; x++) {
        double want = 0;
        for (int u = 46; u < 54; u++) { int d = x - u; if (d >= -30 && d <= 30) want += 255 * kern[d + 30] / ks; }
        l1 += fabs(pix(&g, x, row, 0) - want); tot += want;
    }
    printf("3-pass vs gaussian L1 %.2f%%\n", 100 * l1 / tot);
    CHECK(l1 / tot < 0.06, "3 passes match the gaussian within L1 6%");
    paint(&ref, 2);
    ol_cmd_backdrop_rrect(ref.l, Q, c0, 9, 1, 0, 0); submit(&ref);
    CHECK(memcmp(g.front, ref.front, sizeof g.front) != 0, "3 passes differ from 1 pass");

    /* workspace + damage */
    paint(&g, 1);
    ol_cmd_backdrop_rrect(g.l, R, c16, 9, 3, 0, 0); ol_list_close(g.l);
    unsigned long need = 0; struct ol_submit_info info = {.size = sizeof info};
    ol_submit_workspace_size(g.l, g.s, &need);
    void *sc = allocate(need + 16);
    CHECK(need > 2UL * 54 * 46 * 4 && ol_submit_workspace(g.d, g.l, g.s, sc, need - 1, 0) == OL_LIMIT &&
          ol_submit_workspace(g.d, g.l, g.s, sc, need, &info) == OL_OK, "workspace size covers the extra passes exactly");
    CHECK(info.damage.x == R.x && info.damage.y == R.y && info.damage.w == R.w && info.damage.h == R.h,
          "damage stays the logical rect");
    free(sc); ol_list_reset(g.l);

    /* arguments */
    const int bad[4] = {-1, 0, 0, 0};
    CHECK(ol_cmd_backdrop_rrect(g.l, R, c16, 4, 2, 0, 0) == OL_ARGUMENT, "passes other than 1 or 3 are rejected");
    ol_list_reset(g.l);
    CHECK(ol_cmd_backdrop_rrect(g.l, R, bad, 4, 1, 0, 0) == OL_ARGUMENT, "negative corner is rejected");
    ol_list_reset(g.l);
    CHECK(ol_cmd_backdrop_rrect(g.l, R, 0, 4, 1, 0, 0) == OL_ARGUMENT, "null corner array is rejected");
    ol_list_reset(g.l);
    /* oversize radius clamps to a pill: half the short side */
    const int pill[4] = {60, 60, 60, 60}, half[4] = {20, 20, 20, 20};
    paint(&g, 0); paint(&ref, 0);
    ol_cmd_backdrop_rrect(g.l, R, pill, 0, 1, 0xffffff, 255); submit(&g);
    ol_cmd_backdrop_rrect(ref.l, R, half, 0, 1, 0xffffff, 255); submit(&ref);
    CHECK(!memcmp(g.front, ref.front, sizeof g.front), "radius larger than half the short side clamps to a pill");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
