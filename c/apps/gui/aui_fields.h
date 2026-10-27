/* aui_fields.h -- text field, scrollbar, scroll containers.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_FIELDS_H
#define AUI_FIELDS_H

/* Text field. Full editing: caret, click-to-place, shift-select, drag-select,
 * Home/End/arrows, backspace/delete, and Ctrl+A. Returns 1 on Enter. */
int aui_textfield(int x, int y, int w, char *buf, int cap);
int aui_textfield_ex(int x, int y, int w, char *buf, int cap,
                     const char *placeholder, int enabled);

/* Scrollbar. `off` is the scroll offset in content units; returns 1 when the
 * offset changed (drag or click on the trough). Wheel belongs to the container. */
int aui_scrollbar(int x, int y, int h, int *off, int content, int view);

/* A clipped, scrollable container. Between begin and end, draw in CONTENT
 * coordinates -- aui translates and clips for you. Wheel movement uses the
 * OpenLogit transition sampler; *off always holds the displayed offset, so
 * existing visible-row culling remains valid. Keep off's address stable across
 * frames. Changes to *off and thumb drags take effect immediately.
 * The rightmost 12 points are reserved when an overflowing scrollbar fits.
 * Use aui_anim_due()/aui_anim_wait() in the event loop (as for other widgets).
 * Reduce Motion skips interpolation; inactive/absent containers request no wakes.
 *
 *     static int sc;
 *     aui_scroll_begin(x, y, w, h, &sc, content_h);
 *     for (i...) aui_label(0, i*20, item[i], AUI_TEXT);
 *     aui_scroll_end();
 */
void aui_scroll_begin(int x, int y, int w, int h, int *off, int content_h);
/* The same, clipped to the container's rounded corners (`radius` points; the
 * side that carries the scrollbar stays square). Up to 8 containers nest. */
void aui_scroll_begin_round(int x, int y, int w, int h, int *off, int content_h, int radius);
void aui_scroll_end(void);
/* The displayed offset (points) of scroll container `id`: the id-th
 * aui_scroll_begin of the frame, counting from 1. Read-only. Before that
 * container has been opened in the current frame (a toolbar drawn above its
 * content) it answers last frame's value; 0 for an id no container had. */
int aui_scroll_offset(int id);

#endif /* AUI_FIELDS_H */
