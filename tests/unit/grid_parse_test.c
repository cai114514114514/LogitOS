/* CSS Grid: the value parsers, and the auto-fill/auto-fit repetition count.
 *
 * Every expectation here is read off the spec text, not off this engine's
 * behaviour:
 *   css-grid-1 s7.2   <track-list>, <track-size>, minmax(), fit-content()
 *   css-grid-1 s7.2.3 repeat(), and the merge rule for line names inside it
 *   css-grid-1 s7.2.3.1 auto-fill / auto-fit repetition count
 *   css-grid-1 s7.3   grid-template-areas tokenisation and validity
 *   css-grid-1 s8.3   <grid-line>
 *
 * Prints one line per failure and exits non-zero if there were any. It must
 * never print FAIL and exit 0.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "layout_grid.h"

static int checks, fails;

#define CHK(cond, ...) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void chk_i(const char *what, long got, long want)
{
    checks++;
    if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* ------------------------------------------------------------ track lists -- */

static void t_tracklist(void)
{
    struct gtracklist t;

    /* Two fixed tracks: both sizing functions are the specified breadth. */
    CHK(grid_parse_tracklist("100px 200px", -1, 16, &t) == 0, "parse 100px 200px");
    chk_i("100px 200px: n", t.n, 2);
    chk_i("track0 min kind", t.tr[0].mn.kind, GSF_PX);
    chk_i("track0 min v",    t.tr[0].mn.v, 100);
    chk_i("track0 max v",    t.tr[0].mx.v, 100);
    chk_i("track1 max v",    t.tr[1].mx.v, 200);
    grid_tracklist_free(&t);

    /* s11.2: a <flex>-sized track's MIN track sizing function is `auto`. */
    CHK(grid_parse_tracklist("1fr 2fr", -1, 16, &t) == 0, "parse 1fr 2fr");
    chk_i("1fr: min kind is auto", t.tr[0].mn.kind, GSF_AUTO);
    chk_i("1fr: max kind is fr",   t.tr[0].mx.kind, GSF_FR);
    chk_i("1fr: max v (milli-fr)", t.tr[0].mx.v, 1000);
    chk_i("2fr: max v (milli-fr)", t.tr[1].mx.v, 2000);
    grid_tracklist_free(&t);

    /* Sub-1fr factors must survive parsing -- the flex-factor-sum < 1 rule
     * depends on them being representable. */
    CHK(grid_parse_tracklist("0.5fr .25fr", -1, 16, &t) == 0, "parse fractional fr");
    chk_i("0.5fr", t.tr[0].mx.v, 500);
    chk_i(".25fr", t.tr[1].mx.v, 250);
    grid_tracklist_free(&t);

    /* minmax(): first argument is the min fn, second the max fn. */
    CHK(grid_parse_tracklist("minmax(100px, 1fr)", -1, 16, &t) == 0, "parse minmax");
    chk_i("minmax min kind", t.tr[0].mn.kind, GSF_PX);
    chk_i("minmax min v",    t.tr[0].mn.v, 100);
    chk_i("minmax max kind", t.tr[0].mx.kind, GSF_FR);
    grid_tracklist_free(&t);

    /* <flex> is not an <inflexible-breadth>, so it cannot be a min fn. */
    CHK(grid_parse_tracklist("minmax(1fr, 200px)", -1, 16, &t) == 0, "parse minmax fr min");
    chk_i("minmax(1fr,..) min degrades to auto", t.tr[0].mn.kind, GSF_AUTO);
    chk_i("minmax(1fr,..) max",  t.tr[0].mx.v, 200);
    grid_tracklist_free(&t);

    /* s11.2: fit-content()'s min fn is auto and its max fn is treated as
     * max-content, with the argument kept as the clamp. */
    CHK(grid_parse_tracklist("fit-content(50px)", -1, 16, &t) == 0, "parse fit-content");
    chk_i("fit-content flagged", t.tr[0].is_fc, 1);
    chk_i("fit-content min kind", t.tr[0].mn.kind, GSF_AUTO);
    chk_i("fit-content max kind", t.tr[0].mx.kind, GSF_MAX_CONTENT);
    chk_i("fit-content arg",      t.tr[0].fc.v, 50);
    grid_tracklist_free(&t);

    CHK(grid_parse_tracklist("auto min-content max-content", -1, 16, &t) == 0, "parse keywords");
    chk_i("auto",        t.tr[0].mn.kind, GSF_AUTO);
    chk_i("min-content", t.tr[1].mn.kind, GSF_MIN_CONTENT);
    chk_i("max-content", t.tr[2].mx.kind, GSF_MAX_CONTENT);
    grid_tracklist_free(&t);

    /* Percentages are kept in hundredths of a percent so 33.33% survives. */
    CHK(grid_parse_tracklist("50% 33.33%", -1, 16, &t) == 0, "parse percentages");
    chk_i("50%",    t.tr[0].mn.v, 5000);
    chk_i("33.33%", t.tr[1].mn.v, 3333);
    grid_tracklist_free(&t);

    /* em resolves against the element's font size. */
    CHK(grid_parse_tracklist("2em", -1, 16, &t) == 0, "parse em");
    chk_i("2em at font-size 16", t.tr[0].mn.v, 32);
    grid_tracklist_free(&t);

    CHK(grid_parse_tracklist("none", -1, 16, &t) == 0, "parse none");
    chk_i("none: no tracks", t.n, 0);
    grid_tracklist_free(&t);

    /* An invalid declaration must not half-apply. */
    CHK(grid_parse_tracklist("minmax(100px)", -1, 16, &t) < 0, "minmax with one arg is invalid");
    chk_i("failed parse leaves no tracks", t.n, 0);
    grid_tracklist_free(&t);
}

