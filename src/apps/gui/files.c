#include "aui.h"

/* Aqua Files -- a ring-3 file manager (immediate-mode, aui toolkit).
 *
 * Path bar + Up, a toolbar (New Folder/Rename/Delete/Copy/Cut/Paste), a
 * scrollable list view (icon + name + size), multi-select (plain/shift/ctrl
 * click), a right-click (EV_MOUSE_R) context menu, an in-app clipboard
 * (copy/cut + recursive copy_tree/delete_tree paste), inline rename/new-folder
 * text entry, Get Info, and double-click open-file / enter-folder.
 *
 * The app owns its own cwd (starts at "/") and always builds absolute paths,
 * so the kernel's cwd-relative resolution never surprises us. */

#define WINW   520
#define WINH   420
#define N       64          /* selection + clipboard cap */
#define PMAX   128          /* path buffer size */
#define ROW_H   22
#define LIST_Y  92          /* first list row top */
#define LIST_X  12

/* --- app state --- */
static char cwd[PMAX] = "/";
static int  sel[N];                 /* selected row indices into the current listing */
static int  sel_count;
static int  anchor = -1;            /* shift-select anchor row */
static int  scroll;                 /* first visible row */
static int  rename_mode, newfolder_mode;
static char editbuf[64];
static int  menu_open, menu_x, menu_y;
static char clip[N][PMAX];          /* in-app clipboard: absolute source paths */
static int  clip_count, clip_cut;   /* clip_cut: 1 = move on paste, 0 = copy */
static int  shift_down, ctrl_down;
static int  last_click_row = -1, last_click_frame = -1, frame_no;
static int  info_open;              /* Get Info panel showing */
static char info_text[256];

