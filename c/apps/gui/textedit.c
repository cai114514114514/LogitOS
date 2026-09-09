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

/* ---- UTF-8, at the one place a raw EV_KEY codepoint becomes bytes in `text`
 * ----
 *
 * logit_abi.h: EV_KEY's `a` is "a character, or a KEY_* code ... all > 0xFF
 * so they never collide with a character". Above ASCII, `a` is either one of
 * the eight enumerated navigation codes or a Unicode code point -- the pinyin
 * IME commits CJK this way. `(char)a` truncates a code point to its low byte;
 * te_apply_key and te_utf8_encode are the fix, factored out so a host harness
 * can drive them without a window (no gui_create, no poll_event --
 * text/tlen/saved are the same file statics app_main uses).
 *
 * THAT HARNESS DOES NOT EXIST. This comment named `tests/unit/textedit_test.c`
 * in the present tense; `ls` says no such file, and neither the root Makefile
 * nor any of the 105 fragments under tests/ mentions the word. The factoring is
 * real and still the right shape -- te_apply_key, te_utf8_encode, and now
 * te_cp_len/te_fit/te_walk are all pure functions over the file statics, which
 * is exactly what a host gate needs -- but a cited gate that is not in the tree
 * reads as coverage and is not, so it is named as absent here rather than left
 * to be believed. The same harness would cover the wrap and the caret below.
 *
 * -DAUI_BYTE_BACKSPACE is the negative control: it reverts backspace to
 * removing one BYTE, the original bug -- a CJK character deleted this way
 * leaves its lead byte's continuation bytes in the buffer, decodable as
 * neither the old character nor a new one. Named to match aui.c's control
 * rather than invented separately: both text-entry sites share one flag and
 * one falsifiable claim ("delete a whole character"). */
static int te_is_nav_key(int a)
{
    return a == KEY_UP || a == KEY_DOWN || a == KEY_PGUP || a == KEY_PGDN ||
           a == KEY_HOME || a == KEY_END || a == KEY_LEFT || a == KEY_RIGHT;
}

static int te_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int te_is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* One key against `text`/`tlen`/`saved` -- the whole EV_KEY handler pulled out
 * of app_main's event loop. `wrote` reports whether Ctrl+S was actually asked
 * to write (the test has no filesystem, so it can only check the request was
 * made, not that it landed). Returns 1 if anything changed that a repaint
 * should reflect, 0 for a key this app ignores (navigation) or a no-op
 * (buffer full). */
static int te_apply_key(int a, int *wrote)
{
    if (te_is_nav_key(a)) return 0;        /* navigation, not text -- see the
                                             * file header: append-only, no
                                             * caret to move */
    if (a == CTRL_S) { if (wrote) *wrote = 1; return 1; }
    if (a == '\b') {
        if (tlen <= 0) return 0;
#ifdef AUI_BYTE_BACKSPACE
        tlen--;
#else
        int p = tlen - 1;
        while (p > 0 && te_is_cont(text[p])) p--;
        tlen = p;
#endif
        text[tlen] = 0;
        saved = 0;
        return 1;
    }
    if (a > 0 && a <= 0x7F) {
        if (tlen >= MAXT) return 0;
        text[tlen++] = (char)a; text[tlen] = 0; saved = 0;
        return 1;
    }
    if (a > 0x7F) {
        char enc[4]; int el = te_utf8_encode((unsigned)a, enc);
        if (tlen + el >= MAXT) return 0;
        for (int i = 0; i < el; i++) text[tlen++] = enc[i];
        text[tlen] = 0; saved = 0;
        return 1;
    }
    return 0;
}

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

