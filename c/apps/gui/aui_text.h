/* aui_text.h -- text drawing declarations.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. */
#ifndef AUI_TEXT_H
#define AUI_TEXT_H

/* text: aui_label is body size (14 / 400); aui_heading is the page heading,
 * title1 22 / 600 (design 3.6, 7.2); aui_title is the toolbar / sheet / card
 * title, title2 17 / 600; aui_text_sz is any size at weight 400. Both titles
 * return the width they drew, in points, measured at their own weight: place
 * an inline subtitle at x + that + gap (never aui_text_w, which measures 400). */
void aui_label(int x, int y, const char *s, unsigned color);
int  aui_heading(int x, int y, const char *s, unsigned color);
int  aui_title(int x, int y, const char *s, unsigned color);
void aui_text_sz(int x, int y, const char *s, unsigned color, int px);
/* A SLICE of a buffer -- `len` bytes from `s`, no NUL needed. This is what a
 * scrolling text view draws: one buffer, a table of (offset, length) lines, and
 * only the visible ones painted. Use it instead of reaching for gui_text_run,
 * which takes WINDOW coordinates and therefore ignores aui_scroll_begin's
 * translation -- see the note at its definition. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px);
/* measured width at size px. The name is also the 6-argument weighted draw
 * (aui_text_w(x, y, px, weight, color, s), below): the macro after the
 * prototypes picks by argument count, and the parentheses keep this
 * declaration (and the definition) out of the macro's reach. */
int  (aui_text_w)(const char *s, int px);
/* Centred / right-aligned inside a rect. Vertically the text is centred by the
 * font's own metrics, the aui_text_vc rule of design 3.6 (baseline = top +
 * (h - 1.448 px) / 2 + 1.160 px): every size sits optically centred in its
 * rect, with no metrics query. */
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

/* ------------------------------------------- weights (design 3.6, P1) ----
 * The weighted forms take a CSS weight: AUI_W_* (400 / 500 / 600 / 700) or 0.
 * With TEXT_RUN2 the weight travels in logit_run2.weight and the kernel picks
 * the baked face (ui, ui-medium, ui-semibold, ui-bold). Without it, weight
 * >= 600 draws the bold face bit and 500 the regular face. Measurement and the
 * carets query use that same resolved face bit: the carets query takes face
 * bits only (no weight), so a 500 is measured regular and a 600 bold.
 * Weight 0 is the legacy call, byte for byte (same syscalls, same arguments).
 * Argument order everywhere: ..., px, weight, color, ... */
#define AUI_W_REGULAR  400
#define AUI_W_MEDIUM   500
#define AUI_W_SEMIBOLD 600
#define AUI_W_BOLD     700

void aui_text_draw_w(int x, int y, int px, int weight, unsigned color, const char *s);
#define AUI_TW_PICK_(a1, a2, a3, a4, a5, a6, name, ...) name
#define aui_text_w(...) AUI_TW_PICK_(__VA_ARGS__, aui_text_draw_w, aui_text_w_takes_2_or_6_args, \
                                     aui_text_w_takes_2_or_6_args, aui_text_w_takes_2_or_6_args, \
                                     (aui_text_w), aui_text_w_takes_2_or_6_args)(__VA_ARGS__)
int  aui_text_measure_w(const char *s, int px, int weight);         /* points */
/* aui_text_in with a weight; the same baseline rule as aui_text_in. */
void aui_text_in_w(struct aui_rect r, const char *s, int px, int weight, unsigned color, int align);
/* aui_text_ellipsis_ex with a weight; mode is AUI_TRUNC_TAIL/MIDDLE/HEAD. */
void aui_text_ellipsis_w(int x, int y, int maxw, const char *s, int px, int weight,
                         unsigned color, int mode);

/* --------------------------------------------- tabular digits (P2) --------
 * Each ASCII digit sits centred in a cell as wide as the widest of '0'..'9'
 * at (px, weight); every other character keeps its natural advance. The cells
 * are cached per (px, weight, scale_percent) in 8 entries, so a scale change
 * re-keys. `align` is AUI_ALIGN_*, anchored on the RIGHT EDGE OF THE LAST
 * DIGIT'S CELL (trailing units hang past x for RIGHT): LEFT starts at x,
 * RIGHT puts that edge at x, CENTER puts x halfway to it. y is the top, as
 * aui_text_sz. aui_text_tnum_width is the whole string's width in points,
 * rounded up. */
void aui_text_tnum(int x, int y, int px, int weight, unsigned color, const char *s, int align);
int  aui_text_tnum_width(const char *s, int px, int weight);

/* --------------------------------------- metric vertical centring (3.6) ---
 * baseline = top + (h - 1.448 px) / 2 + 1.160 px from the ui*.ttf metrics in
 * logit_tokens.h (ascent 1160, descent -288, upem 1000), top and h in 24.8
 * points, px an integer size; one rounding, so it is within 1/2 of 1/256 pt
 * of the exact value. aui_text_vc draws `s` in r on that baseline, aligned
 * horizontally per AUI_ALIGN_*. aui_text_in / aui_text_in_w use the same rule. */
int  aui_text_baseline24(int top24, int h24, int px);
void aui_text_vc(struct aui_rect r, const char *s, int px, int weight, unsigned color, int align);

/* ------------------------------------------ explicit line heights (3.6) ---
 * aui_text_wrap / aui_text_measure_wrap with a weight in place of the face
 * bits and the design's line height: line_h > 0 is used as given; line_h <= 0
 * takes the 3.6 line height of the type style with that px (body 14 -> 20,
 * callout 13 -> 18, ...), never the 5/4 default. aui_text_line_h is that
 * lookup. */
int aui_text_line_h(int px);
int aui_text_wrap_lh(struct aui_rect r, const char *s, int len, int px, int weight,
                     unsigned argb, int line_h, int max_lines, int align, int trunc);
int aui_text_measure_wrap_lh(int w, const char *s, int len, int px, int weight,
                             int line_h, int max_lines);

#endif /* AUI_TEXT_H */
