/* TextEdit -- a small text editor.
 *
 * WHAT WAS WRONG WITH IT, and none of it was the editing. The status bar was
 * `gui_rect(..., rgb(236, 238, 242))` with a `rgb(214, 216, 222)` rule over it:
 * two light-mode colours written into the source, so in dark mode this window
 * had a white strip glued to the bottom of a near-black page. The window size
 * was three constants that had to agree, the monospace advance was assumed to
 * be 8 pixels regardless of the font or the backing scale, and text past the
 * bottom edge was drawn off the window rather than scrolled to -- so a file
 * longer than eighteen lines could be typed into and never seen.
 *
 * Everything visible now comes from the toolkit's tokens and from
 * aui_width()/aui_height() on the frame it is drawn, and the advance is
 * measured from the font actually loaded.
 *
 * STILL DELIBERATELY SMALL: one buffer, append-and-backspace, no selection, no
 * undo, no mouse caret placement. This is the app Finder opens a .txt with, and
 * growing it into an editor is a different piece of work from making it stop
 * looking wrong. */
#include "aui.h"

#define MAXT   8000
#define CTRL_S 0x13

static char text[MAXT + 1];
static int  tlen;
static char fname[64];
static int  saved;          /* 1 just after a successful save, 0 once edited */
static int  scroll;         /* first visible line */

/* Window geometry, remembered in the "app." preference namespace (see the
 * SYS_SETTING_ENUM comment in logit_abi.h): app.textedit.w / app.textedit.h,
 * per-user, no schema entry needed -- the first thing in this codebase that
 * used the store for anything other than the nine hardcoded machine keys.
 *
 * geom_dirty + commit-at-close, not commit-per-resize: a window being dragged
 * by its corner fires EV_RESIZE many times a second, and setting_set(...,
 * commit=0) is a RAM-only update -- cheap regardless of how often it runs --
 * while setting_commit() is a whole-file LogitFS write. Committing on every
 * frame of a drag would turn "resize the window" into "hammer the disk"; the
 * store's own doc comment says as much ("set several keys with commit=0 and
 * finish with setting_commit() -- rather than N of them"), and a resize drag
 * is exactly that batch, just spread across frames instead of one call. */
#define TE_W_DEFAULT 520
#define TE_H_DEFAULT 360
#define TE_W_MIN     240
#define TE_H_MIN     160
static int geom_dirty;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void itoa_(int v, char *b)
{
    char t[16]; int n = 0, p = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[p++] = '-';
    while (n) b[p++] = t[--n];
    b[p] = 0;
}

/* Parses a bare non-negative int, or returns -1 for anything that is not one
 * -- empty, a stray letter, a sign. -1 (rather than 0, a real width nobody
 * wants) is what tells load_geometry() to fall back to the default instead of
 * opening a window an out-of-range or hand-edited settings file asked for. */
static int atoi_or_neg(const char *s)
{
    if (!s || !s[0]) return -1;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        int d = s[i] - '0';
        /* Overflow is a REFUSAL here, not silent wraparound -- a settings
         * file holding "app.textedit.w = 99999999999999999999" (a torn write,
         * a hand edit, or a value some future caller wrote for a wider field)
         * must not carry `v` past INT_MAX as undefined behaviour and hope
         * clampi() downstream happens to net it out. That was the bug: it
         * compiled, it usually even looked fine (clampi's <=/>= still hold for
         * whatever an optimiser's chosen wrapped value happens to be), and
         * UBSan still flagged a real signed-overflow at this line. The
         * kernel's own number parser (parse_long, c/kernel/core/settings.c)
         * already refuses overflow instead of wrapping it -- this is the same
         * rule, checked before the multiply so it can never itself overflow. */
        if (v > (2147483647 - d) / 10) return -1;
        v = v * 10 + d;
    }
    return v;
}

/* Reads app.textedit.{w,h}; falls back to the default whenever a value is
 * missing, unparseable, or out of a sane range for the CURRENT screen -- a
 * garbage or stale (different-resolution) settings file must never open a
 * window off-screen or too small to use. */
static void load_geometry(int *w, int *h)
{
    char buf[LOGIT_SET_VALMAX];
    int sw = screen_w(), sh = screen_h();
    int gw = TE_W_DEFAULT, gh = TE_H_DEFAULT;

    if (setting_get("app.textedit.w", buf, (int)sizeof buf) > 0) {
        int v = atoi_or_neg(buf);
        if (v >= TE_W_MIN) gw = v;
    }
    if (setting_get("app.textedit.h", buf, (int)sizeof buf) > 0) {
        int v = atoi_or_neg(buf);
        if (v >= TE_H_MIN) gh = v;
    }
    *w = sw > 0 ? clampi(gw, TE_W_MIN, sw) : gw;
    *h = sh > 0 ? clampi(gh, TE_H_MIN, sh) : gh;
}

/* Where the caret sits, and how many lines the text occupies, under the current
 * wrap width. Both come from one walk because they are the same walk -- and the
 * caret's line is what the scroll has to chase. */
static void measure(int cols, int *nlines, int *cl, int *cc)
{
    int line = 0, col = 0;
    for (int i = 0; i < tlen; i++) {
        if (text[i] == '\n')      { line++; col = 0; }
        else if (col + 1 >= cols) { line++; col = 1; }
        else                      { col++; }
    }
    *nlines = line + 1; *cl = line; *cc = col;
}