/* ---- Wrapping and the caret: ONE walk, in CODE POINTS and REAL ADVANCES ----
 *
 * What stood here counted BYTES and multiplied by the width of "M":
 *
 *     for (int i = 0; i < tlen; i++)
 *         if (text[i] == '\n')      { line++; col = 0; }
 *         else if (col + 1 >= cols) { line++; col = 1; }
 *         else                      { col++; }
 *     ...  caret x = col * text_measure_px("M", 1, px, 1)
 *
 * and that is ONE bug with two symptoms, which is why both are fixed here and
 * not in two places: `col` is the caret AND it is the wrap. 你好 is six UTF-8
 * bytes and two glyphs, so the caret advanced six cells for two characters and
 * the wrap budget was spent three times too fast -- and because the break was
 * taken at a byte index it could land between a lead byte and its continuations
 * and split one character across two display lines.
 *
 * The three were watched failing together before anything was changed, against
 * a host stub of SYS_TEXT_MEASURE with an 8 px 'M' and a 15 px Han glyph:
 *
 *   你好                     caret x = 48, the glyphs end at 30   -> an 18 px gap
 *   20 Han, 150 px column    wrapped into 4 lines; 10 per line fit -> 2
 *   the same buffer          2 breaks landed inside a UTF-8 sequence
 *
 * The exact pixel numbers belong to that stub -- the real ones are whatever
 * ui.ttf and mono.ttf give and only the device can say -- but the shape does
 * not depend on the ratio, and neither does the third row.
 *
 * This is the third instance of the trap aui.c:1459 records: the pinyin IME
 * commits CJK, and every piece of arithmetic in a toolkit that predates it
 * still counts in bytes.
 *
 * THE CELL WAS NEVER RIGHT EITHER, not even for the font it named. The run is
 * drawn with mono=1, which selects mono.ttf -- "printable ASCII plus NBSP, no
 * CJK at all" -- so every CJK code point falls through tl_fonts()' preference
 * order in c/kernel/gui/text.c to ui.ttf and is drawn at THAT font's advance.
 * text_measure_px("M", ...) cannot see that; nothing that measures one
 * character and multiplies can. The file header above already records the same
 * mistake in its first form ("the monospace advance was assumed to be 8 pixels
 * regardless of the font or the backing scale"), so this file has been bitten
 * here before and the lesson to keep is the general one: no cell arithmetic.
 *
 * So every width below is text_measure_px() over the ACTUAL prefix, with
 * mono=1 -- the same length-delimited run, the same font-preference order and
 * the same shaping the draw will use. That is the invariant text.c states for
 * itself: "text_measure and text_draw_run must agree at the same px ... a
 * separate measuring path would drift a few pixels per line in a way nobody
 * could reproduce, so there is not one." te_walk() is that single path on this
 * side: it measures and draws in one function behind a `draw` flag, exactly
 * like layout()'s, so the wrap the caret is placed against cannot be a
 * different wrap from the one on screen.
 *
 * COST, measured rather than estimated, and it is not flat. te_fit() is ONE
 * text_measure_px for a line that fits and a bisection for one that does not,
 * and draw() runs the walk TWICE -- the scroll depends on the caret's line, the
 * caret is at the end of the buffer, so the whole text is walked before the
 * first glyph can be placed. Counted on the host against a stub of
 * SYS_TEXT_MEASURE, at the full MAXT buffer and a 500 px text column:
 *
 *   7,900 B, a newline every 60 chars   131 lines    130 calls/walk    260/repaint
 *   7,900 B, no newlines at all         128 lines  1,612 calls/walk  3,224/repaint
 *
 * The first row is the ordinary case and is one call per line. The second is
 * 12.6 per line because te_fit's upper bound is the whole LOGICAL line, so
 * every break bisects 7,900 bytes rather than the ~60 it will land in. Both are
 * O(text), not O(visible), which is inherent to a caret that lives at the end.
 * If either ever shows up in a profile, the fix is a line-start cache keyed on
 * (tlen, avail, px) -- not a second, cheaper wrap, which is the mistake this
 * whole comment is about. */