static void t_repeat(void)
{
    struct gtracklist t;
    int i;

    CHK(grid_parse_tracklist("repeat(3, 100px)", -1, 16, &t) == 0, "parse repeat(3,100px)");
    chk_i("repeat(3): n", t.n, 3);
    for (i = 0; i < t.n; i++) chk_i("repeat(3): each 100px", t.tr[i].mn.v, 100);
    grid_tracklist_free(&t);

    CHK(grid_parse_tracklist("repeat(2, 10px 20px)", -1, 16, &t) == 0, "parse repeat multi");
    chk_i("repeat(2, a b): n", t.n, 4);
    chk_i("repeat(2, a b)[2]", t.tr[2].mn.v, 10);
    chk_i("repeat(2, a b)[3]", t.tr[3].mn.v, 20);
    grid_tracklist_free(&t);

    /* s7.2.3: "repeat(2, [a] 1fr [b]) is equivalent to [a] 1fr [b a] 1fr [b]".
     * So `a` names lines 0 and 1 and `b` names lines 1 and 2. */
    CHK(grid_parse_tracklist("repeat(2, [a] 1fr [b])", -1, 16, &t) == 0, "parse repeat w/ names");
    chk_i("repeat names: n tracks", t.n, 2);
    {
        int a0 = 0, a1 = 0, b1 = 0, b2 = 0;
        for (i = 0; i < t.nn; i++) {
            if (!strcmp(t.nm[i].n, "a") && t.nm[i].line == 0) a0 = 1;
            if (!strcmp(t.nm[i].n, "a") && t.nm[i].line == 1) a1 = 1;
            if (!strcmp(t.nm[i].n, "b") && t.nm[i].line == 1) b1 = 1;
            if (!strcmp(t.nm[i].n, "b") && t.nm[i].line == 2) b2 = 1;
        }
        CHK(a0 && a1 && b1 && b2, "repeat(2,[a] 1fr [b]) == [a] 1fr [b a] 1fr [b] "
                                  "(a@0=%d a@1=%d b@1=%d b@2=%d)", a0, a1, b1, b2);
    }
    grid_tracklist_free(&t);

    CHK(grid_parse_tracklist("[s] 100px [m] 200px [e]", -1, 16, &t) == 0, "parse line names");
    chk_i("line names: count", t.nn, 3);
    chk_i("[s] on line 0", t.nm[0].line, 0);
    chk_i("[m] on line 1", t.nm[1].line, 1);
    chk_i("[e] on line 2", t.nm[2].line, 2);
    grid_tracklist_free(&t);
}

