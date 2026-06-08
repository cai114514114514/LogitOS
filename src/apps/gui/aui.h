#ifndef AUI_H
#define AUI_H
#include "aether.h"

/* Aether immediate-mode UI toolkit, layered on the gui_* syscalls.
 *
 * Aether's event model delivers click-downs (EV_MOUSE: a=x, b=y, window-local) and
 * keys (EV_KEY: a=char or KEY_*) -- no hover/move/drag. So widgets are *drawn and
 * handled in the same call*, and the app re-runs its frame after each event:
 *
 *     void app_main(void) {
 *         gui_create("Demo", 320, 240);
 *         frame();                            // initial paint
 *         struct aether_event e;
 *         for (;;) {
 *             if (!poll_event(&e)) { sys_yield(); continue; }
 *             if (e.type == EV_CLOSE) app_exit(0);
 *             aui_feed(&e); frame(); aui_feed_done();
 *         }
 *     }
 *     // frame(): aui_begin(AUI_BG); ...widgets...; aui_end();
 */

/* theme */
#define AUI_BG      rgb(244, 245, 248)
#define AUI_TEXT    rgb(40, 42, 50)
#define AUI_MUTED   rgb(140, 144, 154)
#define AUI_ACCENT  rgb(64, 130, 246)
#define AUI_FACE    rgb(232, 234, 240)

void aui_feed(const struct aether_event *e);   /* stash the event for the coming frame */
void aui_feed_done(void);                    /* clear it after the frame */

void aui_begin(unsigned bg);                 /* reset widget ids + clear the window */
void aui_end(void);                          /* present */

void aui_label(int x, int y, const char *s, unsigned color);
void aui_panel(int x, int y, int w, int h, unsigned color);
int  aui_button(int x, int y, int w, int h, const char *label);   /* 1 on click */
int  aui_checkbox(int x, int y, const char *label, int *state);   /* 1 when toggled */
int  aui_textfield(int x, int y, int w, char *buf, int cap);      /* 1 on Enter */

#endif /* AUI_H */