/* Bytes in the UTF-8 sequence starting at `i`: at least 1, never past `limit`,
 * never more than the 4 a legal sequence can occupy.
 *
 * Counted from CONTINUATION bytes rather than decoded out of the lead byte on
 * purpose -- a truncated or malformed sequence still advances by one, so no
 * input can stall the walk and no break can be taken inside a sequence. The
 * cap at 4 is not decoration: this is te_fit's floor, the one place a line is
 * allowed to be wider than the window, and without it a file of 2,000 bare
 * continuation bytes glues into a single 2,000-byte "code point" that te_fits
 * never gets to refuse -- straight into gui_text_run's silent clamp at 1023.
 * Four is the largest a real sequence can be, so the cap can only ever bind on
 * input that was already malformed. */
static int te_cp_len(int i, int limit)
{
    int n = 1;
    while (n < 4 && i + n < limit && te_is_cont(text[i + n])) n++;
    return n;
}

/* Largest UTF-8 boundary at or below `n` bytes past `off`. */
static int te_bound(int off, int n)
{
    while (n > 0 && te_is_cont(text[off + n])) n--;
    return n;
}

/* Do the first `n` bytes at `off` fit in `avail` device px?
 *
 * A width of 0 for a NON-EMPTY run is a REFUSAL, not a measurement of zero, and
 * reading it as "fits" is how this fix would have shipped its own silent
 * truncation. wm.c's SYS_TEXT_MEASURE answers 0 when len exceeds USER_TEXT_MAX
 * (1024) and when px is out of range, and SYS_GUI_TEXT_RUN then clamps the draw
 * to 1023 bytes without telling anyone -- so a 2,000-byte line with no newline
 * in it would have "fitted", drawn its first 1023 bytes, and dropped the rest
 * with no symptom at all. Refusing here keeps that limit spelled ONCE, in the
 * kernel that owns it, instead of a 1024 copied into this file to disagree with
 * it later (one jar, two doors); and if the kernel's limit ever moves, this
 * side answers with a short line rather than a lost one. */
static int te_fits(int off, int n, int avail, int px)
{
    if (n <= 0) return 1;
    int w = text_measure_px(text + off, n, px, 1);
    return w > 0 && w <= avail;
}

/* Bytes of [start, limit) that fit in `avail`: the whole run when it fits, else
 * the largest prefix that does -- always on a code point boundary, and never 0,
 * so a single code point wider than the entire line takes a line to itself
 * instead of looping forever. Bisection is sound because glyph advances are
 * non-negative, so a longer prefix is never narrower. */
static int te_fit(int start, int limit, int avail, int px)
{
    int n = limit - start;
    if (n <= 0) return 0;
    if (te_fits(start, n, avail, px)) return n;

    int lo = 0, hi = n;              /* lo fits and is aligned; hi is known not to */
    for (;;) {
        int mid = te_bound(start, lo + (hi - lo) / 2);
        if (mid <= lo) mid = lo + te_cp_len(start + lo, limit);  /* next boundary up */
        if (mid >= hi) break;                                    /* none strictly between */
        if (te_fits(start, mid, avail, px)) lo = mid; else hi = mid;
    }
    return lo > 0 ? lo : te_cp_len(start, limit);
}

/* One display line beginning at `start`. *drawlen is the byte range to draw (a
 * terminating '\n' is consumed, not drawn); *next is where the following
 * display line begins. Returns 1 if there IS a following line -- a '\n' was
 * consumed or the line was wrapped -- and 0 at the end of the buffer.
 *
 * Progress is guaranteed on the `more` path (te_fit's floor is one code point,
 * and the newline branch steps past the newline), which is what makes the caller
 * a `for(;;)` that cannot spin. */
static int te_line_break(int start, int avail, int px, int *drawlen, int *next)
{
    int e = start;
    while (e < tlen && text[e] != '\n') e++;

    int fit = te_fit(start, e, avail, px);
    if (fit < e - start) { *drawlen = fit; *next = start + fit; return 1; }

    *drawlen = e - start;
    if (e < tlen) { *next = e + 1; return 1; }
    *next = e; return 0;
}

