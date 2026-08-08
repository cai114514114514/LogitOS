/* CSS Grid: placement.
 *
 * css-grid-1 s8.3 line-based placement, s8.4 the shorthands, s8.6 placement
 * conflict handling, s8.5 the grid item placement algorithm (sparse and dense).
 *
 * Placement is where a subtly wrong implementation produces a plausible-looking
 * wrong picture, so the assertions here are line indices, not rectangles. Two
 * whole groups are transcribed from worked examples in the spec itself -- the
 * nine-named-lines table in s8.3 and the `span foo / 4` figure -- because an
 * example the working group wrote down is the closest thing to an oracle this
 * subsystem has before reftests exist.
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

/* ------------------------------------------------------------- the scene -- */

#define MAXI 16
struct scene {
    struct gridcfg cfg;
    struct gridareas ar;
    struct griditem it[MAXI];
    struct gridpos out[MAXI];
    int n;
    int ncols, nrows, col0, row0;
};

static void scene_init(struct scene *s, const char *cols, const char *rows)
{
    memset(s, 0, sizeof *s);
    s->cfg.avail_w = s->cfg.avail_h = GRID_INDEFINITE;
    s->cfg.min_w = s->cfg.max_w = s->cfg.min_h = s->cfg.max_h = GRID_INDEFINITE;
    s->cfg.justify_content = s->cfg.align_content = GA_NORMAL;
    s->cfg.justify_items = s->cfg.align_items = GA_NORMAL;
    if (cols) grid_parse_template(cols, -1, 16, &s->cfg.cols);
    if (rows) grid_parse_template(rows, -1, 16, &s->cfg.rows);
}

static void scene_areas(struct scene *s, const char *areas)
{
    if (grid_parse_areas(areas, -1, &s->ar) == 0) s->cfg.areas = &s->ar;
}

/* Add an item with `grid-column: gc` and `grid-row: gr` (NULL = auto). */
static int scene_add(struct scene *s, const char *gc, const char *gr)
{
    struct griditem *it = &s->it[s->n];
    memset(it, 0, sizeof *it);
    it->def_w = it->def_h = GRID_INDEFINITE;
    if (gc) grid_parse_span2(gc, -1, &it->cs, &it->ce);
    if (gr) grid_parse_span2(gr, -1, &it->rs, &it->re);
    return s->n++;
}

/* Add an item with `grid-area: a`. */
static int scene_add_area(struct scene *s, const char *a)
{
    struct gline q[4];
    struct griditem *it = &s->it[s->n];
    memset(it, 0, sizeof *it);
    it->def_w = it->def_h = GRID_INDEFINITE;
    if (grid_parse_area(a, -1, q) == 0) {
        it->rs = q[0]; it->cs = q[1]; it->re = q[2]; it->ce = q[3];
    }
    return s->n++;
}

static int scene_run(struct scene *s)
{
    return grid_place(&s->cfg, s->it, s->n, s->out,
                      &s->ncols, &s->nrows, &s->col0, &s->row0);
}

static void scene_free(struct scene *s)
{
    grid_template_free(&s->cfg.cols);
    grid_template_free(&s->cfg.rows);
    grid_areas_free(&s->ar);
}

/* Explicit line number L as a 0-based track index. */
static int trk(int base0, int L) { return L - 1 + base0; }

/* ------------------------------------------------------- explicit lines --- */

static void t_explicit(void)
{
    struct scene s;

    scene_init(&s, "100px 100px 100px", "100px");
    scene_add(&s, "2 / 4", NULL);
    CHK(scene_run(&s) == 0, "explicit placement runs");
    chk_i("grid-column: 2/4 -> track", s.out[0].col, trk(s.col0, 2));
    chk_i("grid-column: 2/4 -> span",  s.out[0].colspan, 2);
    scene_free(&s);

    /* "If a negative integer is given, it counts in reverse, starting from the
     * end edge of the explicit grid." 3 tracks -> 4 lines, so -1 is line 4. */
    scene_init(&s, "100px 100px 100px", "100px");
    scene_add(&s, "-2 / -1", NULL);
    scene_run(&s);
    chk_i("grid-column: -2/-1 -> track", s.out[0].col, trk(s.col0, 3));
    chk_i("grid-column: -2/-1 -> span",  s.out[0].colspan, 1);
    scene_free(&s);

    /* s8.6: "If the start line is equal to the end line, remove the end line."
     * A span of 1 results, not a zero-width area. */
    scene_init(&s, "100px 100px 100px", "100px");
    scene_add(&s, "2 / 2", NULL);
    scene_run(&s);
    chk_i("grid-column: 2/2 -> track", s.out[0].col, trk(s.col0, 2));
    chk_i("grid-column: 2/2 -> span 1", s.out[0].colspan, 1);
    scene_free(&s);

    /* s8.6: "If the start line is further end-ward than the end line, swap." */
    scene_init(&s, "100px 100px 100px", "100px");
    scene_add(&s, "4 / 2", NULL);
    scene_run(&s);
    chk_i("grid-column: 4/2 swaps -> track", s.out[0].col, trk(s.col0, 2));
    chk_i("grid-column: 4/2 swaps -> span",  s.out[0].colspan, 2);
    scene_free(&s);

    /* An explicit end past the explicit grid creates implicit tracks (the
     * example under s8.5 step 3: 5 columns + `grid-column: 4 / span 3` needs 6). */
    scene_init(&s, "repeat(5, 100px)", "100px");
    scene_add(&s, "4 / span 3", NULL);
    scene_run(&s);
    chk_i("5 cols + `4 / span 3` needs 6 columns", s.ncols, 6);
    chk_i("that item starts at track 3", s.out[0].col, trk(s.col0, 4));
    chk_i("that item spans 3", s.out[0].colspan, 3);
    scene_free(&s);
}

