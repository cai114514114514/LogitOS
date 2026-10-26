/* Spotlight ("Fluid" P4) -- Cmd+Space, type, Enter.
 *
 * One panel window (WINS_SET_PANEL: no titlebar, centred, hidden when it
 * loses focus) searching three kinds of thing by name:
 *
 *   apps      every *.aex in / -- named from its AEX header, as the dock does
 *   files     a walk of the filesystem, built the first time a query needs it
 *             and capped, skipping the system trees nobody searches by hand
 *   settings  the switches worth flipping without opening Settings (applied
 *             directly), and Settings itself
 *
 * and, as the last row whenever something is typed, "Ask Assistant", which
 * starts the assistant with the query as its goal (SYS_OPEN_PATH_ARG).
 *
 * Ranking is sl_rank.h (host-tested). Keys are handled here, not by an aui
 * text field: Up/Down move through results and Escape puts the panel away,
 * and a committed Chinese character from the IME arrives as one EV_KEY code
 * point, which is appended as UTF-8 like any other key.
 *
 * Lives in /sbin, so it is not in the dock; the window manager starts it on the
 * first Cmd+Space and shows the same process on every one after. */
#include "aui.h"
#include "sl_rank.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#define WIN_W   620
#define ROW_H   40
#define NSHOW   6
#define FIELD_H 56
#define WIN_H   (FIELD_H + NSHOW * ROW_H + 12)

enum { K_APP, K_FILE, K_SET, K_TOGGLE, K_ASK };
struct item { int kind; char name[64]; char path[128]; const char *key; };

static struct item apps[48];
static int napps;
static struct item *files;
static int nfiles, files_built;
#define FILES_MAX 3000

/* `name` is what the row shows; `alias` is the English name it had until
 * 2026-10-26, still matched so "dark" keeps finding the switch. */
static const struct { const char *name, *alias, *key; } toggles[] = {
    { "深色外观", "Dark appearance", "ui.dark" },
    { "减弱动态效果", "Reduce motion", "ui.reduce_motion" },
    { "降低透明度", "Reduce transparency", "ui.reduce_transparency" },
};

static char query[128];
static int qlen;
static struct item shown[NSHOW + 1];
static int nshown, sel;

static void win_state(int what, int arg) { _sys(SYS_GUI_WIN_STATE, what, arg, 0); }

/* ---------------------------------------------------------------- sources -- */

static void load_apps(void)
{
    DIR *d = opendir("/");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && napps < (int)(sizeof apps / sizeof apps[0])) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".aex")) continue;
        struct item *it = &apps[napps];
        snprintf(it->path, sizeof it->path, "/%s", e->d_name);
        /* The display name is the AEX header's (offset 8, 32 bytes), as the
         * dock shows it; the file name is the fallback. */
        char hdr[40] = {0};
        FILE *f = fopen(it->path, "rb");
        int ok = f && fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "AEX", 3);
        if (f) fclose(f);
        if (ok && hdr[8]) { memcpy(it->name, hdr + 8, 32); it->name[32] = 0; }
        else { snprintf(it->name, sizeof it->name, "%.*s", (int)(n - 4), e->d_name); }
        it->kind = K_APP;
        napps++;
    }
    closedir(d);
}

static int skip_dir(const char *p)
{
    static const char *sys[] = { "/proc", "/dev", "/bin", "/sbin", "/usr", "/fonts", "/ime",
                                 "/icons", "/lib", "/etc", "/tu", "/sys" };
    for (unsigned i = 0; i < sizeof sys / sizeof sys[0]; i++) if (!strcmp(p, sys[i])) return 1;
    return 0;
}

