#ifndef AQUA_WM_H
#define AQUA_WM_H

/* Window manager / compositor. */
void wm_init(void);

/* Fed by the mouse driver (absolute cursor position + left button state). */
void wm_mouse_event(int x, int y, int left);

/* Composite one frame (background + windows + cursor) and present it. */
void wm_render(void);

/* Interactive event loop. Does not return. (Entered after the userland demo
 * exits, so the desktop becomes live.) */
void wm_run(void);

#endif /* AQUA_WM_H */