/* ------------------------------------------------ auto-fill / auto-fit ----- */

static int reps_of(const char *tpl, int avail, int gap, int *ntracks)
{
    struct gtemplate t;
    struct gtracklist out;
    int n = -1;
    if (grid_parse_template(tpl, -1, 16, &t) < 0) return -1;
    if (grid_template_expand(&t, avail, gap, &out, &n) < 0) { grid_template_free(&t); return -1; }
    if (ntracks) *ntracks = out.n;
    grid_tracklist_free(&out);
    grid_template_free(&t);
    return n;
}

static void t_autofill(void)
{
    int nt;

    /* s7.2.3.1: the largest positive integer that does not overflow the content
     * box, gap included. 3x100 = 300 fits in 350; 4x100 = 400 does not. */
    chk_i("auto-fill 100px in 350, gap 0", reps_of("repeat(auto-fill, 100px)", 350, 0, &nt), 3);
    chk_i("auto-fill: track count", nt, 3);
    chk_i("auto-fill 100px in 400, gap 0", reps_of("repeat(auto-fill, 100px)", 400, 0, &nt), 4);
    chk_i("auto-fill 100px in 399, gap 0", reps_of("repeat(auto-fill, 100px)", 399, 0, &nt), 3);

    /* With a 10px gap: 3 tracks cost 300 + 2*10 = 320 (fits 350);
     * 4 cost 400 + 3*10 = 430 (does not). */
    chk_i("auto-fill 100px in 350, gap 10", reps_of("repeat(auto-fill, 100px)", 350, 10, &nt), 3);
    /* 4 tracks cost 400 + 30 = 430, exactly the available space. */
    chk_i("auto-fill 100px in 430, gap 10", reps_of("repeat(auto-fill, 100px)", 430, 10, &nt), 4);
    chk_i("auto-fill 100px in 429, gap 10", reps_of("repeat(auto-fill, 100px)", 429, 10, &nt), 3);

    /* A fixed track before the auto-repeat eats into the room. */
    chk_i("50px + auto-fill 100px in 350", reps_of("50px repeat(auto-fill, 100px)", 350, 0, &nt), 3);
    chk_i("50px + auto-fill: total tracks", nt, 4);

    /* "each track is treated as its max track sizing function if that is
     * definite or else its min track sizing function": minmax(100px, 1fr)
     * counts as 100px. */
    chk_i("auto-fill minmax(100px,1fr) in 350",
          reps_of("repeat(auto-fill, minmax(100px, 1fr))", 350, 0, &nt), 3);

    /* "if any number of repetitions would overflow, then 1 repetition" */
    chk_i("auto-fill 100px in 40", reps_of("repeat(auto-fill, 100px)", 40, 0, &nt), 1);

    /* "Otherwise, the specified track list repeats only once." */
    chk_i("auto-fill with indefinite space",
          reps_of("repeat(auto-fill, 100px)", GRID_INDEFINITE, 0, &nt), 1);

    /* Neither sizing function definite -> one repetition. */
    chk_i("auto-fill auto", reps_of("repeat(auto-fill, auto)", 500, 0, &nt), 1);

    /* auto-fit counts repetitions exactly like auto-fill; the difference is
     * that empty tracks collapse AFTER placement (checked in grid_align_test). */
    chk_i("auto-fit counts like auto-fill", reps_of("repeat(auto-fit, 100px)", 350, 0, &nt), 3);

    /* Two auto-repeats in one track list is invalid. */
    {
        struct gtemplate t;
        CHK(grid_parse_template("repeat(auto-fill,100px) repeat(auto-fill,100px)", -1, 16, &t) < 0,
            "two auto-repeats in one track list is invalid");
        grid_template_free(&t);
    }
}

/* ------------------------------------------------------------- <grid-line> - */

