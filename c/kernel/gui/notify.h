#ifndef LOGIT_NOTIFY_H
#define LOGIT_NOTIFY_H

/* Transient notifications: how the system tells the user something without
 * taking the screen away from them.
 *
 * The presentation (where, how long, what happens when several arrive, how one
 * is dismissed) is specified above SYS_NOTIFY in include/abi/logit_abi.h; the
 * reasoning about what it COSTS to draw is above notify_compose() in notify.c.
 *
 * Like clipboard.c there is no notify_init(): zeroed statics mean "nothing is
 * showing", so the service is live from boot with no hook in anybody's init.
 *
 * ---------------------------------------------------------------------------
 * WM-HOOKS -- the whole of what this asks of c/kernel/gui/wm.c
 * ---------------------------------------------------------------------------
 * wm.c belongs to the window-management line. This is the complete list of what
 * the notification overlay needs from it: FIVE statements and one #include.
 * Nothing here reaches into wm.c's statics and nothing here needs wm.c to know
 * what a notification is.
 *
 *   1. #include "notify.h"
 *
 *   2. next to dirty_rect(), the one thing an overlay outside wm.c cannot do
 *      for itself -- report damage:
 *          void wm_damage(int x, int y, int w, int h) { dirty_rect(x, y, w, h); }
 *
 *   3. next to it, so kernel chrome outside wm.c can match the system theme:
 *          int wm_dark(void) { return g_ui_dark; }
 *
 *   4. in render_region(), AFTER the dock and BEFORE draw_cursor_back() -- the
 *      overlay sits above every window and below the pointer:
 *          notify_compose();
 *      (No rect_hit() guard is needed or wanted: the fb clip is already set to
 *      the damage rectangle, and notify_compose draws only with clip-exact
 *      primitives. See the note on glass in notify.c.)
 *
 *   5. in wm_run()'s loop, next to proc_reap():
 *          notify_tick();
 *
 *   6. in wm_process_mouse(), in the `left && !mleft` press path, immediately
 *      after the menu-bar dark-mode switch and before the dock, following that
 *      line's exact idiom -- a notification is chrome drawn on top of
 *      everything, so it must win the click too:
 *          if (notify_click(x, y)) {
 *              mleft = left; mright = right; mmiddle = middle;
 *              return;
 *          }
 *
 * The clipboard needs nothing from wm.c at all: it is reached only through
 * syscall.c. What the window-management line owns for it is the SHORTCUT
 * ROUTING -- Cmd+C / Cmd+V through their modifier and system-shortcut table --
 * which calls clip_set_text() / clip_get_text() from c/kernel/gui/clipboard.h.
 * There is deliberately no second shortcut mechanism here, and no key handling
 * in either of these two files.
 * --------------------------------------------------------------------------- */

/* Raise a notification. KERNEL POINTERS; both strings are copied. Returns the
 * notification's id (>= 1), or 0 if the ring is full. Never blocks, never takes
 * focus, never fails in a way a caller has to handle.
 *
 * Thread context only -- it runs under the big kernel lock, like everything
 * else in the GUI. Do not call it from an interrupt handler. */
int notify_post(const char *title, const char *body, int level);

/* The SYS_NOTIFY back end (user pointers, copied in and bounded). */
long notify_syscall(long num, long a, long b, long c);

/* --- the three WM hooks, in the order the loop reaches them --- */
void notify_tick(void);     /* expire, promote queued, report the damage */
void notify_compose(void);  /* draw the visible cards into the current target */
int  notify_click(int x, int y);   /* 1 if a card took the click and closed */

/* How many cards are showing right now (for tests and for the shell's `notify
 * --count`); the queued ones are not counted. */
int  notify_showing(void);

#endif /* LOGIT_NOTIFY_H */
