#include "aui.h"

/* A live digital clock, rendered with the aui theme. */

static void two(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; }

void app_main(void)
{
    gui_create("Clock", 240, 132);

    int last = -1;
    for (;;) {
        struct aether_event e;
        while (poll_event(&e))
            if (e.type == EV_CLOSE) app_exit(0);

        struct aether_time t;
        get_time(&t);
        if (t.second != last) {
            last = t.second;
            aui_begin(AUI_BG);

            char hms[9];
            two(hms + 0, t.hour);   hms[2] = ':';
            two(hms + 3, t.minute); hms[5] = ':';
            two(hms + 6, t.second); hms[8] = 0;
            int tw = text_measure_px(hms, 8, 34, 0);
            gui_text_run((240 - tw) / 2, 34, 34, 0, AUI_TEXT, hms, 8);   /* big time */

            char d[11];
            d[0] = '0' + (t.year / 1000) % 10; d[1] = '0' + (t.year / 100) % 10;
            d[2] = '0' + (t.year / 10) % 10;   d[3] = '0' + t.year % 10;
            d[4] = '-'; two(d + 5, t.month); d[7] = '-'; two(d + 8, t.day); d[10] = 0;
            int dw = text_measure_px(d, 10, 15, 0);
            aui_label((240 - dw) / 2, 92, d, AUI_MUTED);

            aui_end();
        }
        sys_yield();
    }
}
