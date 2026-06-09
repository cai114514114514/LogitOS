#include "aui.h"

/* Aether Files -- a macOS-Finder-style file manager (ring-3, aui toolkit).
 *
 * Layout: a sectioned SIDEBAR (个人收藏 / 位置) on the left, a TOOLBAR (back +
 * folder title + Grid/List view toggle + New) across the top of the main area,
 * and a content view that is either an ICON GRID (default, vector folder/file
 * icons) or a LIST. Multi-select (plain/shift/ctrl), right-click context menu,
 * in-app clipboard (copy/cut + recursive paste), inline rename / new-folder,
 * Get Info, double-click open. Follows the system light/dark theme via aui.
 *
 * The app owns its own cwd (starts at "/") and always builds absolute paths. */

#define WINW       640
#define WINH       444
#define SIDEBAR_W  168
#define TOOLBAR_H  46
#define CX         SIDEBAR_W          /* content origin x */
#define CY         TOOLBAR_H          /* content origin y */
#define CWID       (WINW - SIDEBAR_W) /* content width  */
#define CHGT       (WINH - TOOLBAR_H) /* content height */
#define GW         104                /* grid cell w */
#define GH         96                 /* grid cell h */
#define GICON      52                 /* grid icon px */
#define LH         28                 /* list row h */
#define N       64
#define PMAX   128

/* --- app state --- */
static char cwd[PMAX] = "/";
static int  view_mode;              /* 0 = icon grid, 1 = list */
static int  sel[N];
static int  sel_count;
static int  anchor = -1;
static int  scroll;                 /* first visible item-row (cell-row in grid) */
static int  rename_mode, newfolder_mode;
static char editbuf[64];
static int  menu_open, menu_x, menu_y;
static char clip[N][PMAX];
static int  clip_count, clip_cut;
static int  shift_down, ctrl_down;
static int  last_click_row = -1, last_click_frame = -1, frame_no;
static int  info_open;
static char info_text[256];

/* --- tiny helpers --- */
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

static void pjoin(char *dst, const char *dir, const char *name, int max)
{
    int i = 0;
    for (; i < max - 2 && dir[i]; i++) dst[i] = dir[i];
    if (i > 0 && dst[i - 1] != '/') dst[i++] = '/';
    for (int j = 0; i < max - 1 && name[j]; i++, j++) dst[i] = name[j];
    dst[i] = 0;
}

static int is_dir(const char *path) { return dir_count(path) >= 0; }

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

/* --- recursive copy / delete --- */
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
    int n = dir_count(src);
    if (make_dir(dst) < 0) { /* may exist */ }
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

static int delete_file_path(const char *p) { return delete_file(p); }
static int delete_tree(const char *path)
{
    if (!is_dir(path)) return delete_file_path(path);
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
    return delete_file_path(path);
}

/* --- selection helpers --- */
static void clear_sel(void) { sel_count = 0; }
static int  in_sel(int row) { for (int i = 0; i < sel_count; i++) if (sel[i] == row) return 1; return 0; }
static void add_sel(int row) { if (in_sel(row) || sel_count >= N) return; sel[sel_count++] = row; }
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

static int row_path(int i, char *out, int max, int *is_dir_out)
{
    char nm[64];
    int sz = dir_name(cwd, i, nm);
    if (sz == -1 || !nm[0]) return -1;
    pjoin(out, cwd, nm, max);
    if (is_dir_out) *is_dir_out = (sz == -2);
    return 0;
}

/* --- navigation --- */
static void reset_view(void) { clear_sel(); scroll = 0; anchor = -1; info_open = 0; }

static void navigate(const char *path) { scpy(cwd, path, PMAX); reset_view(); }

static void go_up(void)
{
    if (streq(cwd, "/")) return;
    int n = slen(cwd);
    if (n > 1 && cwd[n - 1] == '/') n--;
    while (n > 1 && cwd[n - 1] != '/') n--;
    if (n > 1 && cwd[n - 1] == '/') n--;
    if (n < 1) n = 1;
    cwd[n] = 0;
    if (!cwd[0]) { cwd[0] = '/'; cwd[1] = 0; }
    reset_view();
}

