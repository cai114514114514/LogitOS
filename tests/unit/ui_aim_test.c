/* ui_aim_test.c -- ui_aim.h and the widget-level aims of the real ring-3
 * aui.c (make test-ui-aim; design P13, addendum I).
 *
 *   lru        70 distinct names are ALL printed (the 64-slot table evicts,
 *              it never drops), each exactly once         FAIL aim-lru
 *   format     `[<app>] aim <name> <x> <y>\n`, one SYS_WRITE per line, by
 *              value (a reused stack buffer), negative coordinates, nothing
 *              before ui_aim_set_app                        FAIL aim-format
 *   names      ui_aim_i gives distinct `<prefix>-<i>` names  FAIL aim-i
 *   quiet      an unchanged second frame (tabs, segmented, table, select,
 *              popup, dialog) prints ZERO lines; a moved item re-prints only
 *              itself                                       FAIL aim-quiet
 *   table      aims only for the rows whose centre is visible, at the row
 *              centre, 28 pt apart (design 4.8), following the scroll
 *                                                           FAIL aim-table
 *   motion     no aim while the segmented thumb or the tab underline moves;
 *              all of them once it settles                  FAIL aim-motion
 *   widgets    tabs, segmented (names), select control and popup-<i> rows,
 *              dialog-<i> / dialog-confirm / dialog-cancel only after the
 *              entrance                                     FAIL aim-widgets
 *
 * THE CONTROL (-DUI_AIM_NEGCTL) is the fixed-size table that drops new names
 * once full: it must report 'FAIL aim-lru' and nothing else. Exits 1 on any
 * failure. */
#include "ui_aui_core_hook.h"

/* Lines of the captured serial output, and lookups in them. */
static int count_lines(void)
{
    int n = 0;
    for (int i = 0; i < W.nout; i++) n += W.out[i] == '\n';
    return n;
}
/* "[app] aim name x y": 1 and *x, *y when `name` was printed (the last one). */
static int find_aim(const char *app, const char *name, int *x, int *y)
{
    char pfx[96];
    int found = 0;
    snprintf(pfx, sizeof pfx, "[%s] aim %s ", app, name);
    for (const char *p = W.out; (p = strstr(p, pfx)); p++)
        if ((p == W.out || p[-1] == '\n') && sscanf(p + strlen(pfx), "%d %d", x, y) == 2) found = 1;
    return found;
}
/* Empty the aim table: the next frame prints every aim it has. */
static void forget(void)
{
    for (int i = 0; i < UI_AIM_SLOTS; i++) ui_aim_g.slot[i].seen = 0;
}
static int count_prefix(const char *app, const char *pfx)
{
    char k[96];
    int n = 0;
    snprintf(k, sizeof k, "[%s] aim %s", app, pfx);
    for (const char *p = W.out; (p = strstr(p, k)); p++) if (p == W.out || p[-1] == '\n') n++;
    return n;
}

/* ------------------------------------------------------------------- lru */
static void case_lru(int unused)
{
    (void)unused;
    char nm[32];
    ui_aim_set_app("t");
    ch_reset();
    for (int i = 0; i < 70; i++) { snprintf(nm, sizeof nm, "ctl%02d", i); ui_aim(nm, i, 2 * i); }
    int n = count_lines(), miss = 0, x, y;
    for (int i = 0; i < 70; i++) {
        snprintf(nm, sizeof nm, "ctl%02d", i);
        if (!find_aim("t", nm, &x, &y) || x != i || y != 2 * i) miss++;
    }
    if (n != 70 || miss) FAIL("aim-lru: %d of 70 distinct names printed (%d missing): a full table dropped names", n, miss);
    /* the most recent 64 are still held: re-aiming them unchanged is silent */
    ch_reset();
    for (int i = 6; i < 70; i++) { snprintf(nm, sizeof nm, "ctl%02d", i); ui_aim(nm, i, 2 * i); }
    if (!bad && count_lines() != 0) FAIL("aim-lru: the 64 most recent names re-printed %d lines", count_lines());
    if (!bad) printf("ok   aim-lru: 70 distinct names printed, the newest 64 held\n");
}

