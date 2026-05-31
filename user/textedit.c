#include "aqua.h"

/* A tiny text editor. Launched by double/clicking a .txt in Finder (file
 * association) or from the Dock; reads the file via a syscall, lets you type. */

#define MAXT 4000
#define WINW 460
#define WINH 300

static char text[MAXT + 1];
static int  tlen;

static void redraw(void)
{
    gui_clear(rgb(252, 252, 253));
    int cols = (WINW - 20) / 8;
    char line[128];
    int ll = 0, cy = 8, col = 0;

    for (int i = 0; i < tlen; i++) {
        char c = text[i];
        if (c == '\n' || col >= cols) {
            line[ll] = 0;
            if (ll) gui_text(10, cy, rgb(40, 40, 48), line);
            ll = 0; col = 0; cy += 16;
            if (c == '\n') continue;
        }
        line[ll++] = c; col++;
    }
    line[ll] = 0;
    if (ll) gui_text(10, cy, rgb(40, 40, 48), line);

    gui_rect(10 + col * 8, cy, 8, 16, rgb(90, 150, 240));   /* caret */
    gui_flush();
}

void app_main(void)
{
    char fn[64];
    int n = get_arg(fn, sizeof fn);
    gui_create(n > 0 ? fn : "TextEdit", WINW, WINH);

    if (n > 0) {
        int r = read_file(fn, text, MAXT);
        if (r > 0) { tlen = r > MAXT ? MAXT : r; text[tlen] = 0; }
    }
    redraw();

    for (;;) {
        struct aqua_event e;
        int changed = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE)
                app_exit(0);
            if (e.type == EV_KEY) {
                char c = (char)e.a;
                if (c == '\b') { if (tlen > 0) text[--tlen] = 0; }
                else if (tlen < MAXT) { text[tlen++] = c; text[tlen] = 0; }
                changed = 1;
            }
        }
        if (changed)
            redraw();
        sys_yield();
    }
}