static void enter_dir(const char *name)
{
    char np[PMAX];
    pjoin(np, cwd, name, PMAX);
    navigate(np);
}

static void do_open(int row)
{
    char path[PMAX]; int isd;
    if (row_path(row, path, PMAX, &isd) < 0) return;
    if (isd) { char nm[64]; dir_name(cwd, row, nm); enter_dir(nm); }
    else sys_open_path(path);
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
        if (path_under(clip[i], dst)) continue;
        if (clip_cut) sys_rename(clip[i], dst);
        else          copy_tree(clip[i], dst);
    }
    if (clip_cut) clip_count = 0;
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
            char np[PMAX]; pjoin(np, cwd, editbuf, PMAX);
            sys_rename(old, np);
        }
    }
    rename_mode = 0; editbuf[0] = 0; clear_sel(); anchor = -1;
}

static void start_newfolder(void) { editbuf[0] = 0; newfolder_mode = 1; rename_mode = 0; info_open = 0; }

static void commit_newfolder(void)
{
    if (editbuf[0]) { char np[PMAX]; pjoin(np, cwd, editbuf, PMAX); make_dir(np); }
    newfolder_mode = 0; editbuf[0] = 0;
}

static void do_get_info(void)
{
    if (sel_count != 1) { info_open = 0; return; }
    char path[PMAX]; int isd;
    if (row_path(sel[0], path, PMAX, &isd) < 0) { info_open = 0; return; }
    char nm[64]; long sz = dir_name(cwd, sel[0], nm);
    char num[24];
    char *o = info_text; int oi = 0;
    const char *seg;
    seg = "Name: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    for (int i = 0; nm[i] && oi < 200; i++) o[oi++] = nm[i];
    o[oi++] = '\n';
    seg = isd ? "Type: Folder" : "Type: File"; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    o[oi++] = '\n';
    if (isd) {
        seg = "Items: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
        itoa_(dir_count(path), num); for (int i = 0; num[i]; i++) o[oi++] = num[i];
    } else {
        seg = "Size: "; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
        itoa_(sz, num); for (int i = 0; num[i]; i++) o[oi++] = num[i];
        seg = " bytes"; for (int i = 0; seg[i]; i++) o[oi++] = seg[i];
    }
    o[oi] = 0;
    info_open = 1;
}

/* --- context menu --- */
static const char *MENU[] = { "Open", "New Folder", "Rename", "Delete", "Copy", "Cut", "Paste", "Get Info" };
#define MENU_N 8
#define MENU_W 124
#define MENU_IH 24

static void run_menu(int idx)
{
    switch (idx) {
    case 0: if (sel_count == 1) do_open(sel[0]); break;
    case 1: start_newfolder(); break;
    case 2: start_rename(); break;
    case 3: do_delete(); break;
    case 4: do_copy(0); break;
    case 5: do_copy(1); break;
    case 6: do_paste(); break;
    case 7: do_get_info(); break;
    }
}

/* --- icon + color per entry type --- */
static int ext_icon(const char *nm, int isd, unsigned *color)
{
    int dark = aui_is_dark();
    if (isd) { *color = rgb(82, 150, 235); return GICON_FOLDER; }
    int n = slen(nm), d = -1;
    for (int i = 0; i < n; i++) if (nm[i] == '.') d = i;
    const char *e = d >= 0 ? nm + d + 1 : "";
    if (streq(e, "png") || streq(e, "gif") || streq(e, "jpg") || streq(e, "jpeg")) { *color = rgb(80, 190, 160); return GICON_IMAGE; }
    if (streq(e, "as")) { *color = rgb(165, 120, 230); return GICON_CODE; }
    if (streq(e, "aex")) { *color = rgb(235, 145, 90); return GICON_GRID; }
    *color = dark ? rgb(170, 178, 192) : rgb(132, 142, 160);
    return GICON_DOC;
}