/* ---------------------------------------------------------- named lines --- */

/* The table in s8.3: a single-row, 8-column grid whose nine lines are named
 *   1  2  3  4  5  6  7  8  9
 *   A  B  C  A  B  C  A  B  C
 * and the exact placements the spec lists for ten declarations. */
#define NAMED8 "[A] 100px [B] 100px [C] 100px [A] 100px [B] 100px [C] 100px [A] 100px [B] 100px [C]"

static void named_case(const char *decl, int want_start_line, int want_span)
{
    struct scene s;
    scene_init(&s, NAMED8, "100px");
    scene_add(&s, decl, NULL);
    if (scene_run(&s) != 0) { checks++; fails++; printf("FAIL named `%s`: place failed\n", decl); scene_free(&s); return; }
    checks++;
    if (s.out[0].col != trk(s.col0, want_start_line) || s.out[0].colspan != want_span) {
        fails++;
        printf("FAIL grid-column: %s -> lines %d..%d, want %d..%d\n", decl,
               s.out[0].col - s.col0 + 1, s.out[0].col - s.col0 + 1 + s.out[0].colspan,
               want_start_line, want_start_line + want_span);
    }
    scene_free(&s);
}

static void t_named(void)
{
    named_case("4 / auto",        4, 1);   /* Line 4 to line 5 */
    named_case("auto / 6",        5, 1);   /* Line 5 to line 6 */
    named_case("C / C -1",        3, 6);   /* Line 3 to line 9 */
    named_case("C / span C",      3, 3);   /* Line 3 to line 6 */
    named_case("span C / C -1",   6, 3);   /* Line 6 to line 9 */
    named_case("5 / C -1",        5, 4);   /* Line 5 to line 9 */
    named_case("5 / span C",      5, 1);   /* Line 5 to line 6 */
    named_case("8 / 8",           8, 1);   /* Error case: line 8 to line 9 */
    named_case("B 2 / span 1",    5, 1);   /* Line 5 to line 6 */

    /* "Error: The end span is ignored, and an auto-placed item can't span to a
     * named line. Equivalent to `grid-column: span 1`." Auto-placed, so it
     * lands in the first free cell; only the span is the spec's claim. */
    {
        struct scene s;
        scene_init(&s, NAMED8, "100px");
        scene_add(&s, "span C / span C", NULL);
        scene_run(&s);
        chk_i("`span C / span C` degrades to span 1", s.out[0].colspan, 1);
        scene_free(&s);
    }

    /* A bare <custom-ident> means the FIRST line with that name. */
    {
        struct scene s;
        scene_init(&s, NAMED8, "100px");
        scene_add(&s, "B / auto", NULL);
        scene_run(&s);
        chk_i("`B` is the first B line (line 2)", s.out[0].col, trk(s.col0, 2));
        scene_free(&s);
    }

    /* s8.3, the `span foo / 4` figure: with one explicit 100px column there are
     * two lines; the end edge at line 4 creates two implicit lines endward, and
     * a startward span for a name that does not exist can only be satisfied by
     * generating a line on the STARTWARD side of the explicit grid -- line 0. */
    {
        struct scene s;
        scene_init(&s, "100px", "100px");
        scene_add(&s, "span foo / 4", NULL);
        scene_run(&s);
        chk_i("`span foo / 4`: implicit grid is 4 columns wide", s.ncols, 4);
        chk_i("`span foo / 4`: explicit line 1 is track 1", s.col0, 1);
        chk_i("`span foo / 4`: item starts at the startward implicit line", s.out[0].col, 0);
        chk_i("`span foo / 4`: item spans 4", s.out[0].colspan, 4);
        scene_free(&s);
    }

    /* "If not enough lines with that name exist, all implicit grid lines are
     * assumed to have that name." Two A lines exist at 1 and 4 and 7; asking
     * for the 5th counts on past the explicit end (9 lines, 3 named) -> 9+2. */
    {
        struct scene s;
        /* Two tracks, so three explicit lines, all named A. The 4th and 5th A
         * lines can only be implicit ones: line 4, then line 5. */
        scene_init(&s, "[A] 100px [A] 100px [A]", "100px");
        scene_add(&s, "A 5 / auto", NULL);
        scene_run(&s);
        chk_i("A 5 with only 3 A lines counts into the implicit grid",
              s.out[0].col, trk(s.col0, 5));
        scene_free(&s);
    }
}