/* ---------------------------------------------------------------- format */
static void case_format(int unused)
{
    (void)unused;
    char buf[32];
    ch_reset();
    ui_aim("early", 1, 2);
    if (W.nout) FAIL("aim-format: printed before ui_aim_set_app: %s", W.out);
    ui_aim_set_app("monitor");
    strcpy(buf, "row-1"); ui_aim(buf, 10, -4);
    strcpy(buf, "row-2"); ui_aim(buf, 10, 22);    /* same buffer, new name */
    strcpy(buf, "row-1"); ui_aim(buf, 10, -4);    /* unchanged: silent */
    if (strcmp(W.out, "[monitor] aim row-1 10 -4\n[monitor] aim row-2 10 22\n"))
        FAIL("aim-format: got \"%s\"", W.out);
    ui_aim_rect("box", 10, 20, 30, 41);
    int x, y;
    if (!find_aim("monitor", "box", &x, &y) || x != 25 || y != 40) FAIL("aim-format: ui_aim_rect centre %d %d, want 25 40", x, y);
    /* a 40-char name is cut to 31 bytes and still matches its caller */
    ch_reset();
    ui_aim("a-very-long-control-name-that-goes-on-and-on", 1, 1);
    ui_aim("a-very-long-control-name-that-goes-on-and-on", 1, 1);
    if (count_lines() != 1) FAIL("aim-format: a cut name printed %d lines", count_lines());
    ui_aim_set_app("");
    ch_reset();
    ui_aim("off", 1, 1);
    if (W.nout) FAIL("aim-format: printed after ui_aim_set_app(\"\")");
    if (!bad) printf("ok   aim-format: one line per change, by value\n");
}
/* one SYS_WRITE per line, never a fragment */
static void case_writes(int unused)
{
    (void)unused;
    ui_aim_set_app("w");
    ch_reset();
    for (int i = 0; i < 10; i++) ui_aim_i("k", i, 1000 * i, -i);
    if (count_lines() != 10 || W.nwrite != 10) FAIL("aim-format: %d lines from %d SYS_WRITE calls, want 10 and 10", count_lines(), W.nwrite);
    else printf("ok   aim-format: one SYS_WRITE per line\n");
}

/* ----------------------------------------------------------------- names */
static void case_names(int unused)
{
    (void)unused;
    ui_aim_set_app("n");
    ch_reset();
    for (int i = 0; i < 50; i++) ui_aim_i("row", i, 0, i);
    char nm[32];
    int x, y, ok = count_lines() == 50;
    for (int i = 0; i < 50 && ok; i++) {
        snprintf(nm, sizeof nm, "row-%d", i);
        if (!find_aim("n", nm, &x, &y) || y != i) ok = 0;
    }
    if (!ok) FAIL("aim-i: ui_aim_i(\"row\", 0..49) did not give 50 distinct row-<i> names (%d lines)", count_lines());
    else printf("ok   aim-i: 50 distinct names\n");
}

