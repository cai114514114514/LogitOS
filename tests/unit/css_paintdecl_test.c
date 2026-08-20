/* css_paintdecl_test.c -- the declarations LibCSS drops on the floor.
 *
 * `transform`, `transform-origin`, `box-shadow` and every gradient value are
 * absent from our vendored LibCSS's property table, so the cascade cannot
 * carry them at all and css_extra.c's raw-declaration scan is their only
 * producer. This gate is that scan plus the three parsers that turn what it
 * captured into values.
 *
 * TABLE DRIVEN, ONE VERDICT PER CASE. A declaration string goes in and a whole
 * expected result comes out, compared field by field -- one CHECK per row, so
 * "how many rows reddened" is a number a negative control can be measured
 * against rather than a page of output somebody has to read.
 *
 * THE REFUSALS ARE ROWS TOO, and they are half the point. A value we cannot
 * render has to come back REFUSED, not mis-parsed: `repeating-linear-gradient`
 * ignored is one band where a stripe pattern belongs, `-webkit-linear-gradient`
 * accepted under the modern angle rule is every one of them rotated ninety
 * degrees, and a colour hint read as a stop invents a colour. Each of those is
 * a row asserting 0.
 *
 * Colours travel as SPANS and are asserted BYTE FOR BYTE rather than as RGBA.
 * That is not a weaker assertion -- an exact span comparison catches an
 * off-by-one at either end, which an RGBA compare through a forgiving parser
 * would not -- and it is what keeps this gate off c/lib/image/svg.c and
 * c/lib/gfx. css.h explains why css_extra.c must not reach for them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "css.h"
#include "dom.h"
#include "css_interp.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int fail, ncheck;
#define CST(n) ((struct cstyle *)(n)->style)
#define CHECK(c, m) do { ncheck++; if (!(c)) { printf("FAIL: %s\n", (m)); fail = 1; } \
                         else printf("ok: %s\n", (m)); } while (0)

/* span == literal, exactly */
static int spaneq(const char *s, int n, const char *lit)
{
    return s && lit && n == (int)strlen(lit) && !memcmp(s, lit, (size_t)n);
}

static struct node *by_class(struct node *n, const char *cls)
{
    if (n->type == N_ELEM) {
        const char *c = dom_attr(n, "class");
        if (c && !strcmp(c, cls)) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = by_class(c, cls);
        if (r) return r;
    }
    return NULL;
}

/* ====================================================================== */
/* 1. linear-gradient                                                     */
/* ====================================================================== */

struct gcase {
    const char *name;
    const char *decl;
    int ok;                 /* expected css_gradient_parse return          */
    int dir, corner, angle; /* CG_DIR_*, CG_CORNER_*, millidegrees         */
    int nstop;
    const char *c0; int k0, p0;     /* first stop: colour span, kind, pos  */
    const char *cN; int kN, pN;     /* last stop                           */
};

