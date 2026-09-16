#include "aui.h"
#include "hidden.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../lib/agent/sdk.h"

/* Logit Files -- a macOS-Finder-style file manager (ring-3, aui toolkit).
 *
 * Layout: a sectioned SIDEBAR (我的 Project / 系统位置 / 位置) on the left, a
 * TOOLBAR (back + folder title + Grid/List toggle + New) across the top of the
 * main area, a content view that is either an ICON GRID or a LIST, and the
 * Project pane on the right. Multi-select (plain/shift/ctrl), right-click
 * context menu, in-app clipboard (copy/cut + recursive paste), inline rename /
 * new-folder, Get Info, double-click open. Follows the system theme via aui.
 *
 * The app owns its own cwd (starts at "/docs") and always builds absolute paths.
 *
 * THE FILE IS ORGANISED IN ONE DIRECTION: helpers, then the MODEL (one snapshot
 * of cwd), then the SELECTION (names), then GEOMETRY (where item i is), then
 * the filesystem commands, then drawing, then the event loop. Nothing earlier
 * calls anything later. Three things that used to be spread across the file
 * are now each in exactly one place, and each of the three was a live bug:
 *
 *   - the listing        was a row->kernel-index map plus dir_name() re-issued
 *                        from both draw loops and row_path(), so a 28-entry
 *                        directory cost ~56 syscalls per frame to learn the
 *                        same 28 answers twice. It is ent[] now, filled once.
 *   - the selection      was row NUMBERS into a listing rebuilt every frame.
 *                        See the section comment; it is names now.
 *   - item geometry      was computed by the draw pass and computed AGAIN,
 *                        differently, by the hit test. item_rect() is the only
 *                        place that answers it.
 */

#define WINW       1120
#define PROJECT_W  320
#define CONTENT_RIGHT (WINW - PROJECT_W)
#define WINH       660
#define SIDEBAR_W  168
#define TOOLBAR_H  46
#define FOOTER_H   34
#define CX         SIDEBAR_W          /* content origin x */
#define CY         TOOLBAR_H          /* content origin y */
#define CWID       (CONTENT_RIGHT - SIDEBAR_W) /* content width  */
#define CHGT       (WINH - TOOLBAR_H - FOOTER_H) /* content height */
#define GW         104                /* grid cell w */
#define GH         96                 /* grid cell h */
#define GICON      52                 /* grid icon px */
#define LH         28                 /* list row h */
#define N       64
#define PMAX   128

/* SYS_DIR_NAME writes at most 64 bytes AND the kernel checks the buffer is
 * exactly that big before it writes anything -- user_range_ok(rdx, 64, 1) at
 * c/kernel/exec/syscall.c:609. A 32-byte name buffer here does not truncate a
 * long name, it makes the call FAIL. This is an ABI constant, not a taste. */
#define ENAME 64

/* --- tiny helpers (nothing below them may be called from above) --- */
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int  streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scpy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static int hit(int px, int py, int x, int y, int w, int h)
{ return px >= x && px < x + w && py >= y && py < y + h; }

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

static int ends_with(const char *s, const char *suf)
{
    int n = slen(s), m = slen(suf);
    return n > m && streq(s + n - m, suf);
}

/* --- app state --- */
static char cwd[PMAX] = "/docs";
static int  view_mode = 1;              /* 0 = icon grid, 1 = list */
static int  scroll;                     /* first visible ROW (see rows_view) */
static int  rename_mode, newfolder_mode;
static char editbuf[64];
static int  menu_open, menu_x, menu_y;
static char clip[N][PMAX];
static int  clip_count, clip_cut;
static uint64_t last_click_ms;
static char last_click_name[ENAME];
static int  info_open;
static char info_text[256];
static char agent_notice[96];
static int  show_hidden;            /* Ctrl+H; per-session, deliberately not persisted */
static int  apps_view;              /* the 应用程序 location -- see entry_visible */

/* Modifier state for the event being handled, refreshed from e.mods at the top
 * of the loop. These were two file-scope ints that NOTHING EVER ASSIGNED, so
 * Ctrl+H, ctrl-click and shift-range were dead code that reads like working
 * code: every branch that implements them is present and none of them could
 * ever be taken. Other apps in this tree (textedit.c, studio.c) had been
 * reading e.mods the whole time. */
static int  shift_held, ctrl_held;

/* --- THE MODEL: one snapshot of cwd ------------------------------------------
 * Taken once per frame by scan_dir(), and it is the ONLY caller of dir_name().
 * Everything below reads ent[]: the draw pass, the hit test, Get Info, every
 * command. Two properties follow that the previous shape did not have -- one
 * listing per frame instead of three, and one definition of "is a directory"
 * (dir_name's -2) instead of two (that, and dir_count(path) >= 0 in is_dir()).
 *
 * The snapshot is rebuilt per frame rather than on navigation, and that part is
 * unchanged and deliberate: entries appear and vanish underneath this app --
 * another process writes a file, a paste lands, a delete shifts everything down
 * one -- and a listing refreshed only on cd would draw rows that are not there. */