/* ------------------------------------------------------------- the scene */
static const char *const TABS[3] = { "进程", "内存", "磁盘" };
static const char *const SEGS[3] = { "全部", "我的", "系统" };
static const char *const SEG_NAMES[3] = { "view-all", "view-mine", "view-system" };
static const char *const COLS[2] = { "名称", "PID" };
static const int WIDTHS[2] = { 200, 80 };
static const char *CELLS[2 * 50];
static char cellbuf[50][2][16];
static const char *const OPTS[4] = { "一", "二", "三", "四" };
static const char *const BTNS[2] = { "好", "取消" };
static int tab_sel, seg_sel, row_sel, row_scroll, opt_sel;
static int with_dialog;
#define TBL_X 20
#define TBL_Y 120
/* aui_tabs(20, 10, 400, ...) centres its capsule: 3 tabs of 24 pt + 4 (design 4.5) */
#define TAB_X0 (20 + (400 - (3 * 24 + 4)) / 2)
#define SEG_W ((300 - 4) / 3)
#define TBL_W 320
#define TBL_H 160
static void scene(void *p)
{
    (void)p;
    aui_aim_prefix("tab");
    aui_tabs(20, 10, 400, TABS, 3, &tab_sel);
    aui_aim_names(SEG_NAMES, 3);
    aui_segmented(20, 60, 300, 28, SEGS, 3, &seg_sel);
    aui_aim_prefix("proc");
    aui_table(TBL_X, TBL_Y, TBL_W, TBL_H, COLS, WIDTHS, 2, CELLS, 50, &row_sel, &row_scroll);
    aui_aim_prefix("sort");
    aui_select(400, 120, 140, OPTS, 4, &opt_sel);
    if (with_dialog) {
        aui_dialog_begin("确认", 300, 140);
        aui_dialog_buttons(BTNS, 2);
        aui_dialog_end();
    }
}
static void scene_init(void)
{
    for (int i = 0; i < 50; i++) {
        snprintf(cellbuf[i][0], 16, "p%d", i); snprintf(cellbuf[i][1], 16, "%d", 100 + i);
        CELLS[2 * i] = cellbuf[i][0]; CELLS[2 * i + 1] = cellbuf[i][1];
    }
    W.pct = 100; W.dark = 0; W.info = LOGIT_GUI2_F_PATH | LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2;
    aui_set_size(800, 600);
    ui_aim_set_app("monitor");
}
static void click(int x, int y)
{
    struct logit_event e = { EV_MOUSE, x, y, 0, EV_BTN_LEFT, 0 };
    aui_feed(&e); core_frame(scene, 0); aui_feed_done();
    struct logit_event u = { EV_MOUSE_UP, x, y, 0, EV_BTN_LEFT, 0 };
    aui_feed(&u); core_frame(scene, 0); aui_feed_done();
}

/* ----------------------------------------------------------------- quiet */
static void case_quiet(int unused)
{
    (void)unused;
    scene_init();
    with_dialog = 1;
    core_frame(scene, 0);
    W.ms += 1000;                                 /* every entrance settles */
    ch_reset(); core_frame(scene, 0);
    int first = count_lines();
    ch_reset(); core_frame(scene, 0);
    if (first == 0) FAIL("aim-quiet: the settled frame printed nothing");
    if (count_lines() != 0) FAIL("aim-quiet: an unchanged frame printed %d lines:\n%s", count_lines(), W.out);
    /* move ONE row: only the rows whose centre moved re-print, nothing else */
    with_dialog = 0;
    row_scroll = 0;
    core_frame(scene, 0);
    ch_reset();
    row_scroll = 3;                                /* every visible row moves 3 pt */
    core_frame(scene, 0);
    if (count_prefix("monitor", "tab-") || count_prefix("monitor", "view-") || count_prefix("monitor", "sort "))
        FAIL("aim-quiet: a scroll re-printed controls that did not move:\n%s", W.out);
    if (!count_prefix("monitor", "proc-")) FAIL("aim-quiet: scrolled rows were not re-printed");
    if (!bad) printf("ok   aim-quiet: %d aims on the settled frame, 0 on the next\n", first);
}

/* ----------------------------------------------------------------- table */
static void case_table(int unused)
{
    (void)unused;
    scene_init();
    for (int pass = 0; pass < 2; pass++) {
        row_scroll = pass ? 7 * 28 + 5 : 0;
        core_frame(scene, 0);
        ch_reset();
        ui_aim_g.tick = 0;
        forget();   /* forget: print all */
        core_frame(scene, 0);
        /* design 4.8 (ui-aui-lists): the body starts below the 28 pt header with
         * no inset and is h - 28 tall; rows are 28 pt */
        int vy0 = TBL_Y + 28, vy1 = vy0 + TBL_H - 28, n = 0, prev_y = 0, prev_i = -1;
        for (int i = 0; i < 50; i++) {
            char nm[16];
            int x, y, cy = vy0 + i * 28 + 14 - row_scroll;
            int vis = cy >= vy0 && cy < vy1;
            snprintf(nm, sizeof nm, "proc-%d", i);
            int got = find_aim("monitor", nm, &x, &y);
            if (got != vis) FAIL("aim-table: row %d (centre %d, viewport %d..%d) %s", i, cy, vy0, vy1, got ? "aimed while hidden" : "visible but not aimed");
            if (got) {
                n++;
                if (y != cy) FAIL("aim-table: row %d at y %d, want its centre %d", i, y, cy);
                if (x < TBL_X || x >= TBL_X + TBL_W) FAIL("aim-table: row %d at x %d, outside the table", i, x);
                if (prev_i == i - 1 && prev_i >= 0 && y - prev_y != 28) FAIL("aim-table: rows %d,%d %d apart, want 28", prev_i, i, y - prev_y);
                prev_y = y; prev_i = i;
            }
        }
        if (!n) FAIL("aim-table: no rows aimed (scroll %d)", row_scroll);
        else printf("ok   aim-table: scroll %d: %d visible rows aimed, hidden ones not\n", row_scroll, n);
    }
}

