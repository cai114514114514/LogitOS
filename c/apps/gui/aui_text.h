/* aui_text.h -- text drawing declarations.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. Declarations moved verbatim. */
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
/* Centred / right-aligned inside a rect, vertically centred on the cap height. */
void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align);
#define AUI_ALIGN_LEFT   0
#define AUI_ALIGN_CENTER 1
#define AUI_ALIGN_RIGHT  2
/* Draw `s` fitted to `maxw`, appending an ellipsis when it does not fit. */
void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px);

#endif /* AUI_TEXT_H */