struct entry { char name[ENAME]; long size; int is_dir; };
static struct entry ent[N];
static int nent;
static int ndropped;                /* entries past N -- counted, then SHOWN */

static int entry_visible(const char *nm, int is_dir)
{
    /* The 应用程序 location is the one view with an inclusive rule rather than
     * an exclusive one, and it exists because hidden.h now hides every .aex at
     * the root: without this the sidebar entry navigated to "/" and showed a
     * directory containing no applications at all. A sidebar row that provably
     * shows nothing is worse than no sidebar row. */
    if (apps_view) return !is_dir && ends_with(nm, ".aex");
    if (show_hidden) return 1;
    if (hidden_dotfile(nm)) return 0;
    if (hidden_system(cwd, nm, is_dir)) return 0;
    return 1;
}

static void scan_dir(void)
{
    nent = 0; ndropped = 0;
    int n = dir_count(cwd);
    for (int i = 0; i < n; i++) {
        char nm[ENAME]; nm[0] = 0;
        long sz = dir_name(cwd, i, nm);
        if (sz == -1 || !nm[0]) continue;
        if (!entry_visible(nm, sz == -2)) continue;
        if (nent >= N) { ndropped++; continue; }
        scpy(ent[nent].name, nm, ENAME);
        ent[nent].is_dir = (sz == -2);
        ent[nent].size = ent[nent].is_dir ? 0 : sz;
        nent++;
    }
}

static int find_ent(const char *name)
{
    for (int i = 0; i < nent; i++) if (streq(ent[i].name, name)) return i;
    return -1;
}

/* --- THE SELECTION: names, not row numbers -----------------------------------
 * This is the whole reason the previous version needed a workaround at every
 * command. A row number indexes a listing rebuilt every frame from a directory
 * other processes write to, so it names a DIFFERENT FILE the moment anything is
 * created, deleted or renamed. The old code knew: do_delete snapshotted every
 * selected path into 8 KB of static before touching anything, do_paste and
 * commit_rename each ended with clear_sel(), Ctrl+H threw the selection away
 * with a comment explaining that its indices had stopped meaning anything, and
 * the listing's own comment argued the case at length. Four patches, one broken
 * representation.
 *
 * A name is stable under exactly the events that move a row. Deleting a file
 * does not shift anyone else's identity, it just stops matching -- which is
 * what "the selected file is gone" ought to mean. So: no snapshot buffer, and
 * Ctrl+H no longer discards the selection, because hiding a row changes what is
 * DRAWN and not what is SELECTED.
 *
 * What a name is NOT stable under is leaving the directory, and navigate() is
 * the one place that clears it. */
static char sel[N][ENAME];
static int  sel_count;
static char anchor[ENAME];              /* shift-range origin; empty = none */

static void sel_clear(void) { sel_count = 0; anchor[0] = 0; }
static int  sel_has(const char *name)
{
    for (int i = 0; i < sel_count; i++) if (streq(sel[i], name)) return 1;
    return 0;
}
static void sel_add(const char *name)
{
    if (sel_has(name) || sel_count >= N) return;
    scpy(sel[sel_count++], name, ENAME);
}
static void sel_toggle(const char *name)
{
    for (int i = 0; i < sel_count; i++)
        if (streq(sel[i], name)) { scpy(sel[i], sel[sel_count - 1], ENAME); sel_count--; return; }
    sel_add(name);
}
static void sel_only(const char *name)
{
    sel_count = 0; sel_add(name); scpy(anchor, name, ENAME);
}
/* Both ends are resolved against the CURRENT snapshot, which is the one that
 * was drawn when the click happened -- so the range is the rows the user saw
 * between the two rows they clicked, whatever has changed on disk since. */
static void sel_range(int to_idx)
{
    int from = anchor[0] ? find_ent(anchor) : -1;
    if (from < 0) from = to_idx;
    sel_count = 0;
    int lo = from < to_idx ? from : to_idx, hi = from < to_idx ? to_idx : from;
    for (int i = lo; i <= hi && i < nent; i++) if (i >= 0) sel_add(ent[i].name);
}

/* Absolute path of selection slot i. Its directory is cwd because navigate()
 * clears the selection -- a selected name never outlives the directory it came
 * from, which is what lets this be a join instead of a stored path. */
static int sel_path(int i, char *out, int max)
{
    if (i < 0 || i >= sel_count) return -1;
    if (slen(cwd) + 1 + slen(sel[i]) + 1 > max) return -1;
    pjoin(out, cwd, sel[i], max);
    return 0;
}

static int item_path(int idx, char *out, int max)
{
    if (idx < 0 || idx >= nent) return -1;
    if (slen(cwd) + 1 + slen(ent[idx].name) + 1 > max) return -1;
    pjoin(out, cwd, ent[idx].name, max);
    return 0;
}