/* ---------------------------------------------------------------- motion */
static void case_motion(int unused)
{
    (void)unused;
    scene_init();
    core_frame(scene, 0);
    W.ms += 1000;
    core_frame(scene, 0);
    /* click the third segment: the thumb starts to travel */
    ch_reset();
    click(20 + 250, 60 + 14);
    if (seg_sel != 2) FAIL("aim-motion: the click did not select segment 2 (%d)", seg_sel);
    forget();      /* forget: a print is visible */
    W.ms += 40;                                  /* mid-slide (AUI_T_BASE 180 ms) */
    ch_reset(); core_frame(scene, 0);
    if (count_prefix("monitor", "view-")) FAIL("aim-motion: segmented aims printed while the thumb moves:\n%s", W.out);
    if (!count_prefix("monitor", "tab-")) FAIL("aim-motion: the resting tabs were not aimed");
    /* 400 ms of frames: the snappy spring steps at most 250 ms per frame, so one
     * frame after a 400 ms gap does not settle it */
    ch_reset();
    for (int k = 0; k < 4; k++) { W.ms += 100; core_frame(scene, 0); }
    if (count_prefix("monitor", "view-") != 3) FAIL("aim-motion: %d segmented aims after the thumb settled, want 3", count_prefix("monitor", "view-"));
    /* the tab thumb: tabs are a centred segmented capsule (design 4.5,
     * ui-aui-lists); the hook measures text 0 wide, so each tab is 24 pt */
    forget();
    click(TAB_X0 + 2 + 24 * 2 + 12, 10 + 14);
    W.ms += 30;
    ch_reset(); core_frame(scene, 0);
    if (tab_sel != 2) FAIL("aim-motion: the click did not select tab 2 (%d)", tab_sel);
    if (count_prefix("monitor", "tab-")) FAIL("aim-motion: tab aims printed while the thumb moves:\n%s", W.out);
    forget();
    ch_reset();
    for (int k = 0; k < 4; k++) { W.ms += 100; core_frame(scene, 0); }
    if (count_prefix("monitor", "tab-") != 3) FAIL("aim-motion: %d tab aims after the thumb settled, want 3", count_prefix("monitor", "tab-"));
    if (!bad) printf("ok   aim-motion: none while moving, all once settled\n");
}

