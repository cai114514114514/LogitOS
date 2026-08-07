/* Cheap smoke test for the OpenType reader: the header fields, the cmap, hmtx,
 * and a glyf outline, against values pinned to the tracked fsroot/fonts/ui.ttf.
 *
 * The heavy lifting is `make test-font`, which compares EVERY glyph's outline
 * with fontTools and the rasterized coverage with FreeType. This one is here so
 * that a break in the basics reports in one line instead of ten thousand, and
 * because a test with no absolute numbers in it anywhere can drift with the
 * thing it is testing. The numbers below belong to the ui.ttf whose hash is in
 * fsroot/fonts/SHA256SUMS; `make verify-fonts` is what keeps that honest, and if
 * tools/mkfont.py ever changes the subset these must be re-pinned deliberately.
 *
 * (They HAD drifted: this file was wired to no Makefile target and still
 * expected the ascent of a long-superseded subset.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ttf.h"
static int fail;
static void eq(const char *what, int got, int want){
    if (got != want){ printf("FAIL %s: got %d want %d\n", what, got, want); fail = 1; }
}
int main(int argc, char **argv){
    const char *path = argc > 1 ? argv[1] : "fsroot/fonts/ui.ttf";
    FILE *fp = fopen(path, "rb"); if(!fp){ printf("no %s\n", path); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(n); if (fread(buf, 1, n, fp) != (size_t)n) return 2; fclose(fp);

    struct ttf_font f;
    eq("parse", ttf_parse(buf, (int)n, &f), 0);
    eq("outline format", f.outline_fmt, TTF_OUTLINE_GLYF);
    eq("upem", f.units_per_em, 1000);
    eq("ascent", f.ascent, 1160);
    eq("descent", f.descent, -288);
    eq("num_glyphs", f.num_glyphs, 7655);
    eq("gid('A')", ttf_glyph_id(&f, 0x41), 34);
    eq("gid('你')", ttf_glyph_id(&f, 0x4F60), 918);
    eq("adv('A')", ttf_advance(&f, ttf_glyph_id(&f, 0x41)), 608);
    eq("adv('你')", ttf_advance(&f, ttf_glyph_id(&f, 0x4F60)), 1000);
    eq("gid(missing)", ttf_glyph_id(&f, 0x1FBFF), 0);

    /* outlines */
    static uint8_t scratch[1 << 16];
    struct ttf_outline o;
    eq("outline('A') rc", ttf_glyph_outline(&f, ttf_glyph_id(&f,0x41), &o, scratch, sizeof scratch), 0);
    eq("A contours", o.ncontours, 2);
    eq("A bbox xmin", o.xmin, 4); eq("A bbox ymax", o.ymax, 733);
    if (o.npts <= 0) { printf("FAIL A npts=%d\n", o.npts); fail = 1; }
    for (int i = 0; i < o.npts; i++)
        if (o.x[i] < o.xmin - 1 || o.x[i] > o.xmax + 1 || o.y[i] < o.ymin - 1 || o.y[i] > o.ymax + 1)
            { printf("FAIL A pt %d (%d,%d) out of bbox\n", i, o.x[i], o.y[i]); fail = 1; break; }

    eq("outline('你') rc", ttf_glyph_outline(&f, ttf_glyph_id(&f,0x4F60), &o, scratch, sizeof scratch), 0);
    eq("你 contours", o.ncontours, 8);
    eq("你 bbox ymin", o.ymin, -81); eq("你 bbox xmax", o.xmax, 959);

    /* The same glyph as drawing commands, which is the form the rasterizer
     * consumes. Every contour must open with a move and close with a close. */
    static struct fp_cmd cmds[4096];
    struct fp_path p;
    fp_init(&p, cmds, (int)sizeof cmds);
    eq("path('你') rc", ttf_glyph_path(&f, ttf_glyph_id(&f,0x4F60), &p, scratch, sizeof scratch), 0);
    int moves = 0, closes = 0, curves = 0, bad = 0;
    for (int i = 0; i < p.n; i++) {
        if (p.cmd[i].op == FP_MOVE) { moves++; if (i && p.cmd[i-1].op != FP_CLOSE) bad = 1; }
        else if (p.cmd[i].op == FP_CLOSE) closes++;
        else if (p.cmd[i].op == FP_QUAD) curves++;
        else if (p.cmd[i].op == FP_CUBIC) bad = 1;   /* glyf is quadratic only */
    }
    eq("path contours", moves, 8);
    eq("path closes", closes, 8);
    if (!curves || bad) { printf("FAIL path shape: %d curves, bad=%d\n", curves, bad); fail = 1; }

    /* An OpenType/CFF face must be accepted now, not rejected as it once was. */
    if (argc > 2) {
        FILE *cp = fopen(argv[2], "rb");
        if (!cp) { printf("no %s\n", argv[2]); return 2; }
        fseek(cp, 0, SEEK_END); long cn = ftell(cp); fseek(cp, 0, SEEK_SET);
        uint8_t *cb = malloc(cn);
        if (fread(cb, 1, cn, cp) != (size_t)cn) return 2;
        fclose(cp);
        struct ttf_font cf;
        eq("CFF parse", ttf_parse(cb, (int)cn, &cf), 0);
        eq("CFF outline format", cf.outline_fmt, TTF_OUTLINE_CFF);
        fp_init(&p, cmds, (int)sizeof cmds);
        eq("CFF path rc", ttf_glyph_path(&cf, ttf_glyph_id(&cf, 'A'), &p, scratch, sizeof scratch), 0);
        int cub = 0;
        for (int i = 0; i < p.n; i++) if (p.cmd[i].op == FP_CUBIC) cub++;
        if (!cub) { printf("FAIL CFF 'A' produced no cubic segments\n"); fail = 1; }
        free(cb);
    }

    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