/* truncate `s` to fit `maxpx` at 13px, appending ".." if cut */
static void fit_label(const char *s, int maxpx, char *out, int omax)
{
    scpy(out, s, omax);
    if (text_measure_px(out, slen(out), 13, 0) <= maxpx) return;
    int len = slen(out);
    while (len > 1) { out[--len] = 0; if (text_measure_px(out, len, 13, 0) + 8 <= maxpx) break; }
    if (len < omax - 2) { out[len] = '.'; out[len + 1] = '.'; out[len + 2] = 0; }
}

/* --- sidebar model --- */
struct side { const char *label; const char *path; int icon; int header; };
static const struct side SIDE[] = {
    { "个人收藏", 0, 0, 1 },
    { "文稿",    "/docs",  GICON_DOC,    0 },
    { "字体",    "/fonts", GICON_FOLDER, 0 },
    { "位置", 0, 0, 1 },
    { "Aether HD", "/",    GICON_GRID,   0 },
    { "应用程序",  "/",    GICON_TERMINAL, 0 },
};
#define SIDE_N (int)(sizeof SIDE / sizeof SIDE[0])

/* y of sidebar entry i's top (headers and items share the same 28px rhythm) */
static int side_y(int i) { return 14 + i * 28; }

/* --- view geometry --- */
static int grid_cols(void) { int c = (CWID - 16) / GW; return c < 1 ? 1 : c; }
static int grid_x0(void) { int c = grid_cols(); return CX + 8 + ((CWID - 16) - c * GW) / 2; }
static int total_items(void) { int t = dir_count(cwd); return t < 0 ? 0 : t; }
static int item_rows(void) { return view_mode ? total_items() : (total_items() + grid_cols() - 1) / grid_cols(); }
static int visible_rows(void) { return view_mode ? (CHGT - 8) / LH : CHGT / GH; }