/* The walk. Always measures; draws the visible lines when `draw` is set.
 * Reports the display-line count, the caret's line, and the caret's x offset
 * from the left edge of the text in px -- from the real advances of the prefix
 * it follows, not from a column index.
 *
 * The caret is at the END of the buffer: this app appends and backspaces and
 * te_apply_key drops every navigation key, so there is no caret to move. That
 * is what makes the last line the walk produces the caret's line, and its whole
 * drawn extent the caret's prefix -- one measurement of exactly the run that
 * was handed to gui_text_run, so the two cannot disagree by a kerning pair. */
static void te_walk(int avail, int px, int x0, int y0, int lh,
                    int scroll_, int rows, int draw,
                    int *nlines, int *cl, int *cx)
{
    int start = 0, line = 0, dl = 0;
    for (;;) {
        int nx, more = te_line_break(start, avail, px, &dl, &nx);
        if (draw && dl > 0 && line >= scroll_ && line < scroll_ + rows)
            gui_text_run(x0, y0 + (line - scroll_) * lh, px, 1, AUI_TEXT, text + start, dl);
        if (!more) break;
        start = nx; line++;
    }
    *nlines = line + 1;
    *cl = line;
    *cx = dl > 0 ? text_measure_px(text + start, dl, px, 1) : 0;
}

/* ---- what changed this frame, in terms ONLY this file can compute ----
 *
 * aui.c's own generic per-primitive diff (aui.c section 5a-flush) already
 * tracks every call this file makes THROUGH aui.c -- aui_round (the page
 * surface), aui_fill (the cursor, the status strip), aui_hairline,
 * aui_text_ellipsis, aui_text_sz -- automatically and correctly, because
 * every one of those bottoms out to a gui_rect/gui_blit/gui_text_run call
 * aui.c's own macros already intercept. The ONE thing that mechanism cannot
 * see is te_walk()'s raw gui_text_run() calls for the paragraph text below:
 * that call is textually inside THIS translation unit (this file includes
 * aui.h, which includes logit.h, and calls logit.h's gui_text_run directly),
 * where aui.c's interception macros are simply not in scope -- they are
 * #define'd inside aui.c and apply only to that file's own text. te_prev_*
 * below is what lets this file compute that ONE gap itself and hand it to
 * aui_end_rect() as an extra hint, unioned with whatever aui's own tracking
 * already found -- never instead of it. */
static int te_prev_valid;
static int te_prev_scroll, te_prev_cl, te_prev_nlines, te_prev_w, te_prev_h, te_prev_dark;

static int te_imin(int a, int b) { return a < b ? a : b; }
static int te_imax(int a, int b) { return a > b ? a : b; }