/* --- GEOMETRY: the one place that says where item i is -----------------------
 * draw_items() and entry_at() both go through item_rect(). They used to compute
 * it separately and disagree: the list drew row r at CY+4+r*LH and hit-tested
 * it at CY+2+r*LH, so the top 2px of every row selected the row above it, and
 * in the grid the gaps between cells were dead. A view that is drawn at one
 * place and clicked at another is not a layout bug, it is two layouts.
 *
 * view_mode now appears in exactly two expressions -- cols_() and row_h() --
 * instead of being branched on in six functions. */
static int grid_cols(void) { int c = (CWID - 16) / GW; return c < 1 ? 1 : c; }
static int grid_x0(void) { int c = grid_cols(); return CX + 8 + ((CWID - 16) - c * GW) / 2; }

static int cols_(void)      { return view_mode ? 1 : grid_cols(); }
static int row_h(void)      { return view_mode ? LH : GH; }
static int rows_total(void) { return (nent + cols_() - 1) / cols_(); }
static int rows_view(void)  { return (CHGT - 8) / row_h(); }

/* The item's HIT rectangle -- the whole row, or the whole grid cell. The
 * highlight is this inset (see draw_items); the hit test is not, because the
 * space between two cells belongs to one of them. Returns 0 when item i is
 * scrolled out of view. */
static int item_rect(int i, int *rx, int *ry, int *rw, int *rh)
{
    if (i < 0 || i >= nent) return 0;
    int c = cols_(), r = i / c - scroll, cc = i % c;
    if (r < 0 || r > rows_view()) return 0;
    if (view_mode) { *rx = CX + 4;                *rw = CWID - 8; }
    else           { *rx = grid_x0() + cc * GW;   *rw = GW; }
    *ry = CY + 2 + r * row_h();
    *rh = row_h();
    return 1;
}

static int entry_at(int x, int y)
{
    if (x < CX || x >= CONTENT_RIGHT || y < CY || y >= WINH - FOOTER_H) return -1;
    for (int i = 0; i < nent; i++) {
        int rx, ry, rw, rh;
        if (item_rect(i, &rx, &ry, &rw, &rh) && hit(x, y, rx, ry, rw, rh)) return i;
    }
    return -1;
}

