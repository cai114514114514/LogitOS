/* Font outline test: compare our parsers against two independent references.
 *
 *   PATHS    every glyph's outline, in font units, integer-for-integer against
 *            fontTools' own charstring interpreter / glyf decompiler. This is
 *            the bar the H.264 decoder set: a Type 2 charstring is exactly
 *            specified arithmetic, so any disagreement is our bug. It is also
 *            the only check that catches the failure mode that matters here --
 *            a subtly wrong interpreter draws a plausible-looking letter that
 *            is wrong, and no "it rendered something" assertion notices.
 *
 *   BITMAPS  the rasterized coverage against FreeType at the same pixel size,
 *            unhinted, to a stated tolerance. FreeType computes exact area
 *            coverage on a 26.6 grid; we sample four sub-scanlines on a 24.8
 *            one, so the two cannot agree byte for byte and pretending
 *            otherwise would just mean picking a threshold that hides bugs.
 *            The thresholds below are roughly a third of what a genuinely
 *            wrong outline produces (a misplaced contour scores 60+ mean).
 *
 * Usage: font_cff_test FONT REF.bin [--px N] [--verbose]
 *        (REF.bin comes from tests/unit/font_ref_gen.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ttf.h"
#include "fontpath.h"
#include "text.h"

static int fails;
static int verbose;

#define FAILF(...) do { if (fails < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } while (0)

/* ------------------------------------------------------------ ref reader -- */

struct refbuf { const uint8_t *d; size_t n, p; };

static uint32_t r32(struct refbuf *r)
{
    if (r->p + 4 > r->n) { r->p = r->n; return 0; }
    uint32_t v = (uint32_t)r->d[r->p] | ((uint32_t)r->d[r->p+1] << 8) |
                 ((uint32_t)r->d[r->p+2] << 16) | ((uint32_t)r->d[r->p+3] << 24);
    r->p += 4;
    return v;
}
static int32_t ri32(struct refbuf *r) { return (int32_t)r32(r); }
static uint8_t r8(struct refbuf *r) { return r->p < r->n ? r->d[r->p++] : 0; }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 0; }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

static const char *opname(int op)
{
    static const char *n[] = { "move", "line", "quad", "cubic", "close" };
    return (op >= 0 && op <= 4) ? n[op] : "?";
}

/* ---------------------------------------------------------------- main -- */

static struct fp_cmd cmdbuf[65536];
static uint8_t scratch[1 << 20];
static uint8_t cov[1024 * 1024];

