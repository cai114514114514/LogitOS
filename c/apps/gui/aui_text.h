/* aui_text.h -- text drawing declarations.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. */
#ifndef AUI_TEXT_H
#define AUI_TEXT_H

/* text: aui_label is body size; aui_heading is title size; aui_text_sz is any. */
void aui_label(int x, int y, const char *s, unsigned color);
void aui_heading(int x, int y, const char *s, unsigned color);
void aui_text_sz(int x, int y, const char *s, unsigned color, int px);
/* A SLICE of a buffer -- `len` bytes from `s`, no NUL needed. This is what a
 * scrolling text view draws: one buffer, a table of (offset, length) lines, and
 * only the visible ones painted. Use it instead of reaching for gui_text_run,
 * which takes WINDOW coordinates and therefore ignores aui_scroll_begin's
 * translation -- see the note at its definition. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px);
int  aui_text_w(const char *s, int px);      /* measured width at size px        */
/* Centred / right-aligned inside a rect. Vertically the text sits on a BASELINE
 * derived from the font's ascent and descent (the metrics query), the same for
 * every size in the same rect, so mixed sizes in one row share a baseline. The
 * metrics are cached per (px, face, scale_percent). */
void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align);
#define AUI_ALIGN_LEFT   0
#define AUI_ALIGN_CENTER 1
#define AUI_ALIGN_RIGHT  2

/* What to elide when text does not fit. Same numbers for aui_text_ellipsis_ex
 * (TAIL, MIDDLE, HEAD) and aui_text_wrap's last line (plus NONE: just stop). */
#define AUI_TRUNC_TAIL   0
#define AUI_TRUNC_MIDDLE 1
#define AUI_TRUNC_HEAD   2
#define AUI_TRUNC_NONE   3

/* Draw `s` fitted to `maxw` points, replacing what does not fit with U+2026.
 * With the kernel's CARETS and TEXT_RUN2 features the cut is on a grapheme
 * boundary (never inside a UTF-8 sequence) and costs at most two measure
 * syscalls; otherwise it falls back to a code-point-safe tail cut with "...".
 * aui_text_ellipsis is the TAIL form. */
void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px);
void aui_text_ellipsis_ex(int x, int y, int maxw, const char *s, unsigned color, int px, int mode);

/* Wrapped text. `s` is NUL-terminated when len < 0. `face` is LOGIT_FACE_*;
 * line_h is the line pitch in points (<= 0: px * 5/4); max_lines <= 0 means as
 * many as fit in r.h (at most 64). The last allowed line is cut per `trunc`
 * (AUI_TRUNC_*); `align` is AUI_ALIGN_*. Text beyond 1023 bytes is not drawn.
 * aui_text_wrap returns the number of lines drawn; aui_text_measure_wrap the
 * height (lines * line_h) the same wrap needs at width w points. */
int aui_text_wrap(struct aui_rect r, const char *s, int len, int px, unsigned face,
                  unsigned argb, int line_h, int max_lines, int align, int trunc);
int aui_text_measure_wrap(int w, const char *s, int len, int px, unsigned face,
                          int line_h, int max_lines);

/* Text with an alpha 0..255 (255 is opaque), through SYS_GUI_TEXT_RUN2. A kernel
 * without it draws the colour mixed toward the background instead. */
void aui_text_a(int x, int y, const char *s, unsigned color, int px, int alpha);

#endif /* AUI_TEXT_H */
