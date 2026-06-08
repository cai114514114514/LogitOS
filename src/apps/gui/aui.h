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

/* Theme: semantic color tokens resolved at runtime from the active theme, so an
 * app can flip light/dark (or restyle the accent) without touching widget code.
 * The token macros below read `aui_t`, so existing `aui_begin(AUI_BG)` etc. keep
 * working -- they were already runtime expressions (rgb() is an inline fn). */
struct aui_theme {
    unsigned bg;          /* window background          */
    unsigned surface;     /* raised surface (cards, fields) */
    unsigned face;        /* control face (button)      */
    unsigned text;        /* primary text               */
    unsigned muted;       /* secondary text             */
    unsigned border;      /* hairline / control edge    */
    unsigned hi;          /* top-edge highlight         */
    unsigned accent;      /* brand / selection          */
    unsigned accent_text; /* text on an accent fill     */
    unsigned success;     /* green   */
    unsigned warning;     /* amber   */
    unsigned error;       /* red     */
    unsigned focus;       /* focus ring */
};

extern struct aui_theme aui_t;        /* the active theme (light at startup) */
void     aui_ensure(void);            /* fill aui_t on first use (idempotent) */
void     aui_set_dark(int on);        /* swap the whole palette light<->dark */
int      aui_is_dark(void);
unsigned aui_hsl(int h, int s, int l);   /* h:0..359 s,l:0..100 -> packed rgb */
void     aui_set_accent(unsigned color); /* recolor the accent + focus tokens   */

/* Each token lazy-inits the theme (comma operator) before reading, so even the
 * very first `aui_begin(AUI_BG)` -- whose argument is evaluated at the call site
 * -- sees a populated palette rather than zeroed (black) fields. */
#define AUI_BG          (aui_ensure(), aui_t.bg)
#define AUI_SURFACE     (aui_ensure(), aui_t.surface)
#define AUI_FACE        (aui_ensure(), aui_t.face)
#define AUI_TEXT        (aui_ensure(), aui_t.text)
#define AUI_MUTED       (aui_ensure(), aui_t.muted)
#define AUI_BORDER      (aui_ensure(), aui_t.border)
#define AUI_HI          (aui_ensure(), aui_t.hi)
#define AUI_ACCENT      (aui_ensure(), aui_t.accent)
#define AUI_ACCENT_TEXT (aui_ensure(), aui_t.accent_text)
#define AUI_SUCCESS     (aui_ensure(), aui_t.success)
#define AUI_WARNING     (aui_ensure(), aui_t.warning)
#define AUI_ERROR       (aui_ensure(), aui_t.error)
#define AUI_FOCUS       (aui_ensure(), aui_t.focus)

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