/* ------------------------------------------------------- auto-placement --- */

static void t_autoplace(void)
{
    struct scene s;
    int i;

    /* Plain sparse flow across a 3-column grid. */
    scene_init(&s, "repeat(3, 100px)", NULL);
    for (i = 0; i < 5; i++) scene_add(&s, NULL, NULL);
    scene_run(&s);
    chk_i("sparse: item0 col", s.out[0].col, 0); chk_i("sparse: item0 row", s.out[0].row, 0);
    chk_i("sparse: item1 col", s.out[1].col, 1); chk_i("sparse: item1 row", s.out[1].row, 0);
    chk_i("sparse: item2 col", s.out[2].col, 2); chk_i("sparse: item2 row", s.out[2].row, 0);
    chk_i("sparse: item3 col", s.out[3].col, 0); chk_i("sparse: item3 row", s.out[3].row, 1);
    chk_i("sparse: item4 col", s.out[4].col, 1); chk_i("sparse: item4 row", s.out[4].row, 1);
    chk_i("sparse: two implicit rows", s.nrows, 2);
    scene_free(&s);

    /* Seven items in three columns need three rows. */
    scene_init(&s, "repeat(3, 100px)", NULL);
    for (i = 0; i < 7; i++) scene_add(&s, NULL, NULL);
    scene_run(&s);
    chk_i("7 items / 3 columns = 3 rows", s.nrows, 3);
    chk_i("item6 lands at row 2 col 0", s.out[6].row, 2);
    scene_free(&s);

    /* An auto item wider than one track. */
    scene_init(&s, "repeat(3, 100px)", NULL);
    scene_add(&s, "span 2", NULL);
    scene_add(&s, "span 2", NULL);
    scene_run(&s);
    chk_i("span 2 item0 col", s.out[0].col, 0);
    chk_i("span 2 item1 does not fit beside it -> next row", s.out[1].row, 1);
    chk_i("span 2 item1 col", s.out[1].col, 0);
    scene_free(&s);

    /* SPARSE vs DENSE. One item pinned to columns 2..3 with an auto row, then
     * three auto items. Sparse never moves the cursor backwards, so cell (0,0)
     * is left empty; dense goes back and fills it. This is the assertion that
     * separates the two packing modes. */
    scene_init(&s, "repeat(3, 100px)", NULL);
    scene_add(&s, "2 / 4", NULL);
    scene_add(&s, NULL, NULL);
    scene_add(&s, NULL, NULL);
    scene_add(&s, NULL, NULL);
    scene_run(&s);
    chk_i("sparse: pinned item row", s.out[0].row, 0);
    chk_i("sparse: pinned item col", s.out[0].col, 1);
    chk_i("sparse: B row (cursor cannot go back to col 0)", s.out[1].row, 1);
    chk_i("sparse: B col", s.out[1].col, 0);
    chk_i("sparse: C col", s.out[2].col, 1);
    chk_i("sparse: D col", s.out[3].col, 2);
    chk_i("sparse: rows", s.nrows, 2);
    scene_free(&s);

    scene_init(&s, "repeat(3, 100px)", NULL);
    s.cfg.flow_dense = 1;
    scene_add(&s, "2 / 4", NULL);
    scene_add(&s, NULL, NULL);
    scene_add(&s, NULL, NULL);
    scene_add(&s, NULL, NULL);
    scene_run(&s);
    chk_i("dense: pinned item col", s.out[0].col, 1);
    chk_i("dense: B backfills (0,0) -- row", s.out[1].row, 0);
    chk_i("dense: B backfills (0,0) -- col", s.out[1].col, 0);
    chk_i("dense: C row", s.out[2].row, 1);
    chk_i("dense: C col", s.out[2].col, 0);
    chk_i("dense: D row", s.out[3].row, 1);
    chk_i("dense: D col", s.out[3].col, 1);
    scene_free(&s);

    /* Items locked to a row (s8.5 step 2) pack along it in order. */
    scene_init(&s, "repeat(3, 100px)", "repeat(2, 100px)");
    scene_add(&s, NULL, "2 / 3");
    scene_add(&s, NULL, "2 / 3");
    scene_run(&s);
    chk_i("row-locked item0 col", s.out[0].col, 0);
    chk_i("row-locked item0 row", s.out[0].row, 1);
    chk_i("row-locked item1 col", s.out[1].col, 1);
    chk_i("row-locked item1 row", s.out[1].row, 1);
    scene_free(&s);

    /* order-modified document order: the algorithm runs in `order` order. */
    scene_init(&s, "repeat(2, 100px)", NULL);
    scene_add(&s, NULL, NULL);
    scene_add(&s, NULL, NULL);
    s.it[1].order = -1;
    scene_run(&s);
    chk_i("order: the later item with order:-1 is placed first", s.out[1].col, 0);
    chk_i("order: the earlier item follows it", s.out[0].col, 1);
    scene_free(&s);

    /* grid-auto-flow: column swaps the two axes wholesale. */
    scene_init(&s, NULL, "repeat(2, 100px)");
    s.cfg.flow_col = 1;
    for (i = 0; i < 3; i++) scene_add(&s, NULL, NULL);
    scene_run(&s);
    chk_i("flow column: item0 (col,row)", s.out[0].col*10 + s.out[0].row, 0);
    chk_i("flow column: item1 (col,row)", s.out[1].col*10 + s.out[1].row, 1);
    chk_i("flow column: item2 (col,row)", s.out[2].col*10 + s.out[2].row, 10);
    chk_i("flow column: two columns", s.ncols, 2);
    scene_free(&s);

    /* An explicit row past the explicit grid creates implicit rows. */
    scene_init(&s, "100px", "repeat(3, 100px)");
    scene_add(&s, NULL, "5 / 6");
    scene_run(&s);
    chk_i("grid-row: 5/6 with 3 explicit rows -> 5 rows", s.nrows, 5);
    chk_i("that item is in row 4", s.out[0].row, 4);
    scene_free(&s);
}