/* --------------------------------------------------------------- widgets */
static void case_widgets(int unused)
{
    (void)unused;
    int x = 0, y = 0, x2 = 0, y2 = 0;
    scene_init();
    ch_reset();
    core_frame(scene, 0);
    /* the serial a Monitor-shaped frame prints (quoted in the item's README) */
    for (const char *p = W.out; *p; ) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        printf("  serial| %.*s\n", (int)(e - p), p);
        p = e + 1;
    }
    for (int i = 0; i < 3; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "tab-%d", i);
        /* design 4.5 (ui-aui-lists): tab and segment centres inside the 2 pt inset */
        if (!find_aim("monitor", nm, &x, &y) || x != TAB_X0 + 2 + 24 * i + 12 || y != 10 + 14)
            FAIL("aim-widgets: %s at %d %d, want %d %d", nm, x, y, TAB_X0 + 2 + 24 * i + 12, 10 + 14);
        if (!find_aim("monitor", SEG_NAMES[i], &x, &y) || x != 20 + 2 + SEG_W * i + SEG_W / 2 || y != 60 + 14)
            FAIL("aim-widgets: %s at %d %d, want %d 74", SEG_NAMES[i], x, y, 20 + 2 + SEG_W * i + SEG_W / 2);
    }
    if (!find_aim("monitor", "sort", &x, &y) || x != 400 + 70 || y != 120 + 14)
        FAIL("aim-widgets: select control at %d %d, want 470 134", x, y);
    if (count_prefix("monitor", "popup-") || count_prefix("monitor", "dialog-"))
        FAIL("aim-widgets: popup or dialog aims with neither open");
    /* open the select: popup rows once it is shown */
    click(470, 134);
    W.ms += 300;
    forget();
    ch_reset(); core_frame(scene, 0);
    int after = count_prefix("monitor", "popup-");
    if (after != 4) FAIL("aim-widgets: %d popup-<i> aims, want 4:\n%s", after, W.out);
    /* design 4.9 (ui-aui-overlays): menu rows are 24 pt */
    if (!find_aim("monitor", "popup-0", &x, &y) || !find_aim("monitor", "popup-1", &x2, &y2) || y2 - y != 24 || x != x2)
        FAIL("aim-widgets: popup rows %d,%d and %d,%d are not one row apart", x, y, x2, y2);
    opt_sel = 2;
    click(x, y);                                  /* choose popup-0 (closes it) */
    if (opt_sel != 0) FAIL("aim-widgets: clicking popup-0 chose %d", opt_sel);
    W.ms += 300; core_frame(scene, 0);            /* the fade-out finishes */
    /* reopen: this time the popup really fades in, and has no aims meanwhile */
    forget();
    click(470, 134);
    W.ms += 30;
    ch_reset(); core_frame(scene, 0);
    if (count_prefix("monitor", "popup-")) FAIL("aim-widgets: popup aims while it fades in:\n%s", W.out);
    W.ms += 300;
    forget();
    ch_reset(); core_frame(scene, 0);
    if (count_prefix("monitor", "popup-") != 4) FAIL("aim-widgets: %d popup aims after the fade-in", count_prefix("monitor", "popup-"));
    click(x, y);
    /* the dialog: nothing during the entrance, then the four names */
    with_dialog = 1;
    ch_reset(); core_frame(scene, 0);
    W.ms += 60;
    core_frame(scene, 0);
    if (count_prefix("monitor", "dialog-")) FAIL("aim-widgets: dialog aims during the entrance:\n%s", W.out);
    W.ms += 600;
    ch_reset(); core_frame(scene, 0);
    if (!find_aim("monitor", "dialog-0", &x, &y) || !find_aim("monitor", "dialog-confirm", &x2, &y2) || x != x2 || y != y2)
        FAIL("aim-widgets: dialog-confirm is not dialog-0:\n%s", W.out);
    if (!find_aim("monitor", "dialog-1", &x, &y) || !find_aim("monitor", "dialog-cancel", &x2, &y2) || x != x2 || y != y2)
        FAIL("aim-widgets: dialog-cancel is not dialog-1:\n%s", W.out);
    /* design 4.10 (ui-aui-overlays): aui_dialog_buttons puts labels[0], the
     * Primary (Enter's), at the right edge and the last item (Escape's) to its left */
    if (find_aim("monitor", "dialog-0", &x2, &y2) && !(x2 > x && y2 == y))
        FAIL("aim-widgets: confirm (%d,%d) is not right of cancel (%d,%d) on one row", x2, y2, x, y);
    if (!bad) printf("ok   aim-widgets: tabs, segmented, select, popup-<i>, dialog-*\n");
}

int main(void)
{
    core_case("aim-lru", case_lru, 0);
    core_case("aim-format", case_format, 0);
    core_case("aim-format writes", case_writes, 0);
    core_case("aim-i", case_names, 0);
    core_case("aim-quiet", case_quiet, 0);
    core_case("aim-table", case_table, 0);
    core_case("aim-motion", case_motion, 0);
    core_case("aim-widgets", case_widgets, 0);
    if (core_failures) { printf("FAIL ui-aim: %d case(s) failed\n", core_failures); return 1; }
    printf("ok   ui-aim: all cases\n");
    return 0;
}
