#include "aqua.h"

/* A live digital clock -- a real ring-3 process redrawing on each tick. */

static void two(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; }

void app_main(void)
{
    gui_create("Clock", 220, 96);

    int last = -1;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e))
            if (e.type == EV_CLOSE)
                app_exit(0);

        struct aqua_time t;
        get_time(&t);
        if (t.second != last) {
            last = t.second;

            gui_clear(rgb(26, 28, 38));
            char hms[9];
            two(hms + 0, t.hour);   hms[2] = ':';
            two(hms + 3, t.minute); hms[5] = ':';
            two(hms + 6, t.second); hms[8] = 0;
            /* draw the time a few times offset to fake a bolder, bigger look */
            for (int dx = 0; dx < 2; dx++)
                for (int dy = 0; dy < 2; dy++)
                    gui_text(54 + dx, 30 + dy, rgb(120, 220, 255), hms);

            char date[11];
            date[0] = '0' + (t.year / 1000) % 10; date[1] = '0' + (t.year / 100) % 10;
            date[2] = '0' + (t.year / 10) % 10;   date[3] = '0' + t.year % 10;
            date[4] = '-'; two(date + 5, t.month); date[7] = '-'; two(date + 8, t.day);
            date[10] = 0;
            gui_text(66, 60, rgb(150, 160, 180), date);

            gui_flush();
        }
        sys_yield();
    }
}