static void t_gridline(void)
{
    struct gline g;

    CHK(grid_parse_line("auto", -1, &g) == 0, "parse auto");
    chk_i("auto kind", g.kind, GL_AUTO);

    CHK(grid_parse_line("3", -1, &g) == 0, "parse 3");
    chk_i("3 kind", g.kind, GL_LINE);
    chk_i("3 n", g.n, 3);

    CHK(grid_parse_line("-1", -1, &g) == 0, "parse -1");
    chk_i("-1 n", g.n, -1);

    CHK(grid_parse_line("span 2", -1, &g) == 0, "parse span 2");
    chk_i("span 2 kind", g.kind, GL_SPAN);
    chk_i("span 2 n", g.n, 2);

    /* "If the <integer> is omitted, it defaults to 1." */
    CHK(grid_parse_line("span foo", -1, &g) == 0, "parse span foo");
    chk_i("span foo kind", g.kind, GL_SPAN);
    chk_i("span foo n defaults to 1", g.n, 1);
    CHK(strcmp(g.name, "foo") == 0, "span foo name");

    /* A bare <custom-ident> means the first line with that name. */
    CHK(grid_parse_line("foo", -1, &g) == 0, "parse foo");
    chk_i("foo kind", g.kind, GL_LINE);
    chk_i("foo has_n", g.has_n, 0);

    CHK(grid_parse_line("foo 2", -1, &g) == 0, "parse foo 2");
    chk_i("foo 2 n", g.n, 2);
    CHK(strcmp(g.name, "foo") == 0, "foo 2 name");

    /* "An <integer> value of zero makes the declaration invalid." */
    CHK(grid_parse_line("0", -1, &g) < 0, "line 0 is invalid");
    /* "Negative integers or zero are invalid" for span. */
    CHK(grid_parse_line("span -1", -1, &g) < 0, "span -1 is invalid");
    /* "the <custom-ident> additionally excludes the keywords span and auto" */
    CHK(grid_parse_line("span span", -1, &g) < 0, "span span is invalid");

    /* Shorthands. */
    {
        struct gline a, b;
        CHK(grid_parse_span2("2 / span 3", -1, &a, &b) == 0, "grid-column: 2 / span 3");
        chk_i("2/span3 start", a.n, 2);
        chk_i("2/span3 end kind", b.kind, GL_SPAN);
        chk_i("2/span3 end n", b.n, 3);

        /* s8.3: when the end is omitted and the start is a <custom-ident>,
         * both edges take that name. */
        CHK(grid_parse_span2("hdr", -1, &a, &b) == 0, "grid-column: hdr");
        CHK(strcmp(b.name, "hdr") == 0, "omitted end copies the ident");

        /* With an integer start, the omitted end is auto. */
        CHK(grid_parse_span2("2", -1, &a, &b) == 0, "grid-column: 2");
        chk_i("omitted end after integer is auto", b.kind, GL_AUTO);
    }
    {
        struct gline q[4];
        CHK(grid_parse_area("1 / 2 / 3 / 4", -1, q) == 0, "grid-area: 1/2/3/4");
        chk_i("area row-start",  q[0].n, 1);
        chk_i("area col-start",  q[1].n, 2);
        chk_i("area row-end",    q[2].n, 3);
        chk_i("area col-end",    q[3].n, 4);

        CHK(grid_parse_area("main", -1, q) == 0, "grid-area: main");
        CHK(strcmp(q[0].name, "main") == 0 && strcmp(q[1].name, "main") == 0 &&
            strcmp(q[2].name, "main") == 0 && strcmp(q[3].name, "main") == 0,
            "grid-area: <ident> fills all four edges");
    }
}

/* ------------------------------------------------------ template-areas ----- */

