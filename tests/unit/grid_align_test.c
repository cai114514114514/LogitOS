/* CSS Grid: alignment, gutters, auto-fit collapsing, and the two-pass order.
 *
 * css-grid-1 s10.1 gutters, s10.3 justify-content / align-content, s11.1 the
 * grid sizing algorithm's column-then-row order, s7.2.3.1 auto-fit collapsing;
 * css-align-3 s4 the distribution values and their fallbacks, s5/s6 the
 * self-alignment properties.
 *
 * These go through grid_layout(), the entry point layout.c will call, so they
 * check the whole path: placement -> columns -> rows -> track alignment ->
 * item rectangles.
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

/* ------------------------------------------------------------ the scene -- */

#define MAXI 16
struct meas { int cmin, cminc, cmaxc; int rmin, rminc, rmaxc; int narrow_h; };

struct scene {
    struct gridcfg cfg;
    struct gridareas ar;
    struct griditem it[MAXI];
    struct meas m[MAXI];
    struct gridout out;
    int n;
};

static void measure(void *ctx, int i, int axis, int avail, struct gmeas *o)
{
    struct scene *s = (struct scene *)ctx;
    memset(o, 0, sizeof *o);
    if (axis == GAX_COL) {
        o->minimum = s->m[i].cmin; o->min_content = s->m[i].cminc; o->max_content = s->m[i].cmaxc;
    } else {
        int h = s->m[i].rmaxc;
        /* A block that reflows: when the inline size it was given is at least
         * `narrow_h` px wide it needs rmaxc, otherwise twice that. This is the
         * only way to observe that rows really are sized AFTER columns, at the
         * inline size the columns produced (s11.1 steps 1 and 2). */
        if (s->m[i].narrow_h > 0 && avail != GRID_INDEFINITE && avail < s->m[i].narrow_h)
            h = s->m[i].rmaxc * 2;
        o->minimum = s->m[i].rmin ? s->m[i].rmin : h;
        o->min_content = s->m[i].rminc ? s->m[i].rminc : h;
        o->max_content = h;
    }
}

static void scene_init(struct scene *s, const char *cols, const char *rows, int aw, int ah)
{
    memset(s, 0, sizeof *s);
    s->cfg.avail_w = aw; s->cfg.avail_h = ah;
    s->cfg.min_w = s->cfg.max_w = s->cfg.min_h = s->cfg.max_h = GRID_INDEFINITE;
    s->cfg.justify_content = s->cfg.align_content = GA_NORMAL;
    s->cfg.justify_items = s->cfg.align_items = GA_NORMAL;
    if (cols) grid_parse_template(cols, -1, 16, &s->cfg.cols);
    if (rows) grid_parse_template(rows, -1, 16, &s->cfg.rows);
}

static int scene_add(struct scene *s, const char *gc, const char *gr, int w, int h)
{
    struct griditem *it = &s->it[s->n];
    memset(it, 0, sizeof *it);
    it->def_w = w; it->def_h = h;
    it->justify_self = it->align_self = GA_AUTO;
    if (gc) grid_parse_span2(gc, -1, &it->cs, &it->ce);
    if (gr) grid_parse_span2(gr, -1, &it->rs, &it->re);
    if (w != GRID_INDEFINITE) { s->m[s->n].cmin = s->m[s->n].cminc = s->m[s->n].cmaxc = w; }
    if (h != GRID_INDEFINITE) { s->m[s->n].rmaxc = h; }
    return s->n++;
}

static int scene_run(struct scene *s)
{
    return grid_layout(&s->cfg, s->it, s->n, measure, s, &s->out);
}

static void scene_free(struct scene *s)
{
    grid_out_free(&s->out);
    grid_template_free(&s->cfg.cols);
    grid_template_free(&s->cfg.rows);
    grid_areas_free(&s->ar);
}

