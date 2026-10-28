/* aui_fields.h -- text field, scrollbar, scroll containers.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_FIELDS_H
#define AUI_FIELDS_H

/* ---------------------------------------------------------------- fields --
 * Text field (design 4.4): h 28, radius 8, padding 10, body 14 regular. White
 * over the window in light, white @ 13 in dark, a 1 device pixel `sep.strong`
 * rim. Full editing: caret, click-to-place, shift-select, drag-select,
 * Home/End/arrows, backspace/delete and Ctrl+A; the text slides so the caret
 * stays in the box. Returns 1 on Enter.
 *
 * The caret is 2 device pixels by 18 pt in the accent. It is solid for 530 ms
 * after every keystroke or click, then blinks 530 off / 530 on with a 90 ms
 * fade, only while the field has focus; its damage is its own rect (it is one
 * draw call). After 8 s without input it is solid again and the field asks
 * for no more wakes. Reduce Motion keeps it solid.
 *
 * Focus (a click or Tab, any focus) turns the rim `accent` and adds the
 * `accent.focusRing` ring. The selection is `accent.selText`. */
int aui_textfield(int x, int y, int w, char *buf, int cap);
/* The same with a placeholder (`label.tertiary`, shown while the field is empty). */
int aui_textfield_ph(int x, int y, int w, char *buf, int cap, const char *placeholder);
/* The general form, picked by argument count:
 *   aui_textfield_ex(x, y, w, buf, cap, placeholder, enabled)
 *   aui_textfield_ex(x, y, w, buf, cap, placeholder, enabled, error)
 *   aui_textfield_ex(x, y, w, buf, cap, placeholder, enabled, error, flags)
 * `enabled` 0 draws NO box: the value shows as plain `label.secondary` text.
 * A non-NULL `error` makes the field invalid: an `error` rim and the message
 * in `error.ink` 12 pt, 4 pt below the box (the caller leaves the room).
 * `flags`: AUI_TF_LARGE is the 36 pt field with radius 10. */
#define AUI_TF_LARGE 1
int (aui_textfield_ex)(int x, int y, int w, char *buf, int cap,
                       const char *placeholder, int enabled);
int aui_textfield_exe(int x, int y, int w, char *buf, int cap,
                      const char *placeholder, int enabled, const char *error);
int aui_textfield_exf(int x, int y, int w, char *buf, int cap,
                      const char *placeholder, int enabled, const char *error, int flags);
#define AUI_TFX_PICK_(a1, a2, a3, a4, a5, a6, a7, a8, a9, name, ...) name
#define aui_textfield_ex(...) AUI_TFX_PICK_(__VA_ARGS__, aui_textfield_exf, aui_textfield_exe, \
                                            (aui_textfield_ex), aui_textfield_ex_takes_7_to_9_args, \
                                            aui_textfield_ex_takes_7_to_9_args, \
                                            aui_textfield_ex_takes_7_to_9_args)(__VA_ARGS__)

/* Search field (design 4.4): a capsule h 28 in `fill.control`, no rim, the
 * SEARCH glyph at x 10 and the text at x 30 (the placeholder defaults to
 * "搜索"). A clear button (a 16 pt disc, 24 x 24 hit zone) appears while the
 * field is non-empty. Esc clears, and with nothing left to clear it blurs.
 * Returns 1 on Enter. `flags` is reserved (0). */
int aui_searchfield(int x, int y, int w, char *buf, int cap, int flags);
int aui_searchfield_ph(int x, int y, int w, char *buf, int cap, int flags,
                       const char *placeholder);

/* Overlay scrollbar (design 4.12). `x` is the left of its 12 pt hit zone, y/h
 * its track; the 6 pt thumb (8 under the pointer) floats 2 pt in from the zone's
 * right edge. Hidden at rest, shown while `off` moves or the pointer is in the
 * zone, fading over 200 ms after 800 ms idle. `off` is the scroll offset in
 * content units; returns 1 when the offset changed (drag or click on the track
 * of a visible bar). Wheel belongs to the container. */
int aui_scrollbar(int x, int y, int h, int *off, int content, int view);

/* A clipped, scrollable container. Between begin and end, draw in CONTENT
 * coordinates -- aui translates and clips for you. Wheel movement uses the
 * OpenLogit transition sampler; *off always holds the displayed offset, so
 * existing visible-row culling remains valid. Keep off's address stable across
 * frames. Changes to *off and thumb drags take effect immediately.
 * There is no gutter: content runs the full width and the rightmost 12 points
 * are only the scrollbar's hit zone (the overlay bar of design 4.12 floats over
 * the content).
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
