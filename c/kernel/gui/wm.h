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

#endif /* LOGIT_WM_H */