/* Check the column track positions in one go. */
static void chk_pos(struct scene *s, const char *what, const int *want, int n)
{
    int i, bad = 0;
    checks++;
    if (s->out.ncols != n) {
        fails++; printf("FAIL %s: %d columns, want %d\n", what, s->out.ncols, n);
        return;
    }
    for (i = 0; i < n; i++) if (s->out.colpos[i] != want[i]) bad = 1;
    if (bad) {
        fails++;
        printf("FAIL %s: colpos [", what);
        for (i = 0; i < n; i++) printf("%s%d", i ? ", " : "", s->out.colpos[i]);
        printf("], want [");
        for (i = 0; i < n; i++) printf("%s%d", i ? ", " : "", want[i]);
        printf("]\n");
    }
}

/* ---------------------------------------------------- content alignment --- */

static void content_case(const char *what, unsigned char jc, int aw, int gap,
                         const int *want, int n)
{
    struct scene s;
    scene_init(&s, "100px 100px", "50px", aw, GRID_INDEFINITE);
    s.cfg.justify_content = jc;
    s.cfg.gap_x = gap;
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    if (scene_run(&s) != 0) { checks++; fails++; printf("FAIL %s: layout failed\n", what); }
    else chk_pos(&s, what, want, n);
    scene_free(&s);
}

static void t_content(void)
{
    int w[2];

    w[0] = 0;   w[1] = 100; content_case("justify-content: start",  GA_START, 400, 0, w, 2);
    w[0] = 200; w[1] = 300; content_case("justify-content: end",    GA_END,   400, 0, w, 2);
    w[0] = 100; w[1] = 200; content_case("justify-content: center", GA_CENTER,400, 0, w, 2);

    /* css-align-3 s4: "The first alignment subject is placed flush with the
     * start edge, the last flush with the end edge." */
    w[0] = 0;   w[1] = 300; content_case("justify-content: space-between",
                                         GA_SPACE_BETWEEN, 400, 0, w, 2);
    /* "with a half-size space on either end" -- 200 free / 2 tracks = 100
     * between, 50 at each end. */
    w[0] = 50;  w[1] = 250; content_case("justify-content: space-around",
                                         GA_SPACE_AROUND, 400, 0, w, 2);
    /* "with a full-size space on either end" -- 300 free / 3 gaps = 100 each. */
    w[0] = 100; w[1] = 300; content_case("justify-content: space-evenly",
                                         GA_SPACE_EVENLY, 500, 0, w, 2);

    /* s10.1: gutters only appear BETWEEN tracks, never before the first or
     * after the last. */
    w[0] = 0;   w[1] = 120; content_case("gap: 20px with justify-content: start",
                                         GA_START, 400, 20, w, 2);
    /* "Additional spacing may be added between tracks due to justify-content
     * ... This space effectively increases the size of the gutters." */
    w[0] = 0;   w[1] = 300; content_case("gap + space-between adds to the gutter",
                                         GA_SPACE_BETWEEN, 400, 20, w, 2);

    /* css-align-3 s4: the distribution values fall back when the space cannot
     * be distributed -- space-between to start, space-around/evenly to safe
     * center, which is start when the content overflows. */
    w[0] = 0;   w[1] = 100; content_case("space-between falls back when overflowing",
                                         GA_SPACE_BETWEEN, 100, 0, w, 2);
    w[0] = 0;   w[1] = 100; content_case("space-around falls back when overflowing",
                                         GA_SPACE_AROUND, 100, 0, w, 2);

    /* One track: space-between has no gap to grow, so it falls back to start. */
    {
        struct scene s;
        int p[1];
        scene_init(&s, "100px", "50px", 400, GRID_INDEFINITE);
        s.cfg.justify_content = GA_SPACE_BETWEEN;
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
        scene_run(&s);
        p[0] = 0;
        chk_pos(&s, "space-between with one track falls back to start", p, 1);
        scene_free(&s);
    }

    /* direction: rtl makes start the right edge, and `normal` follows it. */
    {
        struct scene s;
        int p[1];
        scene_init(&s, "100px", "50px", 400, GRID_INDEFINITE);
        s.cfg.rtl = 1;
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
        scene_run(&s);
        p[0] = 300;
        chk_pos(&s, "rtl: tracks start from the right edge", p, 1);
        scene_free(&s);
    }

    /* align-content works the same way down the block axis. */
    {
        struct scene s;
        scene_init(&s, "100px", "50px 50px", 100, 300);
        s.cfg.align_content = GA_SPACE_BETWEEN;
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
        scene_run(&s);
        chk_i("align-content: space-between, row 0", s.out.rowpos[0], 0);
        chk_i("align-content: space-between, row 1", s.out.rowpos[1], 250);
        scene_free(&s);
    }
}