static const struct gcase G[] = {
/* --- accepted ------------------------------------------------------------ */
{ "grad: implicit direction is `to bottom`",
  "linear-gradient(#fff, #000)", 1, CG_DIR_ANGLE, 0, 180000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

{ "grad: `to right` is 90 degrees",
  "linear-gradient(to right, red, blue)", 1, CG_DIR_ANGLE, 0, 90000, 2,
  "red", CG_POS_AUTO, 0, "blue", CG_POS_AUTO, 0 },

{ "grad: `to top` is 0, not 360",
  "linear-gradient(to top, red, blue)", 1, CG_DIR_ANGLE, 0, 0, 2,
  "red", CG_POS_AUTO, 0, "blue", CG_POS_AUTO, 0 },

{ "grad: `to left` is 270",
  "linear-gradient(to left,red,blue)", 1, CG_DIR_ANGLE, 0, 270000, 2,
  "red", CG_POS_AUTO, 0, "blue", CG_POS_AUTO, 0 },

/* A corner is NOT an angle: CSS Images 3 puts the line perpendicular to the
 * box's other diagonal, so it depends on w/h and is kept as a keyword. */
{ "grad: `to bottom right` stays a CORNER, not an angle",
  "linear-gradient(to bottom right, #123456, #654321)", 1,
  CG_DIR_CORNER, CG_CORNER_BR, 180000, 2,
  "#123456", CG_POS_AUTO, 0, "#654321", CG_POS_AUTO, 0 },

{ "grad: `to left top` normalises to the same corner as `to top left`",
  "linear-gradient(to left top, #000, #fff)", 1,
  CG_DIR_CORNER, CG_CORNER_TL, 180000, 2,
  "#000", CG_POS_AUTO, 0, "#fff", CG_POS_AUTO, 0 },

{ "grad: an explicit angle in deg",
  "linear-gradient(135deg, #fff, #000)", 1, CG_DIR_ANGLE, 0, 135000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

/* Fractional degrees are why the angle is MILLIdegrees: 45.5 rounded to 45
 * is a visible half-degree on a wide hero banner. */
{ "grad: a fractional angle survives (millidegrees, not degrees)",
  "linear-gradient(45.5deg, #fff, #000)", 1, CG_DIR_ANGLE, 0, 45500, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

{ "grad: 0.25turn is 90 degrees",
  "linear-gradient(0.25turn, #fff, #000)", 1, CG_DIR_ANGLE, 0, 90000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

{ "grad: 100grad is 90 degrees",
  "linear-gradient(100grad, #fff, #000)", 1, CG_DIR_ANGLE, 0, 90000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

/* 3.141 rad is 179.96604345 degrees -- taken from libm in the sweep below,
 * not from memory. The first version of this row said 179948 and was wrong;
 * the code was right. */
{ "grad: radians convert without libm",
  "linear-gradient(3.141rad, #fff, #000)", 1, CG_DIR_ANGLE, 0, 179966, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

{ "grad: a negative angle normalises into [0,360)",
  "linear-gradient(-90deg, #fff, #000)", 1, CG_DIR_ANGLE, 0, 270000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

/* THE PERCENTAGE CASES. Hundredths of a percent, for the reason cstyle's
 * pt0..pl0 are: 62.5% of a 400 px line is 250 and 62% of it is 248. */
{ "grad: a stop percentage is HUNDREDTHS of a percent",
  "linear-gradient(#fff 0%, #000 62.5%)", 1, CG_DIR_ANGLE, 0, 180000, 2,
  "#fff", CG_POS_PCT, 0, "#000", CG_POS_PCT, 6250 },

{ "grad: three stops, middle percentage",
  "linear-gradient(to right, red 10%, lime 50%, blue 90%)", 1,
  CG_DIR_ANGLE, 0, 90000, 3,
  "red", CG_POS_PCT, 1000, "blue", CG_POS_PCT, 9000 },

/* `red 10% 20%` is the two-position shorthand: one component, two stops. */
{ "grad: the two-position stop shorthand expands to two stops",
  "linear-gradient(red 10% 20%, blue)", 1, CG_DIR_ANGLE, 0, 180000, 3,
  "red", CG_POS_PCT, 1000, "blue", CG_POS_AUTO, 0 },

{ "grad: a px stop position stays px (5 of 690 in the corpus)",
  "linear-gradient(to right, #fff 0, #fff 100px, transparent 100px)", 1,
  CG_DIR_ANGLE, 0, 90000, 3,
  "#fff", CG_POS_PX, 0, "transparent", CG_POS_PX, 100 },

/* An em stop position needs the FONT SIZE, which is why the parser takes it
 * and the capture does not: at capture time there is no element. fs = 16. */
{ "grad: an em stop position resolves against the font size",
  "linear-gradient(#fff 1.5em, #000)", 1, CG_DIR_ANGLE, 0, 180000, 2,
  "#fff", CG_POS_PX, 24, "#000", CG_POS_AUTO, 0 },

/* rgba() holds top-level commas of its own. Splitting the stop list without
 * tracking paren depth turns this one declaration into five broken ones. */
{ "grad: rgba() commas do not split the stop list",
  "linear-gradient(to top, rgba(0, 0, 0, 0.6) 0%, rgba(0,0,0,0) 100%)", 1,
  CG_DIR_ANGLE, 0, 0, 2,
  "rgba(0, 0, 0, 0.6)", CG_POS_PCT, 0, "rgba(0,0,0,0)", CG_POS_PCT, 10000 },

{ "grad: the modern rgb(0 0 0 / 50%) spelling is one token",
  "linear-gradient(rgb(0 0 0 / 50%), #fff)", 1, CG_DIR_ANGLE, 0, 180000, 2,
  "rgb(0 0 0 / 50%)", CG_POS_AUTO, 0, "#fff", CG_POS_AUTO, 0 },

{ "grad: uppercase LINEAR-GRADIENT and TO RIGHT",
  "LINEAR-GRADIENT(TO RIGHT, #fff, #000)", 1, CG_DIR_ANGLE, 0, 90000, 2,
  "#fff", CG_POS_AUTO, 0, "#000", CG_POS_AUTO, 0 },

{ "grad: eight stops is the engine's cap and is accepted",
  "linear-gradient(#1, #2, #3, #4, #5, #6, #7, #8)", 1,
  CG_DIR_ANGLE, 0, 180000, 8,
  "#1", CG_POS_AUTO, 0, "#8", CG_POS_AUTO, 0 },

/* --- refused ------------------------------------------------------------- */
{ "grad REFUSED: repeating-linear-gradient (a different tiling rule)",
  "repeating-linear-gradient(45deg, #fff 0 10px, #000 10px 20px)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: -webkit-linear-gradient (the prefixed angle convention differs)",
  "-webkit-linear-gradient(top, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: radial-gradient (gfx_paint_radial exists, this parser does not emit it)",
  "radial-gradient(circle, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: conic-gradient (the engine has no conic paint)",
  "conic-gradient(from 0deg, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: an `in oklab` interpolation space",
  "linear-gradient(in oklab, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: a bare-percentage colour HINT would have to invent a colour",
  "linear-gradient(#fff, 40%, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: one stop is not a gradient (an unresolved var() looks like this)",
  "linear-gradient(var(--brand))", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: nine stops exceeds GFX_MAX_STOPS",
  "linear-gradient(#1,#2,#3,#4,#5,#6,#7,#8,#9)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

/* A unitless number is a valid <length> and never a valid <angle>, so this is
 * a parse error -- the same rule css_interp.c's ang_rad() states. Accepting it
 * would take a declaration the cascade drops. */
{ "grad REFUSED: an angle with no unit",
  "linear-gradient(45, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: `to` with no side",
  "linear-gradient(to, #fff, #000)", 0, 0,0,0,0, 0,0,0, 0,0,0 },

{ "grad REFUSED: unbalanced parens",
  "linear-gradient(#fff, #000", 0, 0,0,0,0, 0,0,0, 0,0,0 },
};

static void run_gradients(void)
{
    for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        const struct gcase *g = &G[i];
        struct cgradient cg;
        int r = css_gradient_parse(g->decl, (int)strlen(g->decl), 16, 16, &cg);
        int good = (r == g->ok);
        if (good && r) {
            good = cg.kind == CG_LINEAR && cg.dir == g->dir && cg.nstop == g->nstop &&
                   (g->dir == CG_DIR_CORNER ? cg.corner == g->corner
                                            : cg.angle_mdeg == g->angle) &&
                   spaneq(cg.stop[0].color, cg.stop[0].colorlen, g->c0) &&
                   cg.stop[0].pos_kind == g->k0 && cg.stop[0].pos == g->p0 &&
                   spaneq(cg.stop[cg.nstop-1].color, cg.stop[cg.nstop-1].colorlen, g->cN) &&
                   cg.stop[cg.nstop-1].pos_kind == g->kN &&
                   cg.stop[cg.nstop-1].pos == g->pN;
        }
        if (!good) {
            printf("  [%s]\n    got ok=%d dir=%d corner=%d angle=%d nstop=%d",
                   g->decl, r, cg.dir, cg.corner, cg.angle_mdeg, cg.nstop);
            if (r && cg.nstop > 0)
                printf(" first=(%.*s,k%d,%d) last=(%.*s,k%d,%d)",
                       cg.stop[0].colorlen, cg.stop[0].color, cg.stop[0].pos_kind,
                       cg.stop[0].pos,
                       cg.stop[cg.nstop-1].colorlen, cg.stop[cg.nstop-1].color,
                       cg.stop[cg.nstop-1].pos_kind, cg.stop[cg.nstop-1].pos);
            printf("\n    want ok=%d dir=%d corner=%d angle=%d nstop=%d\n",
                   g->ok, g->dir, g->corner, g->angle, g->nstop);
        }
        CHECK(good, g->name);
    }
}

/* ====================================================================== */
/* 2. box-shadow                                                          */
/* ====================================================================== */

struct scase {
    const char *name;
    const char *decl;
    int n;                                  /* shadows written              */
    int dx, dy, blur, spread, inset;        /* the FIRST one                */
    const char *col;                        /* NULL = no colour token       */
};

static const struct scase S[] = {
{ "shadow: two lengths, no blur, no colour",
  "2px 4px", 1, 2, 4, 0, 0, 0, NULL },

{ "shadow: the ordinary three-length form",
  "0 1px 3px rgba(0,0,0,.12)", 1, 0, 1, 3, 0, 0, "rgba(0,0,0,.12)" },

{ "shadow: four lengths -- the fourth is SPREAD, not a second blur",
  "0 4px 8px 2px #000", 1, 0, 4, 8, 2, 0, "#000" },

{ "shadow: inset",
  "inset 0 2px 4px #0003", 1, 0, 2, 4, 0, 1, "#0003" },

{ "shadow: inset written last",
  "0 2px 4px #0003 inset", 1, 0, 2, 4, 0, 1, "#0003" },

{ "shadow: the colour may come first",
  "red 1px 2px 3px", 1, 1, 2, 3, 0, 0, "red" },

{ "shadow: negative offsets",
  "-3px -6px 9px black", 1, -3, -6, 9, 0, 0, "black" },

/* em needs the font size, which is the whole reason the parser takes it and
 * the capture does not. fs = 16, so .5em is 8 px and 1em is 16. */
{ "shadow: em resolves against the font size",
  "0 .5em 1em rgba(0,0,0,.2)", 1, 0, 8, 16, 0, 0, "rgba(0,0,0,.2)" },

{ "shadow: rem resolves against the ROOT font size",
  "0 0 2rem #123", 1, 0, 0, 32, 0, 0, "#123" },

/* Half a pixel rounds AWAY from zero rather than truncating: truncation puts
 * every converted length one step low, always in the same direction. */
{ "shadow: 1.5px rounds to 2, not 1",
  "0 1.5px 0 #000", 1, 0, 2, 0, 0, 0, "#000" },

{ "shadow: a comma list, and the FIRST listed shadow comes back first",
  "0 1px 2px #111, 0 8px 16px #222", 2, 0, 1, 2, 0, 0, "#111" },

{ "shadow: !important is stripped",
  "0 2px 4px #000 !important", 1, 0, 2, 4, 0, 0, "#000" },

{ "shadow REFUSED: none",  "none", 0, 0,0,0,0,0, NULL },
{ "shadow REFUSED: one length is not a shadow", "4px", 0, 0,0,0,0,0, NULL },
{ "shadow REFUSED: five lengths", "1px 2px 3px 4px 5px #000", 0, 0,0,0,0,0, NULL },
/* A negative blur is a parse error in CSS, not a zero: clamping it would paint
 * a hard-edged shadow for a declaration a real browser drops entirely. */
{ "shadow REFUSED: a negative blur", "0 0 -4px #000", 0, 0,0,0,0,0, NULL },
{ "shadow REFUSED: two colours", "red blue 1px 2px", 0, 0,0,0,0,0, NULL },
/* A percentage is not a <length> here. */
{ "shadow REFUSED: a percentage offset", "0 50% 4px #000", 0, 0,0,0,0,0, NULL },
};

/* THE RADIAN CONVERSION, AGAINST AN INDEPENDENT ORACLE.
 *
 * css_extra.c may not call libm (see tests/cssdecl.mk on the fourteen link
 * lines), so `rad` goes through an integer 180/pi. A single hand-written
 * expectation would only pin the value somebody already believed -- which is
 * how the row above came to be wrong by 18 millidegrees on its first draft --
 * so the whole legal range is swept against libm's own answer and the WORST
 * error is the number reported. Integer only vs double, and the bar is that
 * they agree to within one millidegree, which is 1/1000 of a degree: on a
 * 2000 px gradient line that is a displacement of 0.03 px.
 *
 * The sweep writes three decimals because that is what the scanner keeps;
 * feeding it more would measure the documented truncation instead. */
static void run_radian_sweep(void)
{
    int worst = 0; double worst_at = 0;
    for (int k = 1; k <= 6283; k++) {
        char decl[96];
        double r = k / 1000.0;
        snprintf(decl, sizeof decl, "linear-gradient(%.3frad, #fff, #000)", r);
        struct cgradient cg;
        if (!css_gradient_parse(decl, (int)strlen(decl), 16, 16, &cg)) {
            printf("  %.3frad refused\n", r);
            CHECK(0, "radian sweep: every angle in range parses");
            return;
        }
        long want = lround(r * 180.0 / 3.14159265358979323846 * 1000.0);
        want %= 360000; if (want < 0) want += 360000;
        int d = (int)(cg.angle_mdeg - want);
        if (d < 0) d = -d;
        if (d > worst) { worst = d; worst_at = r; }
    }
    printf("  radian sweep: 6283 angles, worst error %d millidegrees (at %.3frad)\n",
           worst, worst_at);
    CHECK(worst <= 1, "radian sweep: integer rad->mdeg agrees with libm to 1 millidegree");
}

static void run_shadows(void)
{
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) {
        const struct scase *s = &S[i];
        struct cshadow sh[CS_MAXSHADOW];
        memset(sh, 0, sizeof sh);
        int n = css_shadow_parse(s->decl, (int)strlen(s->decl), 16, 16, sh, CS_MAXSHADOW);
        int good = (n == s->n);
        if (good && n > 0)
            good = sh[0].dx == s->dx && sh[0].dy == s->dy && sh[0].blur == s->blur &&
                   sh[0].spread == s->spread && sh[0].inset == s->inset &&
                   (s->col ? spaneq(sh[0].color, sh[0].colorlen, s->col) : sh[0].color == NULL);
        if (!good) {
            printf("  [%s]\n    got n=%d", s->decl, n);
            if (n > 0) printf(" dx=%d dy=%d blur=%d spread=%d inset=%d col=%.*s",
                              sh[0].dx, sh[0].dy, sh[0].blur, sh[0].spread, sh[0].inset,
                              sh[0].colorlen, sh[0].color ? sh[0].color : "");
            printf("\n    want n=%d dx=%d dy=%d blur=%d spread=%d inset=%d col=%s\n",
                   s->n, s->dx, s->dy, s->blur, s->spread, s->inset, s->col ? s->col : "(none)");
        }
        CHECK(good, s->name);
    }
}

/* ====================================================================== */
/* 3. transform-origin                                                    */
/* ====================================================================== */

struct ocase { const char *name, *decl; int ok, x, xp, y, yp; };

static const struct ocase O[] = {
{ "origin: absent is the CSS initial 50% 50%, not (0,0)", NULL, 1, 5000, 1, 5000, 1 },
{ "origin: `0 0`",            "0 0",        1, 0, 0, 0, 0 },
{ "origin: `0 100%`",         "0 100%",     1, 0, 0, 10000, 1 },
{ "origin: `center`",         "center",     1, 5000, 1, 5000, 1 },
{ "origin: `top left` means `left top`", "top left", 1, 0, 1, 0, 1 },
{ "origin: `left top`",       "left top",   1, 0, 1, 0, 1 },
{ "origin: `top right`",      "top right",  1, 10000, 1, 0, 1 },
{ "origin: `50%` alone centres both axes", "50%", 1, 5000, 1, 5000, 1 },
/* A lone vertical keyword sets Y and leaves X centred. Reading it into X puts
 * the origin on the LEFT EDGE of every element that writes it. */
{ "origin: a lone `top` is the VERTICAL axis", "top", 1, 5000, 1, 0, 1 },
{ "origin: a lone `bottom` likewise", "bottom", 1, 5000, 1, 10000, 1 },
{ "origin: `10px 20px`",      "10px 20px",  1, 10, 0, 20, 0 },
{ "origin: `1em 2em` at fs 16", "1em 2em",  1, 16, 0, 32, 0 },
{ "origin: a third component is the Z origin and is discarded",
  "left top 40px", 1, 0, 1, 0, 1 },
{ "origin REFUSED: four components", "0 0 0 0", 0, 0,0,0,0 },
{ "origin REFUSED: `left right`",    "left right", 0, 0,0,0,0 },
{ "origin REFUSED: a bare number",   "10 20", 0, 0,0,0,0 },
};

static void run_origins(void)
{
    for (unsigned i = 0; i < sizeof O / sizeof O[0]; i++) {
        const struct ocase *o = &O[i];
        struct corigin co;
        int r = css_origin_parse(o->decl, o->decl ? (int)strlen(o->decl) : 0, 16, 16, &co);
        int good = (r == o->ok);
        if (good && r) good = co.x == o->x && co.x_pct == o->xp &&
                              co.y == o->y && co.y_pct == o->yp;
        if (!good)
            printf("  [%s]\n    got ok=%d x=%d/%d y=%d/%d  want ok=%d x=%d/%d y=%d/%d\n",
                   o->decl ? o->decl : "(absent)", r, co.x, co.x_pct, co.y, co.y_pct,
                   o->ok, o->x, o->xp, o->y, o->yp);
        CHECK(good, o->name);
    }
}

/* ====================================================================== */
/* 4. the CAPTURE: a real sheet, through css_apply + css_extra_apply       */
/* ====================================================================== */

static void run_capture(void)
{
    const char *html =
        "<body>"
        "<div class='t'>a</div>"
        "<div class='wk'>b</div>"
        "<div class='sh'>c</div>"
        "<div class='g1'>d</div>"
        "<div class='g2'>e</div>"
        "<div class='multi'>f</div>"
        "<div class='none'>g</div>"
        "<div class='split'>h</div>"
        "<div class='moz'>j</div>"
        "<div class='both'>k</div>"
        "<b class='inl' style='transform:rotate(3deg);box-shadow:0 1px 0 #eee'>i</b>"
        "</body>";
    const char *css =
        ".t{transform:translate(-50%,-50%);transform-origin:0 100%}"
        /* only the prefixed spelling: the fallback's whole reason to exist */
        ".wk{-webkit-transform:scale(2)}"
        ".sh{box-shadow:0 4px 12px rgba(0,0,0,.15) !important}"
        ".g1{background-image:linear-gradient(90deg,#fff,#000)}"
        /* a third of the corpus's gradients are in the SHORTHAND */
        ".g2{background:#fff linear-gradient(to right,red,blue) no-repeat}"
        /* two layers composite; painting only the first is a guess */
        ".multi{background-image:linear-gradient(#fff,#000),url(x.png)}"
        ".none{transform:none}"
        /* two rules, two properties, one element: both must survive */
        ".split{transform:rotate(45deg)}"
        ".split{box-shadow:0 0 2px #000}"
        /* -moz- and -o- are each prefix-only in MORE corpus blocks than -ms-
         * is (7 and 6 against 2), which is why all four spellings are in the
         * table and why this row exists. */
        ".moz{-moz-transform:skewX(4deg);-moz-box-shadow:1px 2px 3px #123}"
        /* Both spellings in one block, PREFIX LAST -- the case that killed the
         * source-order argument. Two fifths of the corpus's box-shadow blocks
         * are written this way, and the unprefixed value must still win,
         * because they are different properties and a real browser never
         * applies the prefixed one. */
        ".both{box-shadow:0 1px 2px #abc;-webkit-box-shadow:9px 9px 9px #f00}";

    struct node *root = dom_parse(html, (int)strlen(html));
    CHECK(root != NULL, "capture: dom_parse");
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));

    struct node *t = by_class(root, "t"), *wk = by_class(root, "wk");
    struct node *sh = by_class(root, "sh"), *g1 = by_class(root, "g1");
    struct node *g2 = by_class(root, "g2"), *mu = by_class(root, "multi");
    struct node *no = by_class(root, "none"), *sp = by_class(root, "split");
    struct node *inl = by_class(root, "inl");
    struct node *mz = by_class(root, "moz"), *bo = by_class(root, "both");

    CHECK(t && CST(t) && spaneq(CST(t)->xraw[XR_TRANSFORM],
                                CST(t)->xrawlen[XR_TRANSFORM], "translate(-50%,-50%)"),
          "capture: transform reaches cstyle.xraw");
    CHECK(t && CST(t) && spaneq(CST(t)->xraw[XR_TRANSFORM_ORIGIN],
                                CST(t)->xrawlen[XR_TRANSFORM_ORIGIN], "0 100%"),
          "capture: transform-origin reaches cstyle.xraw");
    CHECK(wk && CST(wk) && spaneq(CST(wk)->xraw[XR_TRANSFORM],
                                  CST(wk)->xrawlen[XR_TRANSFORM], "scale(2)"),
          "capture: -webkit-transform is taken when the unprefixed one is absent");
    /* !important is stripped at capture, so the parsers never see it. */
    CHECK(sh && CST(sh) && spaneq(CST(sh)->xraw[XR_BOX_SHADOW],
                                  CST(sh)->xrawlen[XR_BOX_SHADOW],
                                  "0 4px 12px rgba(0,0,0,.15)"),
          "capture: box-shadow reaches cstyle.xraw with !important stripped");
    CHECK(g1 && CST(g1) && spaneq(CST(g1)->xraw[XR_BG_IMAGE],
                                  CST(g1)->xrawlen[XR_BG_IMAGE],
                                  "linear-gradient(90deg,#fff,#000)"),
          "capture: background-image gradient reaches cstyle.xraw");
    /* The gradient is lifted OUT of the shorthand, so the parser gets a value
     * and not a background layer. */
    CHECK(g2 && CST(g2) && spaneq(CST(g2)->xraw[XR_BG_IMAGE],
                                  CST(g2)->xrawlen[XR_BG_IMAGE],
                                  "linear-gradient(to right,red,blue)"),
          "capture: the `background` SHORTHAND's gradient is found too");
    CHECK(mu && CST(mu) && CST(mu)->xraw[XR_BG_IMAGE] == NULL,
          "capture REFUSED: more than one background layer");
    CHECK(mz && CST(mz) && spaneq(CST(mz)->xraw[XR_TRANSFORM],
                                  CST(mz)->xrawlen[XR_TRANSFORM], "skewX(4deg)") &&
          spaneq(CST(mz)->xraw[XR_BOX_SHADOW], CST(mz)->xrawlen[XR_BOX_SHADOW],
                 "1px 2px 3px #123"),
          "capture: -moz- transform and box-shadow are taken when alone");
    /* THE ONE THAT DECIDES THE RULE. If the fallback were source-order, the
     * span here would be the -webkit- value, which is deliberately a
     * different, obviously wrong shadow so the failure names itself. */
    CHECK(bo && CST(bo) && spaneq(CST(bo)->xraw[XR_BOX_SHADOW],
                                  CST(bo)->xrawlen[XR_BOX_SHADOW], "0 1px 2px #abc"),
          "capture: unprefixed wins even when the PREFIXED spelling is last");
    CHECK(no && CST(no) && spaneq(CST(no)->xraw[XR_TRANSFORM],
                                  CST(no)->xrawlen[XR_TRANSFORM], "none"),
          "capture: `transform:none` is captured, not dropped");
    CHECK(sp && CST(sp) && CST(sp)->xraw[XR_TRANSFORM] && CST(sp)->xraw[XR_BOX_SHADOW],
          "capture: two rules setting different properties both survive (per-property merge)");
    CHECK(inl && CST(inl) && spaneq(CST(inl)->xraw[XR_TRANSFORM],
                                    CST(inl)->xrawlen[XR_TRANSFORM], "rotate(3deg)") &&
          spaneq(CST(inl)->xraw[XR_BOX_SHADOW], CST(inl)->xrawlen[XR_BOX_SHADOW],
                 "0 1px 0 #eee"),
          "capture: an inline style= attribute is captured too");
    /* The control for all of the above: an element nobody styled must come
     * back with NOTHING, or these assertions are measuring a memset. */
    CHECK(mu && CST(mu) && CST(mu)->xraw[XR_TRANSFORM] == NULL &&
          CST(mu)->xraw[XR_BOX_SHADOW] == NULL,
          "capture control: an element with no such declaration has NULL spans");
}

/* ====================================================================== */
/* 5. THE WIRING: a captured span -> ci_transform_parse -> a matrix        */
/*                                                                        */
/* This is the half the task called wiring rather than implementation: the */
/* parser at css_interp.c has existed all along and CSS.supports() was its */
/* only caller in the tree. The assertion that matters is the percentage   */
/* one -- translate(-50%,-50%) centring is reached by 14 of 15 real pages, */
/* and a percentage translate has NO value until a box is named, which is  */
/* why it resolves here and not at capture time.                           */
/* ====================================================================== */

static void run_matrix(void)
{
    const char *html = "<body><div class='c'>x</div><div class='r'>y</div></body>";
    const char *css  = ".c{transform:translate(-50%,-50%)}"
                       ".r{transform:translateY(2em) scale(2)}";
    struct node *root = dom_parse(html, (int)strlen(html));
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));

    struct node *c = by_class(root, "c");
    struct cstyle *st = c ? CST(c) : NULL;
    int okc = 0;
    if (st && st->xraw[XR_TRANSFORM]) {
        struct ci_xform xf;
        if (ci_transform_parse(st->xraw[XR_TRANSFORM], st->xrawlen[XR_TRANSFORM],
                               st->font_px, st->font_px, &xf) == 0) {
            double m[16];
            /* A 200x100 border box: -50% of each axis is -100 and -50. */
            ci_transform_matrix(&xf, 200, 100, m);
            okc = (m[12] == -100.0 && m[13] == -50.0);
            if (!okc) printf("  translate(-50%%,-50%%) on 200x100 -> (%g,%g), want (-100,-50)\n",
                             m[12], m[13]);
            /* The same list against a DIFFERENT box must give a different
             * answer, or the percentage is not being resolved at all -- a
             * captured-and-ignored percentage would pass the line above if it
             * happened to be stored as pixels. */
            ci_transform_matrix(&xf, 400, 300, m);
            if (okc && !(m[12] == -200.0 && m[13] == -150.0)) {
                printf("  the same list on 400x300 -> (%g,%g), want (-200,-150)\n",
                       m[12], m[13]);
                okc = 0;
            }
        }
    }
    CHECK(okc, "wiring: translate(-50%,-50%) resolves against the box, not a constant");

    struct node *r = by_class(root, "r");
    struct cstyle *rs = r ? CST(r) : NULL;
    int okr = 0;
    if (rs && rs->xraw[XR_TRANSFORM]) {
        struct ci_xform xf;
        if (ci_transform_parse(rs->xraw[XR_TRANSFORM], rs->xrawlen[XR_TRANSFORM],
                               rs->font_px, rs->font_px, &xf) == 0) {
            double m[16];
            ci_transform_matrix(&xf, 100, 100, m);
            /* translateY(2em) at the element's own font size, then scale(2):
             * the list composes left to right, so the scale is inside. */
            okr = (m[0] == 2.0 && m[5] == 2.0 && m[13] == 2.0 * rs->font_px);
            if (!okr) printf("  translateY(2em) scale(2) fs=%d -> sx=%g sy=%g ty=%g\n",
                             rs->font_px, m[0], m[5], m[13]);
        }
    }
    CHECK(okr, "wiring: a two-function list composes, and em uses the element's font size");
}

/* ====================================================================== */
/* 6. rem: the other half of the two length arguments                     */
/*                                                                        */
/* All three parsers take `fs_px` AND `root_px`, and until css_root_px()   */
/* existed only the first was answerable outside css_engine.c -- g_root_px */
/* is static there and the value is a property of the DOCUMENT, not of any */
/* cstyle. A caller with no route to it passes 16, which is right until a  */
/* page writes `html{font-size:62.5%}` and then wrong by 1.6x everywhere   */
/* at once. So the element's font size and the root's are DELIBERATELY     */
/* different here (32 and 10): a build that confused the two, or that fell */
/* back to 16, gets a different number for every field below.              */
/* ====================================================================== */

static void run_rootpx(void)
{
    const char *html = "<body><div class='rem'>x</div></body>";
    const char *css  = "html{font-size:10px}"
                       ".rem{font-size:32px;box-shadow:0 1rem 2em 0.5rem #000}";
    struct node *root = dom_parse(html, (int)strlen(html));
    css_apply(root, css, (int)strlen(css));
    css_extra_apply(root, css, (int)strlen(css));

    /* Does not touch the capture, so it keeps passing under NO_XCAPTURE --
     * this row measures css_engine.c, not css_extra.c's scan. */
    CHECK(css_root_px() == 10,
          "rem: css_root_px() reports the document root's font-size, not 16");

    struct node *e = by_class(root, "rem");
    struct cstyle *st = e ? CST(e) : NULL;
    struct cshadow sh[2];
    int n = 0, ok = 0;
    if (st && st->xraw[XR_BOX_SHADOW]) {
        n = css_shadow_parse(st->xraw[XR_BOX_SHADOW], st->xrawlen[XR_BOX_SHADOW],
                             st->font_px, css_root_px(), sh, 2);
        /* dy 1rem = 10 (NOT 32, and not 16); blur 2em = 64 at the element's
         * own 32px; spread 0.5rem = 5, which also proves the fractional path
         * rounds rather than truncating to 4. */
        ok = (n == 1 && sh[0].dx == 0 && sh[0].dy == 10 &&
              sh[0].blur == 64 && sh[0].spread == 5);
        if (!ok)
            printf("  n=%d dx=%d dy=%d blur=%d spread=%d (fs=%d root=%d)\n",
                   n, sh[0].dx, sh[0].dy, sh[0].blur, sh[0].spread,
                   st->font_px, css_root_px());
    }
    CHECK(ok, "rem: a captured shadow resolves rem against the ROOT and em against the element");
}

int main(void)
{
    css_init();
    css_viewport(1180, 620);
    run_gradients();
    run_radian_sweep();
    run_shadows();
    run_origins();
    run_capture();
    run_matrix();
    run_rootpx();
    printf("checks: %d\n", ncheck);
    if (fail) { printf("FAILURES\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