static void draw(void)
{
    int W = aui_width(), H = aui_height();
    aui_begin(AUI_BG);

    int px = AUI_FS_BODY;
    int adv = text_measure_px("M", 1, px, 1);
    if (adv < 1) adv = 1;
    int lh = px + AUI_SP(1);
    int pad = AUI_SP(3);
    int bar = AUI_H_CTL;

    int viewh = H - bar - 2 * pad;
    int rows  = viewh / lh; if (rows < 1) rows = 1;
    int cols  = (W - 2 * pad) / adv; if (cols < 4) cols = 4;

    int nlines, cl, cc;
    measure(cols, &nlines, &cl, &cc);
    if (cl < scroll)            scroll = cl;
    if (cl >= scroll + rows)    scroll = cl - rows + 1;
    if (scroll > nlines - 1)    scroll = nlines - 1;
    if (scroll < 0)             scroll = 0;

    /* The page. A surface rather than the window background, so the text sits
     * on something with an edge -- the same relationship every other window in
     * the system has between its chrome and its content. */
    aui_round(pad - AUI_SP(1), pad - AUI_SP(1),
              W - 2 * (pad - AUI_SP(1)), viewh + AUI_SP(2), AUI_R_MD, AUI_SURFACE);

    int line = 0, col = 0, start = 0, y = pad;
    for (int i = 0; i <= tlen; i++) {
        int brk = (i == tlen) || text[i] == '\n' || col + 1 >= cols;
        if (brk) {
            int len = i - start;
            if (i < tlen && text[i] != '\n') len++;      /* the wrapped char stays on this line */
            if (line >= scroll && line < scroll + rows && len > 0)
                gui_text_run(pad, y, px, 1, AUI_TEXT, text + start, len);
            if (line >= scroll) y += lh;
            line++;
            start = i + ((i < tlen && text[i] == '\n') ? 1 : 0);
            if (i < tlen && text[i] != '\n') { start = i + 1; col = 1; } else col = 0;
            if (line >= scroll + rows) break;
        } else col++;
    }

    if (cl >= scroll && cl < scroll + rows)
        aui_fill(pad + cc * adv, pad + (cl - scroll) * lh, 2, px, AUI_ACCENT);

    /* Status bar, in the toolkit's colours, so it is a strip of chrome in both
     * themes instead of a light-mode rectangle. */
    int by = H - bar;
    aui_fill(0, by, W, bar, AUI_SURFACE_2);
    aui_hairline(0, by, W);
    int ty = by + (bar - AUI_FS_LABEL) / 2;
    aui_text_ellipsis(AUI_SP(3), ty, W - AUI_SP(30), fname, AUI_TEXT, AUI_FS_LABEL);

    const char *hint = saved ? "saved" : "Ctrl+S";
    int hw = text_measure_px(hint, saved ? 5 : 6, AUI_FS_LABEL, 0);
    aui_text_sz(W - AUI_SP(3) - hw, ty, hint, saved ? AUI_SUCCESS : AUI_MUTED, AUI_FS_LABEL);
    if (!saved) {
        int d = AUI_SP(2);
        aui_round(W - AUI_SP(4) - hw - d, by + (bar - d) / 2, d, d, d / 2, AUI_WARNING);
    }

    aui_end();
}

void app_main(void)
{
    int n = get_arg(fname, sizeof fname);
    if (n <= 0) {
        const char *d = "untitled.txt";
        int i = 0; while (d[i]) { fname[i] = d[i]; i++; } fname[i] = 0;
    }
    int w, h;
    load_geometry(&w, &h);
    gui_create(fname, w, h);
    aui_set_size(w, h);

    int r = read_file(fname, text, MAXT);
    if (r > 0) { tlen = r > MAXT ? MAXT : r; text[tlen] = 0; }
    saved = 1;
    draw();

    for (;;) {
        struct logit_event e;
        int changed = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) {
                /* Commit here, not per-resize -- see geom_dirty's comment
                 * above. A session that never touched the window size never
                 * touches the disk for this at all. */
                if (geom_dirty) setting_commit();
                app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                aui_set_size(e.a, e.b); changed = 1;
                char b[16];
                itoa_(e.a, b); setting_set("app.textedit.w", b, 0);
                itoa_(e.b, b); setting_set("app.textedit.h", b, 0);
                geom_dirty = 1;
            }
            if (e.type == EV_THEME)  changed = 1;
            if (e.type == EV_WHEEL)  { scroll += e.wheel; if (scroll < 0) scroll = 0; changed = 1; }
            if (e.type == EV_KEY) {
                if (e.a > 0xFF) continue;      /* arrows/Home/End: navigation, not text */
                char c = (char)e.a;
                if (c == CTRL_S) {
                    if (write_file(fname, text, tlen) >= 0) saved = 1;
                    changed = 1;
                } else if (c == '\b') {
                    if (tlen > 0) text[--tlen] = 0;
                    saved = 0; changed = 1;
                } else if (tlen < MAXT) {
                    text[tlen++] = c; text[tlen] = 0;
                    saved = 0; changed = 1;
                }
            }
        }
        if (changed) draw();
        wait_idle(100);   /* was sys_yield(): a spin. input-driven; the caret blink is drawn from get_time */
    }
}
