/* aui_button.h -- buttons and the select control.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_BUTTON_H
#define AUI_BUTTON_H

/* The default button's face without a widget: for a caller that hit-tests
 * its own buttons (Finder's toolbar) and must look like aui_button(). */
void aui_button_face(int x, int y, int w, int h, int radius);

/* Buttons. aui_button keeps the original signature AND the original look
 * (a glass pill), so no existing app changes appearance. */
int aui_button(int x, int y, int w, int h, const char *label);
int aui_button_ex(int x, int y, int w, int h, const char *label,
                  enum aui_variant v, int enabled);
int aui_icon_button(int x, int y, int size, int icon, int enabled);

/* Dropdown. Draws closed; when open it defers its popup to aui_end() so the
 * list paints over everything else in the frame. Returns 1 on a new choice. */
int aui_select(int x, int y, int w, const char *const *items, int n, int *sel);

#endif /* AUI_BUTTON_H */