int main(int argc, char **argv)
{
    const char *fontpath = argc > 1 ? argv[1] : 0;
    const char *refpath  = argc > 2 ? argv[2] : 0;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) verbose = 1;
    if (!fontpath || !refpath) { printf("usage: font_cff_test FONT REF.bin\n"); return 2; }

    size_t flen = 0, rlen = 0;
    uint8_t *fdata = slurp(fontpath, &flen);
    uint8_t *rdata = slurp(refpath, &rlen);
    if (!fdata) { printf("no font %s\n", fontpath); return 2; }
    if (!rdata) { printf("no ref %s\n", refpath); return 2; }

    struct refbuf r = { rdata, rlen, 0 };
    if (rlen < 24 || memcmp(rdata, "FTRF", 4)) { printf("bad ref magic\n"); return 2; }
    r.p = 4;
    uint32_t ver = r32(&r), upem = r32(&r), px = r32(&r);
    uint32_t npaths = r32(&r), nbitmaps = r32(&r);
    if (ver != 1) { printf("bad ref version %u\n", ver); return 2; }

    struct ttf_font f;
    if (ttf_parse(fdata, (int)flen, &f) != 0) {
        printf("FAIL ttf_parse(%s) rejected the font\n", fontpath);
        return 1;
    }
    printf("%s: %s outlines, %d glyphs, upem %d\n", fontpath,
           f.outline_fmt == TTF_OUTLINE_CFF ? "CFF" : "glyf", f.num_glyphs, f.units_per_em);
    if ((uint32_t)f.units_per_em != upem)
        FAILF("upem %d, reference says %u", f.units_per_em, upem);

    /* ---- paths ---- */
    int checked = 0, empty = 0, cmds_checked = 0;
    for (uint32_t i = 0; i < npaths; i++) {
        int32_t gid = ri32(&r);
        int32_t adv = ri32(&r);
        uint32_t ncmd = r32(&r);

        struct fp_path p;
        fp_init(&p, cmdbuf, (int)sizeof cmdbuf);
        int rc = ttf_glyph_path(&f, gid, &p, scratch, (int)sizeof scratch);

        int got_adv = ttf_advance(&f, gid);
        if (got_adv != adv) FAILF("gid %d advance %d, want %d", gid, got_adv, adv);

        if (rc != 0) {
            FAILF("gid %d: ttf_glyph_path failed (reference has %u commands)", gid, ncmd);
            /* still consume the reference commands */
            for (uint32_t k = 0; k < ncmd; k++) { r8(&r); for (int j = 0; j < 6; j++) r32(&r); }
            continue;
        }
        if ((uint32_t)p.n != ncmd)
            FAILF("gid %d: %d commands, want %u", gid, p.n, ncmd);
        uint32_t lim = ncmd;
        for (uint32_t k = 0; k < lim; k++) {
            int op = r8(&r);
            int32_t x[3], y[3];
            for (int j = 0; j < 3; j++) x[j] = ri32(&r);
            for (int j = 0; j < 3; j++) y[j] = ri32(&r);
            if ((uint32_t)p.n <= k) continue;
            const struct fp_cmd *c = &p.cmd[k];
            int npt = (op == 3) ? 3 : (op == 2) ? 2 : (op == 4) ? 0 : 1;
            int bad = (c->op != op);
            for (int j = 0; j < npt && !bad; j++)
                if (c->x[j] != x[j] || c->y[j] != y[j]) bad = 1;
            if (bad) {
                FAILF("gid %d cmd %u: got %s (%d,%d)(%d,%d)(%d,%d), want %s (%d,%d)(%d,%d)(%d,%d)",
                      gid, k, opname(c->op), c->x[0], c->y[0], c->x[1], c->y[1], c->x[2], c->y[2],
                      opname(op), x[0], y[0], x[1], y[1], x[2], y[2]);
            }
            cmds_checked++;
        }
        if (ncmd == 0) empty++;
        checked++;
    }
    printf("paths: %d glyphs (%d blank), %d commands compared exactly\n",
           checked, empty, cmds_checked);

    /* ---- bitmaps ---- */
    long total_mean_x100 = 0;
    int worst_mean_x100 = 0, worst_gid = -1, worst_max = 0, nbm = 0;
    for (uint32_t i = 0; i < nbitmaps; i++) {
        int32_t gid = ri32(&r);
        int32_t left = ri32(&r), top = ri32(&r);
        uint32_t w = r32(&r), h = r32(&r);
        if (r.p + (size_t)w * h > r.n) { printf("ref truncated\n"); fails++; break; }
        const uint8_t *ref = rdata + r.p;
        r.p += (size_t)w * h;

        int ow, oh, ox, oy;
        if (text_raster(&f, gid, (int)px, cov, (int)sizeof cov, &ow, &oh, &ox, &oy) != 0) {
            FAILF("gid %d: text_raster failed at %upx", gid, px);
            continue;
        }
        if (ow == 0 || oh == 0) {
            FAILF("gid %d: we drew nothing, FreeType drew %ux%u", gid, w, h);
            continue;
        }
        /* Compare over the union of the two boxes in pen-relative coordinates:
         * x from the pen origin, y measured DOWN from the baseline. */
        int ax0 = ox, ay0 = -oy, ax1 = ox + ow, ay1 = -oy + oh;
        int bx0 = left, by0 = -top, bx1 = left + (int)w, by1 = -top + (int)h;
        int ux0 = ax0 < bx0 ? ax0 : bx0, uy0 = ay0 < by0 ? ay0 : by0;
        int ux1 = ax1 > bx1 ? ax1 : bx1, uy1 = ay1 > by1 ? ay1 : by1;
        long sum = 0; int mx = 0; long npix = 0;
        for (int y = uy0; y < uy1; y++) {
            for (int x = ux0; x < ux1; x++) {
                int a = (x >= ax0 && x < ax1 && y >= ay0 && y < ay1)
                        ? cov[(y - ay0) * ow + (x - ax0)] : 0;
                int b = (x >= bx0 && x < bx1 && y >= by0 && y < by1)
                        ? ref[(y - by0) * (int)w + (x - bx0)] : 0;
                int d = a > b ? a - b : b - a;
                sum += d; npix++;
                if (d > mx) mx = d;
            }
        }
        int mean_x100 = npix ? (int)((sum * 100) / npix) : 0;
        total_mean_x100 += mean_x100;
        nbm++;
        if (mean_x100 > worst_mean_x100) { worst_mean_x100 = mean_x100; worst_gid = gid; worst_max = mx; }
        if (verbose)
            printf("  gid %-5d mean %2d.%02d max %3d  (ours %dx%d@%d,%d  ft %ux%u@%d,%d)\n",
                   gid, mean_x100 / 100, mean_x100 % 100, mx, ow, oh, ox, oy, w, h, left, top);
        /* Per-glyph gate. A correct outline drawn by a different AA rule stays
         * well under this; a wrong one does not. */
        if (mean_x100 > 900)
            FAILF("gid %d: mean |coverage difference| %d.%02d vs FreeType (max %d)",
                  gid, mean_x100 / 100, mean_x100 % 100, mx);
    }
    if (nbm) {
        int avg = (int)(total_mean_x100 / nbm);
        printf("bitmaps: %d glyphs at %upx vs FreeType, mean |diff| %d.%02d/255, "
               "worst glyph %d at %d.%02d (max pixel %d)\n",
               nbm, px, avg / 100, avg % 100, worst_gid,
               worst_mean_x100 / 100, worst_mean_x100 % 100, worst_max);
        if (avg > 400) { printf("FAIL average coverage difference %d.%02d too large\n",
                                avg / 100, avg % 100); fails++; }
    }

    printf(fails ? "SOME FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
