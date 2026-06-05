#include "aqua.h"

/* A tiny text editor. Launched by clicking a .txt in Finder (file association)
 * or from the Dock; reads the file via a syscall, lets you type, and saves
 * back to disk with Ctrl+S. */

#define MAXT 4000
#define WINW 460
#define WINH 300
#define CTRL_S 0x13

static char text[MAXT + 1];
static int  tlen;
static char fname[64];
static int  saved;          /* 1 just after a successful save, 0 once edited */

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

    /* status line */
    gui_rect(0, WINH - 20, WINW, 20, rgb(244, 244, 247));
    if (saved)
        gui_text(10, WINH - 18, rgb(40, 160, 80), "saved");
    else {
        gui_text(10, WINH - 18, rgb(150, 150, 158), "Ctrl+S to save:");
        gui_text(10 + 16 * 8, WINH - 18, rgb(110, 110, 120), fname);
    }
    gui_flush();
}

void app_main(void)
{
    int n = get_arg(fname, sizeof fname);
    if (n <= 0) {
        const char *d = "untitled.txt";
        int i = 0; while (d[i]) { fname[i] = d[i]; i++; } fname[i] = 0;
    }
    gui_create(fname, WINW, WINH);

    int r = read_file(fname, text, MAXT);
    if (r > 0) { tlen = r > MAXT ? MAXT : r; text[tlen] = 0; }
    saved = 1;
    redraw();

    for (;;) {
        struct aqua_event e;
        int changed = 0;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE)
                app_exit(0);
            if (e.type == EV_KEY) {
                char c = (char)e.a;
                if (c == CTRL_S) {
                    if (write_file(fname, text, tlen) >= 0)
                        saved = 1;
                    changed = 1;
                } else if (c == '\b') {
                    if (tlen > 0) text[--tlen] = 0;
                    saved = 0; changed = 1;
                } else if (tlen < MAXT) {
                    text[tlen++] = c; text[tlen] = 0;
                    saved = 0; changed = 1;
                }
            }
        }
        if (changed)
            redraw();
        sys_yield();
    }
}
