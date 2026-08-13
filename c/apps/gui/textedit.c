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
    int w = 520, h = 360;
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
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_RESIZE) { aui_set_size(e.a, e.b); changed = 1; }
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
        sys_yield();
    }
}
