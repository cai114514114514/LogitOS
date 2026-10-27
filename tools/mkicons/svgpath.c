/* svgpath.c -- a small SVG path "d" parser for tools/mkicons (unity-included
 * by mkicons.c, so the tool is one translation unit plus c/lib/gfx).
 *
 * Why it exists: the app icons (design_language.md 3.7.4) are art, and art is
 * iterated by editing path strings in glyphs_svg.inc, not by recompiling
 * hand-written geometry. Supported: M L H V C Q Z, absolute (upper case) and
 * relative (lower case), implicit command repetition ("M0 0 10 10" is a move
 * then a line), numbers with sign, fraction and exponent, comma or space
 * separators. Not supported (and refused loudly, never skipped): S T A and
 * anything else. Coordinates are floating units in the glyph's box; an affine
 * transform maps them to device 24.8 fixed point as each point is emitted, so
 * a Bezier stays a Bezier under rotation and scale.
 *
 * Host tool: floating point is fine here. Compile with -ffp-contract=off so
 * the same source gives the same bytes on every host. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"

struct svg_xf { double a, b, c, d, e, f; };   /* x' = a*x + c*y + e ; y' = b*x + d*y + f */

static int svg_fix(double v) { return (int)floor(v * 256.0 + 0.5); }
#define XFX(xf, x, y) svg_fix((xf)->a * (x) + (xf)->c * (y) + (xf)->e)
#define XFY(xf, x, y) svg_fix((xf)->b * (x) + (xf)->d * (y) + (xf)->f)

static void svg_skip(const char **s)
{
    while (**s && (isspace((unsigned char)**s) || **s == ',')) (*s)++;
}
static int svg_number(const char **s, double *out)
{
    svg_skip(s);
    const char *p = *s;
    char *end;
    if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) return 0;
    *out = strtod(p, &end);
    if (end == p) return 0;
    *s = end;
    return 1;
}

/* Appends the subpaths of `d` to `p`, mapped through `xf`. Returns 0 on
 * success and -1 on a syntax error (a message goes to stderr). */
static int svg_path_build(struct gfx_path *p, const char *d, const struct svg_xf *xf)
{
    const char *s = d;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    int have_move = 0;
    for (;;) {
        svg_skip(&s);
        if (!*s) break;
        if (isalpha((unsigned char)*s)) { cmd = *s++; }
        else if (!cmd) { fprintf(stderr, "svgpath: number before any command at \"%.12s\"\n", s); return -1; }
        else if (cmd == 'M') cmd = 'L';           /* implicit repetition */
        else if (cmd == 'm') cmd = 'l';
        else if (cmd == 'Z' || cmd == 'z') { fprintf(stderr, "svgpath: number after Z at \"%.12s\"\n", s); return -1; }
        int rel = islower((unsigned char)cmd);
        double v[6];
        int n = 0;
        switch (cmd) {
        case 'M': case 'm': case 'L': case 'l': n = 2; break;
        case 'H': case 'h': case 'V': case 'v': n = 1; break;
        case 'C': case 'c': n = 6; break;
        case 'Q': case 'q': n = 4; break;
        case 'Z': case 'z': n = 0; break;
        default: fprintf(stderr, "svgpath: unsupported command '%c'\n", cmd); return -1;
        }
        for (int i = 0; i < n; i++)
            if (!svg_number(&s, &v[i])) { fprintf(stderr, "svgpath: missing number for '%c' at \"%.12s\"\n", cmd, s); return -1; }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (cmd) {
        case 'M': case 'm':
            cx = ox + v[0]; cy = oy + v[1]; sx = cx; sy = cy; have_move = 1;
            gfx_move_to(p, XFX(xf, cx, cy), XFY(xf, cx, cy));
            break;
        case 'L': case 'l': case 'H': case 'h': case 'V': case 'v':
            if (!have_move) { fprintf(stderr, "svgpath: '%c' before M\n", cmd); return -1; }
            if (n == 2) { cx = ox + v[0]; cy = oy + v[1]; }
            else if (cmd == 'H' || cmd == 'h') cx = ox + v[0];
            else cy = oy + v[0];
            gfx_line_to(p, XFX(xf, cx, cy), XFY(xf, cx, cy));
            break;
        case 'Q': case 'q': {
            if (!have_move) { fprintf(stderr, "svgpath: 'Q' before M\n"); return -1; }
            double x1 = ox + v[0], y1 = oy + v[1], x = ox + v[2], y = oy + v[3];
            gfx_quad_to(p, XFX(xf, x1, y1), XFY(xf, x1, y1), XFX(xf, x, y), XFY(xf, x, y));
            cx = x; cy = y;
            break; }
        case 'C': case 'c': {
            if (!have_move) { fprintf(stderr, "svgpath: 'C' before M\n"); return -1; }
            double x1 = ox + v[0], y1 = oy + v[1], x2 = ox + v[2], y2 = oy + v[3], x = ox + v[4], y = oy + v[5];
            gfx_cubic_to(p, XFX(xf, x1, y1), XFY(xf, x1, y1), XFX(xf, x2, y2), XFY(xf, x2, y2),
                         XFX(xf, x, y), XFY(xf, x, y));
            cx = x; cy = y;
            break; }
        case 'Z': case 'z':
            gfx_close(p);
            cx = sx; cy = sy;
            break;
        }
    }
    if (p->overflow) { fprintf(stderr, "svgpath: path storage overflowed\n"); return -1; }
    return 0;
}