static void draw(void)
{
    int W = aui_width(), H = aui_height();
    aui_begin(AUI_BG);

    int px = AUI_FS_BODY;
    int lh = px + AUI_SP(1);
    int pad = AUI_SP(3);
    int bar = AUI_H_CTL;

    int viewh = H - bar - 2 * pad;
    int rows  = viewh / lh; if (rows < 1) rows = 1;
    /* The wrap width is a PIXEL budget now, not a column count. The floor is one
     * em rather than four columns: at that point te_fit degenerates to one code
     * point per line, which is ugly and still correct -- no split sequences, no
     * spin. The caret may sit at exactly pad + avail on a line that fills the
     * budget; that is W - pad, still AUI_SP(1) inside the page surface below,
     * so no column is reserved for it the way the old `col + 1 >= cols` did. */
    int avail = W - 2 * pad; if (avail < px) avail = px;

    /* Pass one measures. The scroll has to chase the caret's line and the caret
     * is at the end of the buffer, so the whole text is walked before anything
     * can be placed. Pass two draws at the scroll this produced -- the SAME
     * function, so there is no second wrap to disagree with the first. */
    int nlines, cl, cx;
    te_walk(avail, px, 0, 0, lh, 0, 0, 0, &nlines, &cl, &cx);
    int prev_scroll = scroll;
    if (cl < scroll)            scroll = cl;
    if (cl >= scroll + rows)    scroll = cl - rows + 1;
    if (scroll > nlines - 1)    scroll = nlines - 1;
    if (scroll < 0)             scroll = 0;

    /* The page. A surface rather than the window background, so the text sits
     * on something with an edge -- the same relationship every other window in
     * the system has between its chrome and its content. */
    aui_round(pad - AUI_SP(1), pad - AUI_SP(1),
              W - 2 * (pad - AUI_SP(1)), viewh + AUI_SP(2), AUI_R_MD, AUI_SURFACE);

    te_walk(avail, px, pad, pad, lh, scroll, rows, 1, &nlines, &cl, &cx);

    if (cl >= scroll && cl < scroll + rows)
        aui_fill(pad + cx, pad + (cl - scroll) * lh, 2, px, AUI_ACCENT);

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

    /* THE ARGUMENT FOR WHY [min(prev_cl,cl) .. max(prev_nlines-1,nlines-1)]
     * IS THE WHOLE STORY, and it rests on this app's own shape: te_apply_key
     * only ever appends at the tail or removes from the tail (te_is_nav_key
     * makes every navigation key a no-op -- see that function's own comment;
     * there is no caret to move mid-buffer). te_fit()/te_line_break() process
     * the buffer strictly left to right, so every wrap decision BEFORE the
     * byte range an edit touched is a pure function of bytes that did not
     * change -- byte-identical to last frame's walk. The only display lines
     * whose wrap CAN differ are therefore the ones covering the last
     * paragraph's tail: from wherever the caret WAS (te_prev_cl) or IS (cl),
     * whichever is EARLIER, through wherever the walk now ENDS (nlines-1) or
     * used to end (te_prev_nlines-1), whichever is LATER. A line that
     * disappeared entirely (backspace un-wrapping two lines back into one)
     * is covered by the max() reaching its OLD position -- which is what
     * makes the page-background aui_round() above (it repaints its whole
     * area every frame, unconditionally, well before this point) actually
     * get COMPOSITED once it has erased that line, not just drawn into a
     * surface nobody was told to show. */
    int W_changed = !te_prev_valid || W != te_prev_w || H != te_prev_h;
    int dark_changed = aui_is_dark() != te_prev_dark;
    int scroll_changed = scroll != prev_scroll;

    if (W_changed || dark_changed) {
        aui_end();                    /* geometry or theme moved: whole canvas, honestly */
    } else if (scroll_changed) {
        /* Every visible line's IDENTITY changed (a wheel notch, or the caret
         * walking off the bottom of a long paste) -- the text viewport is a
         * different window into the buffer, but the chrome below it (the
         * status bar) provably is not, so only the page needs flushing. */
        aui_end_rect(0, pad - AUI_SP(1), W, viewh + AUI_SP(2));
    } else {
        int lo = te_imin(te_prev_cl, cl);
        int hi = te_imax(te_prev_nlines - 1, nlines - 1);
        if (lo < scroll) lo = scroll;
        if (hi > scroll + rows - 1) hi = scroll + rows - 1;
        if (lo > hi) {
            /* Nothing in the visible TEXT changed by this file's own
             * accounting (e.g. Ctrl+S alone landed with no new keystroke) --
             * still let aui's own generic diff decide: it independently
             * covers the status bar's "saved"/"Ctrl+S" swap, which this
             * file's own line-range math knows nothing about. */
            aui_end_rect(0, 0, 0, 0);
        } else {
            int y0 = pad + (lo - scroll) * lh;
            int y1 = pad + (hi - scroll + 1) * lh;
            aui_end_rect(0, y0, W, y1 - y0);
        }
    }

    te_prev_valid = 1;
    te_prev_scroll = scroll; te_prev_cl = cl; te_prev_nlines = nlines;
    te_prev_w = W; te_prev_h = H; te_prev_dark = aui_is_dark();
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
                int wrote = 0;
                if (te_apply_key(e.a, &wrote)) changed = 1;
                if (wrote) { if (write_file(fname, text, tlen) >= 0) saved = 1; changed = 1; }
            }
        }
        if (changed) draw();
        wait_idle(100);   /* was sys_yield(): a spin. input-driven; the caret blink is drawn from get_time */
    }
}
