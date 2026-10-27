/* aui_overlay.h -- in-window menu bar and modal dialog.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect / enum aui_variant from there. Declarations moved
 * verbatim from aui.h (ui-aui-core). */
#ifndef AUI_OVERLAY_H
#define AUI_OVERLAY_H

/* A menu bar: `titles` across the top, each opening `items[i]` (a NULL-
 * terminated array of NUL-terminated strings; "-" is a separator). Writes the
 * chosen menu index into *mi and the item index into *ii, and returns 1. */
int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii);

/* Modal dialog. Returns 1 while it is open; draw its content between the two
 * calls, in dialog-local coordinates. The scrim swallows clicks behind it. */
int  aui_dialog_begin(const char *title, int w, int h);
void aui_dialog_end(void);
/* The standard footer: buttons right-aligned. Returns the index pressed, or -1.
 * Enter picks item 0 (the default), Escape picks the last one (the cancel). */
int  aui_dialog_buttons(const char *const *labels, int n);

#endif /* AUI_OVERLAY_H */