/* ------------------------------------------------------------- areas ------ */

static void t_areas(void)
{
    struct scene s;

    /* The spec's head / nav / main / foot layout. Placement by area name goes
     * through the implicitly-assigned `<name>-start` / `<name>-end` lines. */
    scene_init(&s, "100px 200px", "50px 50px 50px");
    scene_areas(&s, "\"head head\" \"nav main\" \"foot ....\"");
    scene_add_area(&s, "head");
    scene_add_area(&s, "main");
    scene_add_area(&s, "nav");
    CHK(scene_run(&s) == 0, "areas placement runs");
    chk_i("head: col",     s.out[0].col, 0);
    chk_i("head: colspan", s.out[0].colspan, 2);
    chk_i("head: row",     s.out[0].row, 0);
    chk_i("head: rowspan", s.out[0].rowspan, 1);
    chk_i("main: col",     s.out[1].col, 1);
    chk_i("main: row",     s.out[1].row, 1);
    chk_i("nav: col",      s.out[2].col, 0);
    chk_i("nav: row",      s.out[2].row, 1);
    scene_free(&s);

    /* An area generates line names usable on their own: `grid-row-start: main`
     * resolves through the implicit `main-start` line. */
    scene_init(&s, "100px 200px", "50px 50px 50px");
    scene_areas(&s, "\"head head\" \"nav main\" \"foot ....\"");
    {
        struct griditem *it = &s.it[s.n];
        memset(it, 0, sizeof *it);
        it->def_w = it->def_h = GRID_INDEFINITE;
        grid_parse_line("foot", -1, &it->rs);
        s.n++;
    }
    scene_run(&s);
    chk_i("grid-row-start: foot resolves to the area's start line", s.out[0].row, 2);
    scene_free(&s);

    /* grid-template-areas defines the explicit grid on its own: three strings
     * of two cells make a 3x2 explicit grid even with no templates. */
    scene_init(&s, NULL, NULL);
    scene_areas(&s, "\"a b\" \"c d\" \"e f\"");
    scene_add_area(&s, "f");
    scene_run(&s);
    chk_i("areas alone define 2 columns", s.ncols, 2);
    chk_i("areas alone define 3 rows",    s.nrows, 3);
    chk_i("area f is the bottom-right cell -- col", s.out[0].col, 1);
    chk_i("area f is the bottom-right cell -- row", s.out[0].row, 2);
    scene_free(&s);
}

int main(void)
{
    t_explicit();
    t_named();
    t_autoplace();
    t_areas();
    printf("grid_place_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