/* --- tiny helpers (GUI apps use aqua.h, not clib.h) --- */
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int  streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scpy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static void itoa_(long v, char *b)
{
    char t[24]; int n = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    int p = 0; if (neg) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

/* Join dir + name into dst, inserting a single '/' (mirrors clib.h path_join). */
static void pjoin(char *dst, const char *dir, const char *name, int max)
{
    int i = 0;
    for (; i < max - 2 && dir[i]; i++) dst[i] = dir[i];
    if (i > 0 && dst[i - 1] != '/') dst[i++] = '/';
    for (int j = 0; i < max - 1 && name[j]; i++, j++) dst[i] = name[j];
    dst[i] = 0;
}

static int is_dir(const char *path) { return dir_count(path) >= 0; }

/* 1 iff absolute path `b` equals `a` or is nested under it (a is an ancestor).
 * Used to refuse copy/move of a folder into itself or a descendant. */
static int path_under(const char *a, const char *b)
{
    int la = 0; while (a[la]) la++;
    while (la > 1 && a[la - 1] == '/') la--;
    int lb = 0; while (b[lb]) lb++;
    while (lb > 1 && b[lb - 1] == '/') lb--;
    if (lb < la) return 0;
    for (int i = 0; i < la; i++) if (a[i] != b[i]) return 0;
    return lb == la || b[la] == '/';
}

/* --- recursive copy / delete (userland, via the fd + dir API) --- */

static int copy_file(const char *src, const char *dst)
{
    int rf = sys_open(src, O_RDONLY);
    if (rf < 0) return -1;
    int wf = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (wf < 0) { sys_close(rf); return -1; }
    char buf[4096];
    int rc = 0;
    for (;;) {
        int n = sys_read(rf, buf, sizeof buf);
        if (n <= 0) break;
        if (sys_write(wf, buf, n) != n) { rc = -1; break; }
    }
    sys_close(rf); sys_close(wf);
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    if (!is_dir(src)) return copy_file(src, dst);
    int n = dir_count(src);             /* capture BEFORE make_dir: if dst were under src,
                                         * creating dst would inflate src's listing -> runaway */
    if (make_dir(dst) < 0) { /* may already exist; continue regardless */ }
    for (int i = 0; i < n; i++) {
        char nm[64];
        if (dir_name(src, i, nm) < 0 || !nm[0] || streq(nm, ".") || streq(nm, "..")) continue;
        char cs[PMAX], cd[PMAX];
        pjoin(cs, src, nm, PMAX);
        pjoin(cd, dst, nm, PMAX);
        copy_tree(cs, cd);
    }
    return 0;
}

/* Delete a tree leaf-first. Deleting a dirent tombstones it, which shifts the
 * LIVE index of every later entry down by one -- so a captured-once `for i<n`
 * loop skips ~half the children. Instead always delete the FIRST live entry
 * until the directory is empty; `prev` breaks the loop if a child won't delete
 * (no progress), so a stuck entry can't spin forever. */
static int delete_tree(const char *path)
{
    if (!is_dir(path)) return delete_file(path);
    char nm[64], child[PMAX];
    int prev = -1;
    for (;;) {
        int n = dir_count(path);
        if (n <= 0 || n == prev) break;
        prev = n;
        if (dir_name(path, 0, nm) < 0) break;
        pjoin(child, path, nm, PMAX);
        delete_tree(child);
    }
    return delete_file(path);          /* the (now empty) directory itself */
}

/* --- selection helpers --- */
static void clear_sel(void) { sel_count = 0; }
static int  in_sel(int row) { for (int i = 0; i < sel_count; i++) if (sel[i] == row) return 1; return 0; }
static void add_sel(int row)
{
    if (in_sel(row) || sel_count >= N) return;
    sel[sel_count++] = row;
}
static void toggle_sel(int row)
{
    for (int i = 0; i < sel_count; i++)
        if (sel[i] == row) { sel[i] = sel[sel_count - 1]; sel_count--; return; }
    add_sel(row);
}
static void select_one(int row) { clear_sel(); add_sel(row); anchor = row; }
static void select_range(int from, int to)
{
    clear_sel();
    if (from < 0) from = to;
    int lo = from < to ? from : to, hi = from < to ? to : from;
    for (int r = lo; r <= hi; r++) add_sel(r);
}

/* Build absolute path for listing row `i` into out. Returns 0 ok, -1 bad. */
static int row_path(int i, char *out, int max, int *is_dir_out)
{
    char nm[64];
    int sz = dir_name(cwd, i, nm);
    if (sz == -1 || !nm[0]) return -1;
    pjoin(out, cwd, nm, max);
    if (is_dir_out) *is_dir_out = (sz == -2);
    return 0;
}

/* --- actions --- */

static void go_up(void)
{
    if (streq(cwd, "/")) return;
    int n = slen(cwd);
    if (n > 1 && cwd[n - 1] == '/') n--;          /* trailing slash */
    while (n > 1 && cwd[n - 1] != '/') n--;        /* strip leaf */
    if (n > 1 && cwd[n - 1] == '/') n--;           /* drop the slash, unless root */
    if (n < 1) n = 1;
    cwd[n] = 0;
    if (!cwd[0]) { cwd[0] = '/'; cwd[1] = 0; }
    clear_sel(); scroll = 0; anchor = -1; info_open = 0;
}

static void enter_dir(const char *name)
{
    char np[PMAX];
    pjoin(np, cwd, name, PMAX);
    scpy(cwd, np, PMAX);
    clear_sel(); scroll = 0; anchor = -1; info_open = 0;
}

static void do_open(int row)
{
    char path[PMAX]; int isd;
    if (row_path(row, path, PMAX, &isd) < 0) return;
    if (isd) {
        char nm[64]; dir_name(cwd, row, nm);
        enter_dir(nm);
    } else {
        sys_open_path(path);
    }
}

static void do_delete(void)
{
    for (int i = 0; i < sel_count; i++) {
        char path[PMAX]; int isd;
        if (row_path(sel[i], path, PMAX, &isd) < 0) continue;
        delete_tree(path);
    }
    clear_sel(); anchor = -1; info_open = 0;
}

static void do_copy(int cut)
{
    clip_count = 0; clip_cut = cut;
    for (int i = 0; i < sel_count && clip_count < N; i++) {
        char path[PMAX]; int isd;
        if (row_path(sel[i], path, PMAX, &isd) < 0) continue;
        scpy(clip[clip_count++], path, PMAX);
    }
}

/* extract the leaf name of an absolute path into out */
static void leaf_of(const char *path, char *out, int max)
{
    int n = slen(path), e = n;
    if (e > 1 && path[e - 1] == '/') e--;
    int s = e;
    while (s > 0 && path[s - 1] != '/') s--;
    int j = 0;
    for (; s < e && j < max - 1; s++) out[j++] = path[s];
    out[j] = 0;
}

static void do_paste(void)
{
    for (int i = 0; i < clip_count; i++) {
        char leaf[64];
        leaf_of(clip[i], leaf, sizeof leaf);
        if (!leaf[0]) continue;
        char dst[PMAX];
        pjoin(dst, cwd, leaf, PMAX);
        /* skip if the destination is the source itself or nested under it --
         * otherwise copy_tree would recurse into the copy it just made. */
        if (path_under(clip[i], dst)) continue;
        if (clip_cut) sys_rename(clip[i], dst);
        else          copy_tree(clip[i], dst);
    }
    if (clip_cut) clip_count = 0;                    /* a cut item moves once */
    clear_sel(); anchor = -1;
}

static void start_rename(void)
{
    if (sel_count != 1) return;
    char nm[64];
    dir_name(cwd, sel[0], nm);
    scpy(editbuf, nm, sizeof editbuf);
    rename_mode = 1; newfolder_mode = 0; info_open = 0;
}

static void commit_rename(void)
{
    if (editbuf[0] && sel_count == 1) {
        char old[PMAX]; int isd;
        if (row_path(sel[0], old, PMAX, &isd) == 0) {
            char np[PMAX];
            pjoin(np, cwd, editbuf, PMAX);
            sys_rename(old, np);
        }
    }
    rename_mode = 0; editbuf[0] = 0; clear_sel(); anchor = -1;
}

static void start_newfolder(void)
{
    editbuf[0] = 0;
    newfolder_mode = 1; rename_mode = 0; info_open = 0;
}

static void commit_newfolder(void)
{
    if (editbuf[0]) {
        char np[PMAX];
        pjoin(np, cwd, editbuf, PMAX);
        make_dir(np);
    }
    newfolder_mode = 0; editbuf[0] = 0;
}

static void do_get_info(void)
{
    if (sel_count != 1) { info_open = 0; return; }
    char path[PMAX]; int isd;
    if (row_path(sel[0], path, PMAX, &isd) < 0) { info_open = 0; return; }
    char nm[64]; long sz = dir_name(cwd, sel[0], nm);
    char num[24];
    /* compose: Name / Type / Size (+ item count for dirs) */
    char *o = info_text; int oi = 0;
    const char *seg;
    seg = "Name: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    for (int i = 0; nm[i] && oi < 200; i++) o[oi++] = nm[i];
    o[oi++] = '\n';
    seg = isd ? "Type: Folder" : "Type: File"; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    o[oi++] = '\n';
    if (isd) {
        seg = "Items: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
        itoa_(dir_count(path), num);
        for (int i = 0; num[i]; i++) o[oi++] = num[i];
    } else {
        seg = "Size: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
        itoa_(sz, num);
        for (int i = 0; num[i]; i++) o[oi++] = num[i];
        seg = " bytes"; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    }
    o[oi] = 0;
    info_open = 1;
}

/* --- context menu --- */
static const char *MENU[] = { "Open", "New Folder", "Rename", "Delete",
                              "Copy", "Cut", "Paste", "Get Info" };
#define MENU_N 8
#define MENU_W 120
#define MENU_IH 22

static void run_menu(int idx)
{
    switch (idx) {
    case 0: if (sel_count == 1) do_open(sel[0]); break;   /* Open */
    case 1: start_newfolder(); break;                     /* New Folder */
    case 2: start_rename(); break;                        /* Rename */
    case 3: do_delete(); break;                           /* Delete */
    case 4: do_copy(0); break;                            /* Copy */
    case 5: do_copy(1); break;                            /* Cut */
    case 6: do_paste(); break;                            /* Paste */
    case 7: do_get_info(); break;                         /* Get Info */
    }
}

/* --- frame draw --- */

static int visible_rows(void) { return (WINH - LIST_Y - 8) / ROW_H; }

static void frame(void)
{
    aui_begin(AUI_BG);

    /* path bar */
    gui_rect(0, 0, WINW, 34, AUI_FACE);
    if (aui_button(LIST_X, 6, 48, 22, "Up")) go_up();
    aui_label(70, 10, cwd, AUI_TEXT);

    /* toolbar */
    int bx = LIST_X, by = 40, bw = 78, bh = 24, gap = 4;
    if (aui_button(bx, by, bw, bh, "New Folder")) { start_newfolder(); }
    bx += bw + gap;
    if (aui_button(bx, by, 64, bh, "Rename")) { start_rename(); }
    bx += 64 + gap;
    if (aui_button(bx, by, 60, bh, "Delete")) { do_delete(); }
    bx += 60 + gap;
    if (aui_button(bx, by, 52, bh, "Copy")) { do_copy(0); }
    bx += 52 + gap;
    if (aui_button(bx, by, 48, bh, "Cut")) { do_copy(1); }
    bx += 48 + gap;
    if (aui_button(bx, by, 56, bh, "Paste")) { do_paste(); }

    /* inline rename / new-folder field */
    if (rename_mode) {
        aui_label(LIST_X, 70, "Rename:", AUI_MUTED);
        if (aui_textfield(70, 66, 200, editbuf, sizeof editbuf)) commit_rename();
    } else if (newfolder_mode) {
        aui_label(LIST_X, 70, "New Folder:", AUI_MUTED);
        if (aui_textfield(96, 66, 200, editbuf, sizeof editbuf)) commit_newfolder();
    } else {
        aui_label(LIST_X, 70, "Name", AUI_MUTED);
        aui_label(WINW - 110, 70, "Size", AUI_MUTED);
    }

    /* list view */
    int total = dir_count(cwd);
    if (total < 0) total = 0;
    int vis = visible_rows();
    if (scroll > total - vis) scroll = total - vis;
    if (scroll < 0) scroll = 0;

    int y = LIST_Y;
    for (int r = scroll; r < total && r < scroll + vis; r++, y += ROW_H) {
        char nm[64];
        long sz = dir_name(cwd, r, nm);
        int isd = (sz == -2);
        if (in_sel(r)) gui_rect(0, y - 2, WINW, ROW_H, AUI_ACCENT);
        unsigned fg = in_sel(r) ? rgb(255, 255, 255) : AUI_TEXT;
        /* type marker */
        aui_label(LIST_X, y, isd ? "[D]" : "   ", isd ? (in_sel(r) ? fg : AUI_ACCENT) : fg);
        aui_label(LIST_X + 30, y, nm, fg);
        /* size column */
        if (isd) {
            aui_label(WINW - 110, y, "--", in_sel(r) ? fg : AUI_MUTED);
        } else {
            char num[24]; itoa_(sz, num);
            aui_label(WINW - 110, y, num, in_sel(r) ? fg : AUI_MUTED);
        }
    }

    /* scroll hint */
    if (total > vis) {
        char sb[48];
        char a[12], b[12], c[12];
        itoa_(scroll + 1, a); itoa_(scroll + vis < total ? scroll + vis : total, b); itoa_(total, c);
        int i = 0; const char *p;
        p = a; while (*p) sb[i++] = *p++;
        sb[i++] = '-';
        p = b; while (*p) sb[i++] = *p++;
        sb[i++] = ' '; sb[i++] = 'o'; sb[i++] = 'f'; sb[i++] = ' ';
        p = c; while (*p) sb[i++] = *p++;
        sb[i] = 0;
        aui_label(WINW - 130, WINH - 18, sb, AUI_MUTED);
    }

    /* Get Info panel */
    if (info_open) {
        int pw = 240, ph = 96, px = (WINW - pw) / 2, py = (WINH - ph) / 2;
        gui_rect(px - 2, py - 2, pw + 4, ph + 4, rgb(120, 124, 134));
        gui_rect(px, py, pw, ph, rgb(252, 252, 254));
        aui_label(px + 10, py + 8, "Get Info", AUI_TEXT);
        /* render info_text line by line */
        int ly = py + 30; char line[128]; int li = 0;
        for (int i = 0; ; i++) {
            char ch = info_text[i];
            if (ch == '\n' || ch == 0) {
                line[li] = 0;
                aui_label(px + 10, ly, line, AUI_MUTED);
                ly += 18; li = 0;
                if (ch == 0) break;
            } else if (li < 120) line[li++] = ch;
        }
        if (aui_button(px + pw - 60, py + ph - 26, 50, 20, "OK")) info_open = 0;
    }

    /* context menu drawn last (on top) */
    if (menu_open) {
        int mh = MENU_N * MENU_IH + 4;
        int mx = menu_x, my = menu_y;
        if (mx + MENU_W > WINW) mx = WINW - MENU_W;
        if (my + mh > WINH) my = WINH - mh;
        if (mx < 0) mx = 0; if (my < 0) my = 0;
        gui_rect(mx - 1, my - 1, MENU_W + 2, mh + 2, rgb(120, 124, 134));
        gui_rect(mx, my, MENU_W, mh, rgb(250, 250, 252));
        for (int i = 0; i < MENU_N; i++) {
            int iy = my + 2 + i * MENU_IH;
            gui_text_run(mx + 10, iy + 4, 15, 0, AUI_TEXT, MENU[i], slen(MENU[i]));
        }
    }

    aui_end();
}

/* hit-test a click against the context menu; returns 1 if consumed (menu was
 * open), filling *item with the chosen index or -1 if clicked outside. */
static int menu_hit(int x, int y, int *item)
{
    *item = -1;
    if (!menu_open) return 0;
    int mh = MENU_N * MENU_IH + 4;
    int mx = menu_x, my = menu_y;
    if (mx + MENU_W > WINW) mx = WINW - MENU_W;
    if (my + mh > WINH) my = WINH - mh;
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    if (x >= mx && x < mx + MENU_W && y >= my && y < my + mh) {
        int i = (y - my - 2) / MENU_IH;
        if (i >= 0 && i < MENU_N) *item = i;
    }
    return 1;
}

/* map a content-area y to a listing row, or -1 */
static int row_at(int y)
{
    if (y < LIST_Y - 2) return -1;
    int r = scroll + (y - (LIST_Y - 2)) / ROW_H;
    int total = dir_count(cwd);
    if (total < 0) total = 0;
    if (r < scroll || r >= total) return -1;
    return r;
}

static void handle_click(int x, int y)
{
    /* if a menu is open, route the click through it first */
    if (menu_open) {
        int item;
        menu_hit(x, y, &item);
        menu_open = 0;
        if (item >= 0) run_menu(item);
        return;
    }

    int row = row_at(y);
    if (row < 0) { if (!shift_down && !ctrl_down) { clear_sel(); anchor = -1; } return; }

    if (ctrl_down) {
        toggle_sel(row);
        anchor = row;
    } else if (shift_down) {
        select_range(anchor, row);
    } else {
        /* double-click detection: same row within a few frames */
        if (row == last_click_row && frame_no - last_click_frame <= 12) {
            do_open(row);
            last_click_row = -1;
            return;
        }
        select_one(row);
    }
    last_click_row = row;
    last_click_frame = frame_no;
}

void app_main(void)
{
    gui_create("Finder", WINW, WINH);
    frame();

    struct aqua_event e;
    for (;;) {
        if (!poll_event(&e)) { sys_yield(); continue; }
        frame_no++;

        if (e.type == EV_CLOSE) app_exit(0);

        if (e.type == EV_KEY) {
            int k = e.a;
            /* modifier tracking is best-effort; most builds don't deliver shift/
             * ctrl as standalone keys, so these stay 0 and we degrade to single
             * select (per spec, acceptable). */
            if (k == KEY_PGUP) { scroll -= visible_rows(); if (scroll < 0) scroll = 0; }
            else if (k == KEY_PGDN) { scroll += visible_rows(); }
            else if (k == KEY_UP) { if (scroll > 0) scroll--; }
            else if (k == KEY_DOWN) { scroll++; }
            else if (k == 27) { /* ESC closes overlays */
                menu_open = 0; info_open = 0; rename_mode = 0; newfolder_mode = 0;
            }
            /* feed printable keys + Enter/backspace to the active textfield */
            aui_feed(&e); frame(); aui_feed_done();
            continue;
        }

        if (e.type == EV_MOUSE_R) {
            menu_open = 1; menu_x = e.a; menu_y = e.b;
            info_open = 0;
            frame();
            continue;
        }

        if (e.type == EV_MOUSE) {
            /* selection + menu + double-click are handled by the app; toolbar
             * buttons + the inline textfield are handled by aui. Run our hit-test
             * first (it consumes nothing aui needs), then let aui see the event. */
            handle_click(e.a, e.b);
            aui_feed(&e); frame(); aui_feed_done();
            continue;
        }

        frame();
    }
}
