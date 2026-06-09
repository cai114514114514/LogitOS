#include "aether.h"

/* Preview: a minimal image viewer. The kernel decodes the file (PNG/GIF/JPEG) into
 * our RGBA buffer via SYS_IMG_DECODE; we aspect-fit it into the window with gui_blit
 * (which scales). Opened by the Finder for image files (see launch_for_ext). */

#define WINW 760
#define WINH 560
#define MAXW 1280            /* up to screen resolution */
#define MAXH 800
static unsigned char rgba[MAXW * MAXH * 4];

void app_main(void)
{
    gui_create("Preview", WINW, WINH);

    char path[128];
    int have = (get_arg(path, sizeof path) > 0 && path[0]);
    int iw = 0, ih = 0;
    int ok = have && img_open(path, rgba, (int)sizeof rgba, &iw, &ih) == 0 && iw > 0 && ih > 0;

    int redraw = 1;
    for (;;) {
        struct aether_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_KEY && e.a == 27) app_exit(0);   /* Esc closes */
        }
        if (redraw) {
            redraw = 0;
            gui_clear(rgb(28, 28, 32));
            if (ok) {
                /* aspect-fit into the content area, centered */
                int dw = WINW, dh = (int)((long)WINW * ih / iw);
                if (dh > WINH) { dh = WINH; dw = (int)((long)WINH * iw / ih); }
                gui_blit((WINW - dw) / 2, (WINH - dh) / 2, dw, dh, rgba, iw, ih);
            } else {
                gui_text(16, 16, rgb(220, 220, 224),
                         have ? "cannot open this image" : "no file given");
            }
            gui_flush();
        }
        sys_yield();
    }
}
