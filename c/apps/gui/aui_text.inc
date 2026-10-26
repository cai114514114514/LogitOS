/* aui_text.inc -- text drawing: label, heading, sized/sliced/aligned/ellipsised text.
 * Unity-included by aui.c (one translation unit): every static it names is
 * file-scope in aui.c, and the variant builds (-DAUI_NO_AA, -DAUI_COST, ...)
 * reach it automatically. Moved verbatim; do not compile on its own. */

/* ------------------------------------------------------------------ text */

#define PX AUI_FS_BODY

static int tw(const char *s) { return text_measure_px(s, slen(s), PX, 0); }
static int twn(const char *s, int n) { return n <= 0 ? 0 : text_measure_px(s, n, PX, 0); }
static void txt(int x, int y, unsigned c, const char *s)
{ gui_text_run(X_(x), Y_(y), PX, 0, c, s, slen(s)); }

int  aui_text_w(const char *s, int px) { return text_measure_px(s, slen(s), px, 0); }
void aui_text_sz(int x, int y, const char *s, unsigned color, int px)
{ gui_text_run(X_(x), Y_(y), px, 0, color, s, slen(s)); }
/* A SLICE of a buffer, not a NUL-terminated string. Every text view that
 * scrolls has this shape -- one big buffer, a table of (offset, length) lines,
 * and only the visible ones drawn -- and until this existed the only way to
 * draw one was to call gui_text_run directly. That is a trap, because
 * gui_text_run takes WINDOW coordinates and every aui text call takes
 * CONTAINER ones: inside aui_scroll_begin the two differ by the container's
 * origin AND by the scroll offset, so the labels moved with the scroll and the
 * text did not. The symptom was a transcript whose role headings sat two rows
 * away from the text they belonged to, which reads as a wrapping bug and is
 * not one. Found by c/apps/gui/ch.c, the first scrolling text view in the tree
 * that is not the browser's own painter. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px)
{ if (len > 0) gui_text_run(X_(x), Y_(y), px, 0, color, s, len); }
void aui_heading(int x, int y, const char *s, unsigned color) { aui_text_sz(x, y, s, color, AUI_FS_TITLE); }
void aui_label(int x, int y, const char *s, unsigned color) { txt(x, y, color, s); }

void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align)
{
    int w = aui_text_w(s, px);
    int x = r.x;
    if (align == AUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
    else if (align == AUI_ALIGN_RIGHT) x = r.x + r.w - w;
    aui_text_sz(x, r.y + (r.h - px) / 2 - 1, s, color, px);
}

void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px)
{
    int n = slen(s);
    if (text_measure_px(s, n, px, 0) <= maxw) { aui_text_sz(x, y, s, color, px); return; }
    int ew = text_measure_px("...", 3, px, 0);
    int lo = 0, hi = n;
    while (lo < hi) {                                  /* binary search, not a scan:
                                                        * each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (text_measure_px(s, mid, px, 0) + ew <= maxw) lo = mid; else hi = mid - 1;
    }
    gui_text_run(X_(x), Y_(y), px, 0, color, s, lo);
#ifdef AUI_SPLIT_NEGCTL            /* tests/aui-split.mk: must change the object hash */
    gui_text_run(X_(x) + text_measure_px(s, lo, px, 0), Y_(y), px, 0, color, "..", 2);
#else
    gui_text_run(X_(x) + text_measure_px(s, lo, px, 0), Y_(y), px, 0, color, "...", 3);
#endif
}