static void t_areas(void)
{
    struct gridareas a;

    /* The spec's own worked example (s7.3). */
    CHK(grid_parse_areas("\"head head\" \"nav  main\" \"foot ....\"", -1, &a) == 0,
        "parse the spec's head/nav/main/foot example");
    chk_i("areas rows", a.rows, 3);
    chk_i("areas cols", a.cols, 2);
    if (a.rows == 3 && a.cols == 2) {
        CHK(strcmp(a.cell[0], "head") == 0, "cell(0,0) = head");
        CHK(strcmp(a.cell[1], "head") == 0, "cell(0,1) = head");
        CHK(strcmp(a.cell[2], "nav")  == 0, "cell(1,0) = nav");
        CHK(strcmp(a.cell[3], "main") == 0, "cell(1,1) = main");
        CHK(strcmp(a.cell[4], "foot") == 0, "cell(2,0) = foot");
        CHK(a.cell[5][0] == 0, "cell(2,1) is a null cell");
    }
    grid_areas_free(&a);

    /* "A sequence of one or more '.' represents a null cell token" -- so ".."
     * is ONE cell, not two. */
    CHK(grid_parse_areas("\"a ..\"", -1, &a) == 0, "run of dots is one null cell");
    chk_i("\"a ..\" columns", a.cols, 2);
    grid_areas_free(&a);

    /* "All strings must define the same number of cell tokens ... or else the
     * declaration is invalid." */
    CHK(grid_parse_areas("\"a b\" \"c\"", -1, &a) < 0, "ragged rows are invalid");
    grid_areas_free(&a);

    /* "If a named grid area spans multiple grid cells, but those cells do not
     * form a single filled-in rectangle, the declaration is invalid." */
    CHK(grid_parse_areas("\"a b\" \"b a\"", -1, &a) < 0, "disconnected area is invalid");
    grid_areas_free(&a);
    CHK(grid_parse_areas("\"a a\" \"a .\"", -1, &a) < 0, "L-shaped area is invalid");
    grid_areas_free(&a);

    /* A genuine rectangle spanning both axes is fine. */
    CHK(grid_parse_areas("\"a a\" \"a a\"", -1, &a) == 0, "2x2 rectangle is valid");
    grid_areas_free(&a);

    /* A trash token is a syntax error. */
    CHK(grid_parse_areas("\"a $\"", -1, &a) < 0, "trash token is invalid");
    grid_areas_free(&a);

    CHK(grid_parse_areas("none", -1, &a) < 0 || a.rows == 0, "none defines no areas");
    grid_areas_free(&a);
}

static void t_misc(void)
{
    unsigned char col, dense;
    CHK(grid_parse_flow("row", -1, &col, &dense) == 0 && !col && !dense, "flow: row");
    CHK(grid_parse_flow("column", -1, &col, &dense) == 0 && col && !dense, "flow: column");
    CHK(grid_parse_flow("dense", -1, &col, &dense) == 0 && !col && dense, "flow: dense");
    CHK(grid_parse_flow("column dense", -1, &col, &dense) == 0 && col && dense, "flow: column dense");
    CHK(grid_parse_flow("sideways", -1, &col, &dense) < 0, "flow: bogus is invalid");

    chk_i("align: start",         grid_parse_align("start", -1), GA_START);
    chk_i("align: end",           grid_parse_align("end", -1), GA_END);
    chk_i("align: center",        grid_parse_align("center", -1), GA_CENTER);
    chk_i("align: stretch",       grid_parse_align("stretch", -1), GA_STRETCH);
    chk_i("align: space-between", grid_parse_align("space-between", -1), GA_SPACE_BETWEEN);
    chk_i("align: space-around",  grid_parse_align("space-around", -1), GA_SPACE_AROUND);
    chk_i("align: space-evenly",  grid_parse_align("space-evenly", -1), GA_SPACE_EVENLY);
    chk_i("align: baseline",      grid_parse_align("baseline", -1), GA_BASELINE);
    chk_i("align: last baseline", grid_parse_align("last baseline", -1), GA_LAST_BASELINE);
    /* safe/unsafe are accepted and do not change the positional value. */
    chk_i("align: safe center",   grid_parse_align("safe center", -1), GA_CENTER);
    chk_i("align: bogus",         grid_parse_align("diagonal", -1), -1);
}

int main(void)
{
    t_tracklist();
    t_repeat();
    t_autofill();
    t_gridline();
    t_areas();
    t_misc();

    printf("grid_parse_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