/* ------------------------------------------------------ self alignment ---- */

static void self_case(const char *what, unsigned char js, int want_x, int want_w)
{
    struct scene s;
    scene_init(&s, "100px", "50px", 100, 50);
    s.cfg.justify_items = js;
    scene_add(&s, NULL, NULL, 40, GRID_INDEFINITE);
    if (scene_run(&s) != 0) { checks++; fails++; printf("FAIL %s: layout failed\n", what); }
    else {
        chk_i(what, s.out.items[0].x, want_x);
        if (want_w >= 0) chk_i("(width)", s.out.items[0].w, want_w);
    }
    scene_free(&s);
}

static void t_self(void)
{
    self_case("justify-items: start on a 40px item in a 100px track",  GA_START,  0,  40);
    self_case("justify-items: center", GA_CENTER, 30, 40);
    self_case("justify-items: end",    GA_END,    60, 40);

    /* An item with no definite size stretches to fill its area (the initial
     * `normal` behaviour for a non-replaced grid item). */
    {
        struct scene s;
        scene_init(&s, "100px", "50px", 100, 50);
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
        s.m[0].cmaxc = 30;
        scene_run(&s);
        chk_i("an auto-sized item stretches to its area", s.out.items[0].w, 100);
        chk_i("... at x = 0", s.out.items[0].x, 0);
        scene_free(&s);
    }

    /* ... but a NON-stretch alignment shrink-to-fits it instead. */
    {
        struct scene s;
        scene_init(&s, "100px", "50px", 100, 50);
        s.cfg.justify_items = GA_CENTER;
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
        s.m[0].cminc = 10; s.m[0].cmaxc = 30;
        scene_run(&s);
        chk_i("justify-items: center shrink-to-fits", s.out.items[0].w, 30);
        chk_i("... and centres it", s.out.items[0].x, 35);
        scene_free(&s);
    }

    /* justify-self overrides justify-items for one item. */
    {
        struct scene s;
        scene_init(&s, "100px 100px", "50px", 200, 50);
        s.cfg.justify_items = GA_START;
        scene_add(&s, "1 / 2", NULL, 40, GRID_INDEFINITE);
        scene_add(&s, "2 / 3", NULL, 40, GRID_INDEFINITE);
        s.it[1].justify_self = GA_END;
        scene_run(&s);
        chk_i("justify-items: start applies to item 0", s.out.items[0].x, 0);
        chk_i("justify-self: end overrides it on item 1", s.out.items[1].x, 160);
        scene_free(&s);
    }

    /* align-items down the block axis. */
    {
        struct scene s;
        scene_init(&s, "100px", "100px", 100, 100);
        s.cfg.align_items = GA_CENTER;
        scene_add(&s, NULL, NULL, GRID_INDEFINITE, 40);
        scene_run(&s);
        chk_i("align-items: center", s.out.items[0].y, 30);
        scene_free(&s);
    }

    /* Margins come off the grid area before alignment. */
    {
        struct scene s;
        scene_init(&s, "100px", "50px", 100, 50);
        s.cfg.justify_items = GA_START;
        scene_add(&s, NULL, NULL, 40, GRID_INDEFINITE);
        s.it[0].margin[3] = 10;
        scene_run(&s);
        chk_i("margin-left shifts a start-aligned item", s.out.items[0].x, 10);
        scene_free(&s);
    }

    /* s10.2: "auto margins in either axis absorb positive free space prior to
     * alignment ... thereby disabling the effects of any self-alignment
     * properties in that axis." */
    {
        struct scene s;
        scene_init(&s, "100px", "50px", 100, 50);
        s.cfg.justify_items = GA_START;
        scene_add(&s, NULL, NULL, 40, GRID_INDEFINITE);
        s.it[0].m_auto[3] = 1;
        scene_run(&s);
        chk_i("margin-left: auto beats justify-items: start", s.out.items[0].x, 60);
        scene_free(&s);
    }
    {
        struct scene s;
        scene_init(&s, "100px", "50px", 100, 50);
        s.cfg.justify_items = GA_START;
        scene_add(&s, NULL, NULL, 40, GRID_INDEFINITE);
        s.it[0].m_auto[1] = s.it[0].m_auto[3] = 1;
        scene_run(&s);
        chk_i("two auto margins centre the item", s.out.items[0].x, 30);
        scene_free(&s);
    }
}