static void walk(const char *dir, int depth)
{
    if (depth > 6 || nfiles >= FILES_MAX || skip_dir(dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nfiles < FILES_MAX) {
        if (e->d_name[0] == '.') continue;
        char p[128];
        if (snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(p, depth + 1); continue; }
        size_t n = strlen(e->d_name);
        if (depth == 0 && n > 4 && !strcmp(e->d_name + n - 4, ".aex")) continue;   /* the apps */
        struct item *it = &files[nfiles++];
        it->kind = K_FILE;
        snprintf(it->name, sizeof it->name, "%s", e->d_name);
        snprintf(it->path, sizeof it->path, "%s", p);
    }
    closedir(d);
}

static void build_files(void)
{
    if (files_built) return;
    files_built = 1;
#ifdef SPOTLIGHT_NEGCTL_NO_FILES
    return;              /* THE CONTROL (make test-spotlight-negctl): no file source */
#endif
    files = calloc(FILES_MAX, sizeof *files);
    if (files) walk("/", 0);
}

/* ---------------------------------------------------------------- results -- */

static void consider_named(const struct item *it, const char *match, int *best, int *score)
{
    int sc = sl_score(query, match);
    if (sc <= 0) return;
    /* insertion into the top NSHOW by score; apps first among equals */
    if (it->kind == K_APP) sc += 1;
    int at = nshown;
    while (at > 0 && score[at - 1] < sc) at--;
    if (at >= NSHOW) return;
    int end = nshown < NSHOW ? nshown : NSHOW - 1;
    for (int k = end; k > at; k--) { shown[k] = shown[k - 1]; score[k] = score[k - 1]; best[k] = best[k - 1]; }
    shown[at] = *it; score[at] = sc; best[at] = 1;
    if (nshown < NSHOW) nshown++;
}

static void consider(const struct item *it, int *best, int *score)
{ consider_named(it, it->name, best, score); }

static void search(void)
{
    int best[NSHOW + 1], score[NSHOW + 1];
    nshown = 0;
    if (!qlen) {                                  /* nothing typed: the apps */
        for (int i = 0; i < napps && nshown < NSHOW; i++) shown[nshown++] = apps[i];
        sel = 0;
        return;
    }
    for (int i = 0; i < napps; i++) consider(&apps[i], best, score);
    for (unsigned i = 0; i < sizeof toggles / sizeof toggles[0]; i++) {
        struct item t = { K_TOGGLE, "", "", toggles[i].key };
        snprintf(t.name, sizeof t.name, "%s", toggles[i].name);
        const char *m = sl_score(query, toggles[i].alias) > sl_score(query, toggles[i].name)
                        ? toggles[i].alias : toggles[i].name;
        consider_named(&t, m, best, score);
    }
    { struct item t = { K_SET, "Settings", "/settings.aex", 0 }; consider(&t, best, score); }
    build_files();
    for (int i = 0; i < nfiles; i++) consider(&files[i], best, score);
    /* The assistant is always the last row, never ranked above a match. */
    if (nshown == NSHOW) nshown--;
    struct item *a = &shown[nshown++];
    a->kind = K_ASK;
    snprintf(a->name, sizeof a->name, "询问助手：%s", query);
    a->path[0] = 0;
    if (sel >= nshown) sel = nshown - 1;
}

static void put_away(void)
{
    qlen = 0; query[0] = 0; sel = 0;
    search();
    win_state(WINS_SET_HIDDEN, 1);
}

static void activate(int i)
{
    if (i < 0 || i >= nshown) return;
    struct item *it = &shown[i];
    printf("[spotlight] open kind=%d %s\n", it->kind, it->kind == K_ASK ? query : it->path[0] ? it->path : it->name);
    switch (it->kind) {
    case K_APP: case K_FILE: case K_SET: sys_open_path(it->path); break;
    case K_TOGGLE: {
        char v[8] = "0";
        setting_get(it->key, v, sizeof v);
        const char *nv = v[0] == '1' ? "0" : "1";
        if (!strcmp(it->key, "ui.dark")) sys_ui_dark(nv[0] == '1');
        setting_set(it->key, nv, 1);
        break;
    }
    case K_ASK: _sys(SYS_OPEN_PATH_ARG, (long)"/assistant.aex", (long)query, 0); break;
    }
    put_away();
}

/* ---------------------------------------------------------------- drawing -- */

static const char *kind_name(int k)
{
    return k == K_APP ? "应用程序" : k == K_FILE ? "文件" : k == K_TOGGLE ? "开关"
         : k == K_SET ? "设置" : "助手";
}

static void draw(void)
{
    aui_begin(AUI_BG);
    aui_round(10, 10, WIN_W - 20, FIELD_H - 16, 12, AUI_SURFACE);
    /* The magnifier, drawn: a ring (aui_stroke on a round rect whose radius is
     * half its side) and a short handle of overlapping dots. The shipped fonts
     * have no search glyph -- U+2315 fell back to .notdef and sat on the text. */
    aui_stroke(24, 20, 16, 16, 8, 2, AUI_MUTED);
    for (int k = 0; k < 5; k++) aui_circle(38 + k, 34 + k, 1, AUI_MUTED);
    if (qlen) aui_text_sz(54, 17, query, AUI_TEXT, 22);
    else      aui_text_sz(54, 17, "聚焦搜索", AUI_MUTED, 22);
    int cx = 54 + (qlen ? aui_text_w(query, 22) : 0) + 2;
    aui_fill(cx, 18, 2, 24, AUI_ACCENT);                            /* caret */
    for (int i = 0; i < nshown; i++) {
        int y = FIELD_H + i * ROW_H;
        if (i == sel) aui_round(8, y + 2, WIN_W - 16, ROW_H - 4, 8, AUI_ACCENT);
        unsigned ink = i == sel ? 0xFFFFFF : AUI_TEXT, dim = i == sel ? 0xE8EEFF : AUI_MUTED;
        aui_text_ellipsis(22, y + 11, WIN_W - 190, shown[i].name, ink, 15);
        struct aui_rect r = { WIN_W - 170, y, 150, ROW_H };
        aui_text_in(r, kind_name(shown[i].kind), dim, 13, AUI_ALIGN_RIGHT);
    }
    aui_end();
}

/* ---------------------------------------------------------------- input -- */

static void put_utf8(unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    if (qlen + n >= (int)sizeof query) return;
    memcpy(query + qlen, b, (size_t)n); qlen += n; query[qlen] = 0;
}

static void backspace(void)
{
    while (qlen > 0 && ((unsigned char)query[qlen - 1] & 0xC0) == 0x80) qlen--;
    if (qlen > 0) qlen--;
    query[qlen] = 0;
}

static int on_key(int c, int mods)
{
    if (mods & (EV_MOD_SUPER | EV_MOD_CTRL)) return 0;
    if (c == 27) { put_away(); return 1; }
    if (c == '\n' || c == '\r') { activate(sel); return 1; }
    if (c == KEY_DOWN) { if (sel + 1 < nshown) sel++; return 1; }
    if (c == KEY_UP)   { if (sel > 0) sel--; return 1; }
    if (c == 8 || c == 127) { backspace(); sel = 0; search(); return 1; }
    if (c >= 32 && c != 127 && c < 0x110000 && !(c >= 0x100 && c <= 0x1FF)) {
        put_utf8((unsigned)c); sel = 0; search(); return 1;
    }
    return 0;
}

void app_main(void)
{
    gui_create("Spotlight", WIN_W, WIN_H);
    aui_set_size(WIN_W, WIN_H);
    win_state(WINS_SET_PANEL, 1);
    load_apps();
    search();
    draw();
    for (;;) {
        struct logit_event e;
        if (!poll_event(&e)) { wait_idle(0); continue; }
        if (e.type == EV_CLOSE) app_exit(0);
        int redraw = 0;
        if (e.type == EV_KEY) redraw = on_key(e.a, e.mods);
        else if (e.type == EV_MOUSE && e.b >= FIELD_H) {
            int i = (e.b - FIELD_H) / ROW_H;
            if (i < nshown) { sel = i; activate(i); }
            redraw = 1;
        } else if (e.type == EV_WINDOW_FOCUS || e.type == EV_RESIZE) redraw = 1;
        if (redraw) draw();
    }
}