static void clamp_scroll(void)
{
    int max = rows_total() - rows_view();
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

/* --- filesystem helpers --- */
static int is_dir_path(const char *path) { return dir_count(path) >= 0; }

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

#define TREE_DEPTH_MAX 32               /* ring-3 stack guard for deep directory trees */

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

static int copy_tree(const char *src, const char *dst, int depth)
{
    if (depth > TREE_DEPTH_MAX) return -1;
    if (!is_dir_path(src)) return copy_file(src, dst);
    int n = dir_count(src);
    if (make_dir(dst) < 0) { /* may exist */ }
    for (int i = 0; i < n; i++) {
        char nm[ENAME];
        if (dir_name(src, i, nm) == -1 || !nm[0] || streq(nm, ".") || streq(nm, "..")) continue;
        char cs[PMAX], cd[PMAX];
        pjoin(cs, src, nm, PMAX);
        pjoin(cd, dst, nm, PMAX);
        copy_tree(cs, cd, depth + 1);
    }
    return 0;
}

static int delete_tree(const char *path, int depth)
{
    if (!is_dir_path(path)) return delete_file(path);
    if (depth > TREE_DEPTH_MAX) return -1;
    char nm[ENAME], child[PMAX];
    int prev = -1;
    for (;;) {
        int n = dir_count(path);
        if (n <= 0 || n == prev) break;
        prev = n;
        if (dir_name(path, 0, nm) == -1) break;
        pjoin(child, path, nm, PMAX);
        delete_tree(child, depth + 1);
    }
    return delete_file(path);
}

/* --- navigation --- */
static void reset_view(void) { sel_clear(); scroll = 0; last_click_name[0] = 0; info_open = 0; }

static void navigate(const char *path)
{
    apps_view = 0;
    scpy(cwd, path, PMAX);
    reset_view();
}

static void go_up(void)
{
    if (apps_view) { apps_view = 0; reset_view(); return; }
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

/* --- commands. Every one of them reads sel[] or ent[] and nothing else. --- */
static void do_open(int idx)
{
    if (idx < 0 || idx >= nent) return;
    char path[PMAX];
    if (item_path(idx, path, PMAX) < 0) return;
    if (ent[idx].is_dir) navigate(path);
    else sys_open_path(path);
}

static void do_delete(void)
{
    /* No path snapshot. sel[] holds names, and a name does not move when the
     * entry beside it is removed -- which is the entire point of the selection
     * section above. The old version copied every selected path into an 8 KB
     * static first, because resolving a row number mid-loop deleted the wrong
     * file once the first delete had shifted the listing. */
    for (int i = 0; i < sel_count; i++) {
        char path[PMAX];
        if (sel_path(i, path, PMAX) == 0) delete_tree(path, 0);
    }
    sel_clear(); info_open = 0;
}

static void do_copy(int cut)
{
    clip_count = 0; clip_cut = cut;
    for (int i = 0; i < sel_count && clip_count < N; i++)
        if (sel_path(i, clip[clip_count], PMAX) == 0) clip_count++;
}

static void do_paste(void)
{
    for (int i = 0; i < clip_count; i++) {
        char leaf[ENAME];
        leaf_of(clip[i], leaf, sizeof leaf);
        if (!leaf[0]) continue;
        char dst[PMAX];
        pjoin(dst, cwd, leaf, PMAX);
        if (path_under(clip[i], dst)) continue;
        if (clip_cut) sys_rename(clip[i], dst);
        else if (copy_tree(clip[i], dst, 0) < 0)
            fprintf(stderr, "files: paste failed for %s\n", clip[i]);
    }
    if (clip_cut) clip_count = 0;
    sel_clear();
}

static void start_rename(void)
{
    if (sel_count != 1) return;
    scpy(editbuf, sel[0], sizeof editbuf);
    rename_mode = 1; newfolder_mode = 0; info_open = 0;
}

static int new_name_valid(void)
{
    int n = slen(editbuf), base = slen(cwd);
    if (!n || n >= ENAME - 4 || strchr(editbuf, '/') ||
        streq(editbuf, ".") || streq(editbuf, "..") || base + n + 2 > PMAX) {
        scpy(agent_notice, "名称须为 1–59 字节，完整路径须短于 128 字节", sizeof agent_notice);
        return 0;
    }
    return 1;
}

static void commit_rename(void)
{
    if (!new_name_valid()) return;
    if (sel_count == 1) {
        char old[PMAX], np[PMAX];
        if (sel_path(0, old, PMAX) == 0) {
            pjoin(np, cwd, editbuf, PMAX);
            if (sys_rename(old, np) < 0) {
                scpy(agent_notice, "改名未完成，请检查名称和权限", sizeof agent_notice);
                return;
            }
            agent_notice[0] = 0;
            /* Follow the file rather than dropping it: the user renamed the
             * thing they had selected and still has it selected. */
            sel_only(editbuf);
        }
    }
    rename_mode = 0; editbuf[0] = 0;
}

static void start_newfolder(void) { editbuf[0] = 0; newfolder_mode = 1; rename_mode = 0; info_open = 0; }

static void commit_newfolder(void)
{
    if (!new_name_valid()) return;
    char np[PMAX];
    pjoin(np, cwd, editbuf, PMAX);
    if (make_dir(np) < 0) {
        scpy(agent_notice, "Project 未创建，请检查名称和权限", sizeof agent_notice);
        return;
    }
    agent_notice[0] = 0;
    newfolder_mode = 0; editbuf[0] = 0;
}

static void append(char *o, int *oi, int omax, const char *s)
{
    for (int i = 0; s[i] && *oi < omax - 1; i++) o[(*oi)++] = s[i];
    o[*oi] = 0;
}

static void do_get_info(void)
{
    info_open = 0;
    if (sel_count != 1) return;
    int idx = find_ent(sel[0]);
    if (idx < 0) return;
    char path[PMAX];
    if (item_path(idx, path, PMAX) < 0) return;

    char num[24]; int oi = 0;
    append(info_text, &oi, (int)sizeof info_text, "Name: ");
    append(info_text, &oi, (int)sizeof info_text, ent[idx].name);
    append(info_text, &oi, (int)sizeof info_text, "\n");
    append(info_text, &oi, (int)sizeof info_text, ent[idx].is_dir ? "Type: Project\n" : "Type: File\n");
    if (ent[idx].is_dir) {
        itoa_(dir_count(path), num);
        append(info_text, &oi, (int)sizeof info_text, "Items: ");
        append(info_text, &oi, (int)sizeof info_text, num);
    } else {
        itoa_(ent[idx].size, num);
        append(info_text, &oi, (int)sizeof info_text, "Size: ");
        append(info_text, &oi, (int)sizeof info_text, num);
        append(info_text, &oi, (int)sizeof info_text, " bytes");
    }
    info_open = 1;
}

static void ask_logit(void)
{
    char names[AG_OBJECTS][AG_PATH]; const char *paths[AG_OBJECTS];
    if (sel_count > AG_OBJECTS) {
        scpy(agent_notice, "最多选择 32 份资料", sizeof agent_notice);
        return;
    }
    unsigned n = 0;
    for (int i = 0; i < sel_count; i++) {
        if (sel_path(i, names[n], AG_PATH) < 0) {
            scpy(agent_notice, "所选路径不可用或过长", sizeof agent_notice);
            return;
        }
        paths[n] = names[n]; n++;
    }
    if (!n) { paths[0] = cwd; n = 1; }
    uint64_t context;
    if (ag_publish_selection(paths, n, cwd, &context) == 0) { agent_notice[0] = 0; ag_show_assistant(context); }
    else scpy(agent_notice, "任务服务暂不可用，选择已保留", sizeof agent_notice);
}

/* --- context menu ------------------------------------------------------------
 * The labels and the actions are one table. They used to be an array of strings
 * and a switch on the index, which is a jar with two doors: reordering the
 * array moved every command onto the wrong label, silently and with no way for
 * a reader of either half to notice. */
struct menu_item { const char *label; void (*run)(void); };
static void mi_open(void)   { if (sel_count == 1) do_open(find_ent(sel[0])); }
static void mi_copy(void)   { do_copy(0); }
static void mi_cut(void)    { do_copy(1); }
static const struct menu_item MENU[] = {
    { "Open",        mi_open },
    { "New Project", start_newfolder },
    { "Rename",      start_rename },
    { "Delete",      do_delete },
    { "Copy",        mi_copy },
    { "Cut",         mi_cut },
    { "Paste",       do_paste },
    { "Get Info",    do_get_info },
};
#define MENU_N (int)(sizeof MENU / sizeof MENU[0])
#define MENU_W 124
#define MENU_IH 24

/* Placement is computed here and nowhere else, so the menu is hit-tested at the
 * rectangle it was drawn at even when it was clamped against an edge. */
static void menu_rect(int *mx, int *my, int *mh)
{
    *mh = MENU_N * MENU_IH + 4;
    *mx = menu_x; *my = menu_y;
    if (*mx + MENU_W > CONTENT_RIGHT) *mx = CONTENT_RIGHT - MENU_W;
    if (*my + *mh > WINH) *my = WINH - *mh;
    if (*mx < 0) *mx = 0;
    if (*my < 0) *my = 0;
}

/* --- WHAT aui's FLUSH DIFF CANNOT SEE ----------------------------------------
 * aui_end() works out the flush rectangle by diffing the draw calls made INSIDE
 * aui.c: ad_note_rect/_icon/_text are macros defined at aui.c:85-117 that shadow
 * gui_rect/gui_icon/gui_text_run FOR THAT TRANSLATION UNIT ONLY. This file
 * includes aui.h, so every primitive it issues itself -- every list row, every
 * icon, the sidebar's active highlight -- reaches the kernel unrecorded, and the
 * rectangle aui hands the compositor covers the widgets and not the content.
 *
 * MEASURED, not deduced. Navigating from the 3-entry root to the 10-entry
 * application view repainted the toolbar title and the first TWO rows and left
 * the rest of the previous directory on screen, selection highlight and all.
 * The reported rectangle was the union of the two aui labels that had changed
 * -- the toolbar title, and the Project pane's 使用…的资料 -- and that union
 * happened to reach as far down as row 1. The listing in the app's own canvas
 * was correct the entire time; this was never a listing bug.
 *
 * It only became visible when a directory listing got SHORTER for the first
 * time. Everything about a damage scheme keyed on what got DRAWN is blind to
 * what stopped being drawn.
 *
 * textedit.c and clock/view.c each hit this and each answered it with
 * aui_end_rect(); textedit.c's comment above te_prev_* states the scoping rule
 * outright. Doing it once per app is not the real fix -- the real fix is for the
 * recorders to be reachable from aui.h -- but it is the fix that is local to
 * this file, and a third app rediscovering this from a blank screen is worse.
 *
 * The signature below is the same idea as aui's own: hash everything this file
 * draws with its own hands, and report the rectangle only when that changes. */
static unsigned sig_acc, content_sig;

static void sig_int(int v) { sig_acc = (sig_acc ^ (unsigned)v) * 16777619u; }
static void sig_str(const char *s) { for (int i = 0; s[i]; i++) sig_int((unsigned char)s[i]); }

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

#include "files_project.inc"

/* --- sidebar model -----------------------------------------------------------
 * `apps` marks the one row that is a VIEW rather than a path. It used to be a
 * second row pointing at "/", distinguished from LogitOS HD by testing its icon
 * (`SIDE[i].icon != GICON_TERMINAL`) so the two rows would not both highlight --
 * a model that stores the wrong thing and then patches the symptom in the
 * painter. */
struct side { const char *label; const char *path; int icon; int header; int apps; };
static const struct side SIDE[] = {
    { "我的 Project", 0, 0, 1, 0 },
    { "文稿",        "/docs",  GICON_DOC,      0, 0 },
    { "系统位置",     0, 0, 1, 0 },
    { "字体",        "/fonts", GICON_FOLDER,   0, 0 },
    { "位置",        0, 0, 1, 0 },
    { "LogitOS HD",  "/",      GICON_GRID,     0, 0 },
    { "应用程序",     "/",      GICON_TERMINAL, 0, 1 },
};
#define SIDE_N (int)(sizeof SIDE / sizeof SIDE[0])

/* y of sidebar entry i's top (headers and items share the same 28px rhythm) */
static int side_y(int i) { return 14 + i * 28; }

static void side_go(const struct side *s)
{
    navigate(s->path);                  /* clears apps_view, resets scroll/selection */
    apps_view = s->apps;                /* frame()'s scan_dir() picks up the filter */
}

/* --- frame draw --- */
static void draw_sidebar(void)
{
    aui_glass(0, 0, SIDEBAR_W, WINH, 1);                 /* liquid-glass sidebar */
    gui_rect(SIDEBAR_W - 1, 0, 1, WINH, AUI_BORDER);
    unsigned selbg = aui_is_dark() ? rgb(58, 70, 92) : rgb(210, 224, 250);
    for (int i = 0; i < SIDE_N; i++) {
        int ty = side_y(i);
        if (SIDE[i].header) { gui_text_run(16, ty + 4, 12, 0, AUI_MUTED, SIDE[i].label, slen(SIDE[i].label)); continue; }
        int active = SIDE[i].apps ? apps_view : (!apps_view && streq(cwd, SIDE[i].path));
        if (active) { sig_int(i); gui_rect(8, ty - 2, SIDEBAR_W - 16, 26, selbg); }
        gui_icon(SIDE[i].icon, 16, ty, 18, rgb(86, 152, 236));
        gui_text_run(42, ty + 3, 14, 0, AUI_TEXT, SIDE[i].label, slen(SIDE[i].label));
    }
}

/* toolbar button rects (shared by draw + hit-test, so no immediate-mode lag) */
#define TB_BTN_Y 10
#define TB_BTN_H 26
#define TB_BACK_X (CX + 8)
#define TB_BACK_W 30
#define TB_NEW_X  (CONTENT_RIGHT - 8 - 72)
#define TB_NEW_W  72
#define TB_VIEW_X (CONTENT_RIGHT - 8 - 72 - 56)
#define TB_VIEW_W 52

static void glass_btn(int x, int y, int w, int h, const char *label)
{
    int rad = h / 2; if (rad > 11) rad = 11;
    aui_glass(x, y, w, h, rad);
    int lw = text_measure_px(label, slen(label), 13, 0);
    gui_text_run(x + (w - lw) / 2, y + (h - 13) / 2, 13, 0, AUI_TEXT, label, slen(label));
}

static void draw_toolbar(void)
{
    aui_glass(CX, 0, CWID, TOOLBAR_H, 1);               /* liquid-glass toolbar */
    gui_rect(CX, TOOLBAR_H - 1, CWID, 1, AUI_BORDER);
    glass_btn(TB_BACK_X, TB_BTN_Y, TB_BACK_W, TB_BTN_H, "<");

    /* title (folder leaf) or inline rename / new-folder field */
    if (rename_mode) {
        if (aui_textfield(CX + 48, 11, 230, editbuf, sizeof editbuf)) commit_rename();
    } else if (newfolder_mode) {
        if (aui_textfield(CX + 48, 11, 230, editbuf, sizeof editbuf)) commit_newfolder();
    } else {
        char title[64];
        if (apps_view)          scpy(title, "应用程序", sizeof title);
        else if (streq(cwd, "/")) scpy(title, "LogitOS HD", sizeof title);
        else                    leaf_of(cwd, title, sizeof title);
        aui_heading(CX + 48, 12, title, AUI_TEXT);
    }
    glass_btn(TB_VIEW_X, TB_BTN_Y, TB_VIEW_W, TB_BTN_H, view_mode ? "Icons" : "List");
    glass_btn(TB_NEW_X, TB_BTN_Y, TB_NEW_W, TB_BTN_H, "+Project");
}

/* One draw pass for both views. The two used to be separate functions that
 * shared their selection colour, their clip, their icon lookup and their label
 * fitting by copy. What actually differs between them is the rectangle, and
 * item_rect() already owns that. */
static void draw_items(void)
{
    unsigned selbg = aui_is_dark() ? rgb(58, 70, 92) : rgb(208, 224, 250);
    sig_int(view_mode); sig_int(scroll); sig_int(ndropped); sig_int(aui_is_dark());
    gui_clip(CX, CY, CWID, CHGT);
    for (int i = 0; i < nent; i++) {
        int rx, ry, rw, rh;
        if (!item_rect(i, &rx, &ry, &rw, &rh)) continue;
        const struct entry *en = &ent[i];
        unsigned col; int icon = ext_icon(en->name, en->is_dir, &col);
        int selected = sel_has(en->name);
        /* Exactly the inputs to this row's pixels, and nothing else: a field
         * that is drawn but not hashed is a change the compositor never hears
         * about, and a field that is hashed but not drawn is a repaint for
         * nothing. */
        sig_str(en->name); sig_int((int)en->size); sig_int(en->is_dir);
        sig_int(selected); sig_int(rx); sig_int(ry);

        if (view_mode) {
            if (selected) gui_rect(rx, ry, rw, rh, selbg);
            gui_icon(icon, rx + 6, ry + 2, 20, col);
            char lab[48]; fit_label(en->name, CWID - 150, lab, sizeof lab);
            gui_text_run(rx + 34, ry + 6, 14, 0, AUI_TEXT, lab, slen(lab));
            if (en->is_dir) gui_text_run(CONTENT_RIGHT - 96, ry + 6, 13, 0, AUI_MUTED, "--", 2);
            else { char num[24]; itoa_(en->size, num); gui_text_run(CONTENT_RIGHT - 96, ry + 6, 13, 0, AUI_MUTED, num, slen(num)); }
        } else {
            if (selected) gui_rect(rx + 6, ry + 4, rw - 12, rh - 8, selbg);
            gui_icon(icon, rx + (GW - GICON) / 2, ry + 12, GICON, col);
            char lab[40]; fit_label(en->name, GW - 14, lab, sizeof lab);
            int lw = text_measure_px(lab, slen(lab), 13, 0);
            gui_text_run(rx + (GW - lw) / 2, ry + GICON + 20, 13, 0, AUI_TEXT, lab, slen(lab));
        }
    }
    /* N is a fixed ceiling on a directory this app can show, and a listing that
     * quietly stops at 64 is the silent-truncation shape this tree has paid for
     * twice (LOGIT_ARG_MAX, the SSH password prefix). Say the number. */
    if (ndropped) {
        char msg[64], num[24]; int oi = 0;
        itoa_(ndropped, num);
        append(msg, &oi, (int)sizeof msg, "还有 ");
        append(msg, &oi, (int)sizeof msg, num);
        append(msg, &oi, (int)sizeof msg, " 项未显示");
        gui_text_run(CX + 12, CY + CHGT - 20, 13, 0, AUI_MUTED, msg, slen(msg));
    }
    gui_clip(0, 0, 0, 0);
}

static void draw_info(void)
{
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

static void draw_menu(void)
{
    int mx, my, mh;
    menu_rect(&mx, &my, &mh);
    gui_rect(mx - 1, my - 1, MENU_W + 2, mh + 2, AUI_BORDER);
    gui_rect(mx, my, MENU_W, mh, AUI_SURFACE);
    for (int i = 0; i < MENU_N; i++)
        gui_text_run(mx + 12, my + 6 + i * MENU_IH, 14, 0, AUI_TEXT, MENU[i].label, slen(MENU[i].label));
}

static void frame(void)
{
    /* Take the snapshot here, and ONLY here. dispatch() runs before the next
     * frame(), so its hit test resolves against the listing that was actually
     * drawn -- the user clicks the row they can see, not the row the directory
     * has drifted to since. Re-scanning inside the click handler instead would
     * be the subtle version of the same bug. */
    scan_dir();
    aui_begin(AUI_BG);
    sig_acc = 2166136261u;              /* FNV-1a offset basis, per frame */
    /* Content first, then the glass sidebar + toolbar composited ON TOP of it
     * (so the chrome frosts the app's own content). */
    clamp_scroll();
    draw_items();
    draw_sidebar();
    draw_toolbar();
    draw_project();
    if (info_open) draw_info();
    if (menu_open) draw_menu();
    if (aui_button(CONTENT_RIGHT - 118, WINH - 30, 108, 24, "Ask Logit")) ask_logit();
    if (agent_notice[0]) aui_label(CX + 8, WINH - 27, agent_notice, AUI_MUTED);

    /* The rectangle covers the sidebar AND the content area, because both are
     * painted by this file with raw primitives aui never saw. The Project pane
     * is entirely aui widgets, so aui's own diff already covers it -- reporting
     * it here too would flush the whole window on every keystroke into the goal
     * field. Unioned with aui's rect, never instead of it (aui.c:1800). */
    if (sig_acc != content_sig) {
        content_sig = sig_acc;
        aui_end_rect(0, 0, CONTENT_RIGHT, WINH);
    } else {
        aui_end();
    }
}

/* --- input ------------------------------------------------------------------- */
static void sidebar_click(int y)
{
    for (int i = 0; i < SIDE_N; i++) {
        if (SIDE[i].header) continue;
        int ty = side_y(i);
        if (y >= ty - 2 && y < ty + 24) { side_go(&SIDE[i]); return; }
    }
}

static void toggle_view(void)
{
    /* Keep the reading position across the switch. `scroll` counts ROWS and a
     * row is one item in the list and `grid_cols()` items in the grid, so the
     * old `scroll = 0` was not a reset, it was the only arithmetic that did not
     * need to know that. */
    int first = scroll * cols_();
    view_mode = !view_mode;
    scroll = first / cols_();
}

/* Returns 1 if aui should also see this click (i.e. it was not consumed here). */
static int handle_click(int x, int y)
{
    /* A click that dismisses the context menu belongs to the menu and to
     * nothing underneath it. */
    if (menu_open) {
        int mx, my, mh;
        menu_rect(&mx, &my, &mh);
        menu_open = 0;
        if (hit(x, y, mx, my, MENU_W, mh)) {
            int i = (y - my - 4) / MENU_IH;
            if (i >= 0 && i < MENU_N) MENU[i].run();
        }
        return 0;
    }
    if (x >= CONTENT_RIGHT) return 1;                       /* the Project pane is aui's */
    if (hit(x, y, CONTENT_RIGHT - 118, WINH - 30, 108, 24)) return 1;  /* Ask Logit; frame() runs it */
    if (x < SIDEBAR_W) { sidebar_click(y); return 1; }
    if (y < TOOLBAR_H) {
        if (hit(x, y, TB_BACK_X, TB_BTN_Y, TB_BACK_W, TB_BTN_H)) go_up();
        else if (hit(x, y, TB_VIEW_X, TB_BTN_Y, TB_VIEW_W, TB_BTN_H)) toggle_view();
        else if (hit(x, y, TB_NEW_X, TB_BTN_Y, TB_NEW_W, TB_BTN_H)) start_newfolder();
        return 1;                                           /* rename/new field is aui's */
    }

    int idx = entry_at(x, y);
    /* Empty space clears, but a MODIFIED click on empty space does not: the
     * user holding ctrl is in the middle of building a selection and did not
     * hit a row. (This condition was already written; it was unreachable,
     * because both modifier flags were permanently 0.) */
    if (idx < 0) {
        if (!shift_held && !ctrl_held) { sel_clear(); last_click_name[0] = 0; }
        return 1;
    }
    const char *name = ent[idx].name;

    if (ctrl_held)       { sel_toggle(name); scpy(anchor, name, ENAME); }
    else if (shift_held) { sel_range(idx); }
    else {
        if (streq(name, last_click_name) && monotonic_ms() - last_click_ms <= 500) {
            last_click_name[0] = 0;
            do_open(idx);
            return 1;
        }
        sel_only(name);
    }
    scpy(last_click_name, name, ENAME);
    last_click_ms = monotonic_ms();
    return 1;
}

static void handle_key(int k)
{
    if (k == 12) { ask_logit(); return; }                   /* Ctrl+L */
    if (k == KEY_PGUP)        { scroll -= rows_view(); if (scroll < 0) scroll = 0; }
    else if (k == KEY_PGDN)   { scroll += rows_view(); }
    else if (k == KEY_UP)     { if (scroll > 0) scroll--; }
    else if (k == KEY_DOWN)   { scroll++; }
    else if (k == 27)         { menu_open = 0; info_open = 0; rename_mode = 0; newfolder_mode = 0; }
    /* Ctrl+H reveals what hidden.h filters. The selection SURVIVES it: the
     * names it holds still name the same files, whether or not those files are
     * being drawn. The previous version cleared it here, and had to, because it
     * held row numbers into a listing that had just changed length. */
    else if (ctrl_held && (k == 'h' || k == 'H' || k == 8)) {
        show_hidden = !show_hidden;
        scroll = 0; info_open = 0;
    }
}

/* --- the event loop ----------------------------------------------------------
 * ONE frame() call site. The previous version had six, each wrapped differently:
 * the click path fed aui only when no menu had been open, the key path fed aui
 * AFTER the app had already acted on the key, and EV_THEME and EV_MOUSE_R fed it
 * not at all -- three orderings of the same two steps, chosen by wherever that
 * branch's `continue` happened to sit. */
void app_main(void)
{
    gui_create("Finder", WINW, WINH);
    /* The SAME size, told to aui. It is a jar with two doors: gui_create tells
     * the KERNEL how big the window is, aui_set_size tells the TOOLKIT, and
     * this app only ever walked through the first. aui's win_w/win_h default to
     * 640x480 (aui.c:128) and ad_finish() clamps every flush rectangle to them
     * -- so for the whole life of this 1120x660 window, damage below y=480
     * could not be reported no matter what changed there, and a directory
     * listing is exactly the thing that is tall.
     *
     * Four more apps are missing this line today: greeter, preview, terminal,
     * widgets. Seven others (settings, monitor, gallery, studio, textedit, ch,
     * assistant) have it. The real fix is for one call to serve both doors. */
    aui_set_size(WINW, WINH);
    project_refresh(1);
    frame();
    struct logit_event e;
    for (;;) {
        if (!poll_event(&e)) {
            if (project_refresh(0)) frame();
            wait_idle(250);                 /* was sys_yield(): a spin. input-driven */
            continue;
        }
        /* The WM delivers EV_MOUSE_MOVE at pointer rate (~100/s) and the Finder
         * has no hover state, so drop it rather than repaint per sample.
         * Double-click timing uses the monotonic clock, not event arrival. */
        if (e.type == EV_MOUSE_MOVE) continue;
        if (e.type == EV_CLOSE) app_exit(0);

        shift_held = (e.mods & EV_MOD_SHIFT) != 0;
        ctrl_held  = (e.mods & EV_MOD_CTRL) != 0;

        int feed = 0;
        switch (e.type) {
        case EV_KEY:
            handle_key((int)e.a);
            feed = 1;
            break;
        case EV_MOUSE_R:
            if (e.a >= CONTENT_RIGHT) continue;
            { int idx = entry_at((int)e.a, (int)e.b);   /* right-click selects under it */
              if (idx >= 0 && !sel_has(ent[idx].name)) sel_only(ent[idx].name); }
            menu_open = 1; menu_x = (int)e.a; menu_y = (int)e.b; info_open = 0;
            break;
        case EV_MOUSE:
            feed = handle_click((int)e.a, (int)e.b);
            break;
        default:
            break;                          /* EV_THEME and anything new: repaint */
        }

        if (!streq(cwd, project_path)) project_refresh(1);
        if (feed) { aui_feed(&e); frame(); aui_feed_done(); }
        else frame();
    }
}

int main(void)
{
    struct aex_agent_identity identity;
    if (ag_self(&identity) < 0) return 1;
    if (identity.mode == AEX_ACT_WORKER) return ag_worker(AG_FINDER);
    app_main(); return 0;
}