/* --------------------------------------------------- areas and spanning --- */

static void t_area_rects(void)
{
    struct scene s;

    /* An item spanning two tracks owns the gutter between them: its grid area
     * is 100 + 20 + 100, not 200. */
    scene_init(&s, "100px 100px", "50px", 220, 50);
    s.cfg.gap_x = 20;
    scene_add(&s, "1 / 3", NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("a spanning item's area covers the gutter", s.out.items[0].area_w, 220);
    chk_i("... and starts at 0", s.out.items[0].area_x, 0);
    scene_free(&s);

    /* The same with alignment-added spacing: s10.3 notes that the extra space
     * justify-content puts between tracks enlarges a spanning item's area. */
    scene_init(&s, "100px 100px", "50px", 400, 50);
    s.cfg.justify_content = GA_SPACE_BETWEEN;
    scene_add(&s, "1 / 3", NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("space-between enlarges a spanning item's area", s.out.items[0].area_w, 400);
    scene_free(&s);

    /* Placement by area name lands on the right rectangle. */
    scene_init(&s, "100px 200px", "50px 60px 70px", 300, 180);
    if (grid_parse_areas("\"head head\" \"nav main\" \"foot ....\"", -1, &s.ar) == 0)
        s.cfg.areas = &s.ar;
    {
        struct gline q[4];
        struct griditem *it = &s.it[s.n];
        memset(it, 0, sizeof *it);
        it->def_w = it->def_h = GRID_INDEFINITE;
        it->justify_self = it->align_self = GA_AUTO;
        grid_parse_area("main", -1, q);
        it->rs = q[0]; it->cs = q[1]; it->re = q[2]; it->ce = q[3];
        s.n++;
    }
    scene_run(&s);
    chk_i("area main: x", s.out.items[0].area_x, 100);
    chk_i("area main: y", s.out.items[0].area_y, 50);
    chk_i("area main: w", s.out.items[0].area_w, 200);
    chk_i("area main: h", s.out.items[0].area_h, 60);
    scene_free(&s);
}

/* ------------------------------------------------------------- auto-fit --- */

static void t_autofit(void)
{
    struct scene s;

    /* s7.2.3.1: auto-fit collapses any repeated track that no in-flow item was
     * placed into or spans across. Three 100px tracks fit in 350; with a single
     * item, two of them collapse to zero. */
    scene_init(&s, "repeat(auto-fit, 100px)", "50px", 350, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("auto-fit: three tracks exist", s.out.ncols, 3);
    chk_i("auto-fit: the occupied track keeps its size", s.out.colsz[0], 100);
    chk_i("auto-fit: empty track 1 collapses", s.out.colsz[1], 0);
    chk_i("auto-fit: empty track 2 collapses", s.out.colsz[2], 0);
    scene_free(&s);

    /* "A collapsed grid track is treated as having a fixed track sizing
     * function of 0px, and the gutters on either side of it collapse." A run of
     * collapsed tracks at the end must leave no trailing gutter, so the grid is
     * exactly one 100px track wide. */
    scene_init(&s, "repeat(auto-fit, 100px)", "50px", 350, GRID_INDEFINITE);
    s.cfg.gap_x = 10;
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("auto-fit: collapsed gutters leave no trailing gap", s.out.width, 100);
    scene_free(&s);

    /* The classic use: the surviving track takes all the space back. */
    scene_init(&s, "repeat(auto-fit, minmax(100px, 1fr))", "50px", 350, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("auto-fit + minmax(100px,1fr): survivor takes the width", s.out.colsz[0], 350);
    scene_free(&s);

    /* auto-fill does NOT collapse: all three tracks keep their size. */
    scene_init(&s, "repeat(auto-fill, 100px)", "50px", 350, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("auto-fill keeps its empty tracks -- track 1", s.out.colsz[1], 100);
    chk_i("auto-fill keeps its empty tracks -- track 2", s.out.colsz[2], 100);
    scene_free(&s);
}

/* ------------------------------------------------------ the two-pass order- */

static void t_two_pass(void)
{
    struct scene s;

    /* s11.1: columns are sized FIRST, then rows are sized using the inline
     * sizes that came out. An item whose height depends on the width it is
     * given must be measured at the resolved column width, not at the
     * container width and not at an infinite one.
     *
     * Here the item needs 50px of height at 200px wide and 100px at anything
     * narrower. In a 200px column it must produce a 50px row; in a 150px column
     * the same item must produce a 100px row. If rows were sized before columns
     * -- or at the wrong width -- one of these two comes out wrong. */
    scene_init(&s, "200px", "auto", 200, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    s.m[0].cminc = 10; s.m[0].cmaxc = 400;
    s.m[0].rmaxc = 50; s.m[0].narrow_h = 200;
    scene_run(&s);
    chk_i("row sized at the resolved 200px column width", s.out.rowsz[0], 50);
    scene_free(&s);

    scene_init(&s, "150px", "auto", 150, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    s.m[0].cminc = 10; s.m[0].cmaxc = 400;
    s.m[0].rmaxc = 50; s.m[0].narrow_h = 200;
    scene_run(&s);
    chk_i("the same item in a 150px column reflows to a 100px row", s.out.rowsz[0], 100);
    scene_free(&s);

    /* The item's margins come off the inline size it is measured at. */
    scene_init(&s, "200px", "auto", 200, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    s.it[0].margin[1] = s.it[0].margin[3] = 10;
    s.m[0].cminc = 10; s.m[0].cmaxc = 400;
    s.m[0].rmaxc = 50; s.m[0].narrow_h = 200;
    scene_run(&s);
    chk_i("margins narrow the measured inline size", s.out.rowsz[0], 100);
    scene_free(&s);

    /* Implicit rows take their sizing function from grid-auto-rows. */
    scene_init(&s, "100px", "50px", 100, GRID_INDEFINITE);
    grid_parse_tracklist("30px", -1, 16, &s.cfg.auto_rows);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("explicit row keeps its 50px", s.out.rowsz[0], 50);
    chk_i("the implicit row takes grid-auto-rows", s.out.rowsz[1], 30);
    grid_tracklist_free(&s.cfg.auto_rows);
    scene_free(&s);

    /* s7.5: grid-auto-* cycles, and the track BEFORE the explicit grid takes
     * the last size, counting backwards. */
    scene_init(&s, "100px", "50px", 100, GRID_INDEFINITE);
    grid_parse_tracklist("10px 20px", -1, 16, &s.cfg.auto_rows);
    scene_add(&s, NULL, NULL, GRID_INDEFINITE, GRID_INDEFINITE);
    scene_add(&s, NULL, "2 / 3", GRID_INDEFINITE, GRID_INDEFINITE);
    scene_add(&s, NULL, "3 / 4", GRID_INDEFINITE, GRID_INDEFINITE);
    scene_run(&s);
    chk_i("first implicit row after the explicit grid", s.out.rowsz[1], 10);
    chk_i("second implicit row cycles to the next size", s.out.rowsz[2], 20);
    grid_tracklist_free(&s.cfg.auto_rows);
    scene_free(&s);
}

int main(void)
{
    t_content();
    t_self();
    t_area_rects();
    t_autofit();
    t_two_pass();
    printf("grid_align_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