static void clamp_scroll(void)
{
    int max = item_rows() - visible_rows();
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

/* --- frame draw --- */
static void draw_sidebar(void)
{
    gui_rect(0, 0, SIDEBAR_W, WINH, AUI_FACE);
    gui_rect(SIDEBAR_W - 1, 0, 1, WINH, AUI_BORDER);
    unsigned selbg = aui_is_dark() ? rgb(58, 70, 92) : rgb(210, 224, 250);
    for (int i = 0; i < SIDE_N; i++) {
        int ty = side_y(i);
        if (SIDE[i].header) { gui_text_run(16, ty + 4, 12, 0, AUI_MUTED, SIDE[i].label, slen(SIDE[i].label)); continue; }
        int active = streq(cwd, SIDE[i].path) && SIDE[i].icon != GICON_TERMINAL;
        if (active) gui_rect(8, ty - 2, SIDEBAR_W - 16, 26, selbg);
        gui_icon(SIDE[i].icon, 16, ty, 18, rgb(86, 152, 236));
        gui_text_run(42, ty + 3, 14, 0, AUI_TEXT, SIDE[i].label, slen(SIDE[i].label));
    }
}

static void draw_toolbar(void)
{
    gui_rect(CX, TOOLBAR_H - 1, CWID, 1, AUI_BORDER);
    if (aui_button(CX + 8, 10, 30, 26, "<")) go_up();

    /* title (folder leaf) or inline rename / new-folder field */
    if (rename_mode) {
        if (aui_textfield(CX + 48, 11, 230, editbuf, sizeof editbuf)) commit_rename();
    } else if (newfolder_mode) {
        if (aui_textfield(CX + 48, 11, 230, editbuf, sizeof editbuf)) commit_newfolder();
    } else {
        char title[64];
        if (streq(cwd, "/")) scpy(title, "Aether HD", sizeof title);
        else leaf_of(cwd, title, sizeof title);
        aui_heading(CX + 48, 12, title, AUI_TEXT);
    }

    /* right side: a single view toggle (shows the mode you'll switch TO) + New */
    int nb = WINW - 8 - 50, vb = nb - 56;
    if (aui_button(vb, 10, 52, 26, view_mode ? "Icons" : "List")) { view_mode = !view_mode; scroll = 0; }
    if (aui_button(nb, 10, 50, 26, "+New")) start_newfolder();
}

static void draw_grid(void)
{
    int total = total_items(), cols = grid_cols(), gx0 = grid_x0();
    unsigned selbg = aui_is_dark() ? rgb(58, 70, 92) : rgb(208, 224, 250);
    gui_clip(CX, CY, CWID, CHGT);
    for (int idx = 0; idx < total; idx++) {
        int cr = idx / cols, cc = idx % cols;
        if (cr < scroll || cr >= scroll + visible_rows() + 1) continue;
        char nm[64]; long sz = dir_name(cwd, idx, nm); int isd = (sz == -2);
        int cellx = gx0 + cc * GW, celly = CY + 6 + (cr - scroll) * GH;
        if (in_sel(idx)) gui_rect(cellx + 6, celly, GW - 12, GH - 8, selbg);
        unsigned col; int icon = ext_icon(nm, isd, &col);
        gui_icon(icon, cellx + (GW - GICON) / 2, celly + 8, GICON, col);
        char lab[40]; fit_label(nm, GW - 14, lab, sizeof lab);
        int lw = text_measure_px(lab, slen(lab), 13, 0);
        gui_text_run(cellx + (GW - lw) / 2, celly + GICON + 16, 13, 0, AUI_TEXT, lab, slen(lab));
    }
    gui_clip(0, 0, 0, 0);
}

static void draw_list(void)
{
    int total = total_items();
    unsigned selbg = aui_is_dark() ? rgb(58, 70, 92) : rgb(208, 224, 250);
    gui_clip(CX, CY, CWID, CHGT);
    int y = CY + 4;
    for (int r = scroll; r < total && r < scroll + visible_rows() + 1; r++, y += LH) {
        char nm[64]; long sz = dir_name(cwd, r, nm); int isd = (sz == -2);
        if (in_sel(r)) gui_rect(CX + 4, y - 2, CWID - 8, LH, selbg);
        unsigned col; int icon = ext_icon(nm, isd, &col);
        gui_icon(icon, CX + 10, y, 20, col);
        char lab[48]; fit_label(nm, CWID - 150, lab, sizeof lab);
        gui_text_run(CX + 38, y + 4, 14, 0, AUI_TEXT, lab, slen(lab));
        if (isd) gui_text_run(WINW - 96, y + 4, 13, 0, AUI_MUTED, "--", 2);
        else { char num[24]; itoa_(sz, num); gui_text_run(WINW - 96, y + 4, 13, 0, AUI_MUTED, num, slen(num)); }
    }
    gui_clip(0, 0, 0, 0);
}

static void frame(void)
{
    aui_begin(AUI_BG);
    /* Chrome first: toolbar buttons (view toggle / back / New) and sidebar set
     * view_mode/cwd THIS frame; the content below then reflects them immediately
     * (the three regions are disjoint, so draw order doesn't affect pixels). */
    draw_toolbar();
    draw_sidebar();
    clamp_scroll();
    if (view_mode) draw_list(); else draw_grid();

    if (info_open) {
        int pw = 248, ph = 104, px = CX + (CWID - pw) / 2, py = CY + (CHGT - ph) / 2;
        gui_rect(px - 1, py - 1, pw + 2, ph + 2, AUI_BORDER);
        gui_rect(px, py, pw, ph, AUI_SURFACE);
        aui_label(px + 12, py + 10, "Get Info", AUI_TEXT);
        int ly = py + 34; char line[128]; int li = 0;
        for (int i = 0; ; i++) {
            char ch = info_text[i];
            if (ch == '\n' || ch == 0) {
                line[li] = 0; aui_label(px + 12, ly, line, AUI_MUTED); ly += 18; li = 0;
                if (ch == 0) break;
            } else if (li < 120) line[li++] = ch;
        }
        if (aui_button(px + pw - 60, py + ph - 28, 50, 22, "OK")) info_open = 0;
    }

    if (menu_open) {
        int mh = MENU_N * MENU_IH + 4, mx = menu_x, my = menu_y;
        if (mx + MENU_W > WINW) mx = WINW - MENU_W;
        if (my + mh > WINH) my = WINH - mh;
        if (mx < 0) mx = 0; if (my < 0) my = 0;
        gui_rect(mx - 1, my - 1, MENU_W + 2, mh + 2, AUI_BORDER);
        gui_rect(mx, my, MENU_W, mh, AUI_SURFACE);
        for (int i = 0; i < MENU_N; i++)
            gui_text_run(mx + 12, my + 6 + i * MENU_IH, 14, 0, AUI_TEXT, MENU[i], slen(MENU[i]));
    }

    aui_end();
}

/* --- hit-testing --- */
static int menu_hit(int x, int y, int *item)
{
    *item = -1;
    if (!menu_open) return 0;
    int mh = MENU_N * MENU_IH + 4, mx = menu_x, my = menu_y;
    if (mx + MENU_W > WINW) mx = WINW - MENU_W;
    if (my + mh > WINH) my = WINH - mh;
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    if (x >= mx && x < mx + MENU_W && y >= my && y < my + mh) {
        int i = (y - my - 4) / MENU_IH;
        if (i >= 0 && i < MENU_N) *item = i;
    }
    return 1;
}

/* map a content click to an item index, or -1 */
static int entry_at(int x, int y)
{
    if (x < CX || y < CY) return -1;
    int total = total_items();
    if (view_mode) {
        int r = scroll + (y - (CY + 2)) / LH;
        return (r >= scroll && r < total) ? r : -1;
    }
    int cols = grid_cols(), gx0 = grid_x0();
    int cc = (x - gx0) / GW;
    if (cc < 0 || cc >= cols || x < gx0) return -1;
    int cr = scroll + (y - (CY + 6)) / GH;
    int idx = cr * cols + cc;
    return (idx >= 0 && idx < total) ? idx : -1;
}

static void sidebar_click(int y)
{
    for (int i = 0; i < SIDE_N; i++) {
        if (SIDE[i].header) continue;
        int ty = side_y(i);
        if (y >= ty - 2 && y < ty + 24) { navigate(SIDE[i].path); return; }
    }
}

static void handle_click(int x, int y)
{
    if (menu_open) { int item; menu_hit(x, y, &item); menu_open = 0; if (item >= 0) run_menu(item); return; }
    if (x < SIDEBAR_W) { sidebar_click(y); return; }
    if (y < TOOLBAR_H) return;                       /* toolbar -> aui buttons */

    int row = entry_at(x, y);
    if (row < 0) { if (!shift_down && !ctrl_down) { clear_sel(); anchor = -1; } return; }
    if (ctrl_down) { toggle_sel(row); anchor = row; }
    else if (shift_down) { select_range(anchor, row); }
    else {
        if (row == last_click_row && frame_no - last_click_frame <= 12) { do_open(row); last_click_row = -1; return; }
        select_one(row);
    }
    last_click_row = row; last_click_frame = frame_no;
}

void app_main(void)
{
    gui_create("Finder", WINW, WINH);
    frame();
    struct aether_event e;
    for (;;) {
        if (!poll_event(&e)) { sys_yield(); continue; }
        frame_no++;
        if (e.type == EV_CLOSE) app_exit(0);

        if (e.type == EV_THEME) { frame(); continue; }   /* system light/dark changed */

        if (e.type == EV_KEY) {
            int k = e.a;
            if (k == KEY_PGUP) { scroll -= visible_rows(); if (scroll < 0) scroll = 0; }
            else if (k == KEY_PGDN) scroll += visible_rows();
            else if (k == KEY_UP) { if (scroll > 0) scroll--; }
            else if (k == KEY_DOWN) scroll++;
            else if (k == 27) { menu_open = 0; info_open = 0; rename_mode = 0; newfolder_mode = 0; }
            aui_feed(&e); frame(); aui_feed_done();
            continue;
        }
        if (e.type == EV_MOUSE_R) { menu_open = 1; menu_x = e.a; menu_y = e.b; info_open = 0; frame(); continue; }
        if (e.type == EV_MOUSE) { handle_click(e.a, e.b); aui_feed(&e); frame(); aui_feed_done(); continue; }
        frame();
    }
}
