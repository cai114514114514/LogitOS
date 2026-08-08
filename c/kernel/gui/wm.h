#ifndef LOGIT_WM_H
#define LOGIT_WM_H

/* Window manager / compositor + application platform. */
void wm_init(void);
void wm_render(void);
void wm_run(void);                 /* scheduler "main" thread; does not return */

/* Input (from the mouse / keyboard drivers). Both run in IRQ context and only
 * enqueue; the WM thread does the real work (see wm_drain_input).
 *
 * Buttons are LEVELS (held / not held), not edges: the driver reports what the
 * hardware says in each packet and the WM derives press/release from the
 * previous level, because deriving them is only correct where the window
 * routing lives. `wheel` is notches in this packet, + = scrolled toward the
 * user (down), 0 for a mouse with no wheel. */
void wm_mouse_event(int x, int y, int left, int right, int middle, int wheel);
void wm_key(int c);

/* Launch a .aex application (optionally with a file argument). */
void wm_launch(const char *aex_file, const char *arg);

/* GUI system-call back end (called from syscall.c in the app's context). */
long wm_gui_syscall(long num, long a, long b, long c);
void wm_app_exit(void);

/* ---- what kernel chrome OUTSIDE wm.c needs from the compositor -------------
 *
 * The notification overlay (c/kernel/gui/notify.c) draws on top of every window
 * but is not a window: it has no surface, no input queue and no place in the
 * z-order. These two are the entire interface it needs, and they are the whole
 * reason a second file can composite into this desktop without reaching into
 * this one's statics. See the WM-HOOKS block in notify.h for the five call
 * sites on the other side.
 *
 * wm_damage() is the important one. Damage rectangles are DEVICE PIXELS, and
 * the rule is the same one every caller inside wm.c now lives under: report the
 * TRUE extent of what changed. Under-reporting leaves pixels on screen that
 * nothing will ever repaint -- there is deliberately no periodic full repaint
 * left to cover for it. Over-reporting is only slow. */
void wm_damage(int x, int y, int w, int h);
int  wm_dark(void);                /* 1 if the system theme is dark */

#endif /* LOGIT_WM_H */
