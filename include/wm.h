#ifndef AQUA_WM_H
#define AQUA_WM_H

/* Window manager / compositor + application platform. */
void wm_init(void);
void wm_render(void);
void wm_run(void);                 /* scheduler "main" thread; does not return */

/* Input (from the mouse / keyboard drivers). */
void wm_mouse_event(int x, int y, int left);
void wm_key(int c);

/* Launch a .aex application (optionally with a file argument). */
void wm_launch(const char *aex_file, const char *arg);

/* GUI system-call back end (called from syscall.c in the app's context). */
long wm_gui_syscall(long num, long a, long b, long c);
void wm_app_exit(void);

#endif /* AQUA_WM_H */
