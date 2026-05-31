#include "aqua.h"

/* Activity Monitor: shows real uptime, memory, context switches, and the live
 * process list -- all from the kernel via the SYS_SYSINFO syscall. */

#define WINW 360
#define WINH 250

void app_main(void)
{
    gui_create("Activity Monitor", WINW, WINH);

    int last = -1;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e))
            if (e.type == EV_CLOSE)
                app_exit(0);

        struct aqua_time t;
        get_time(&t);
        if (t.second != last) {           /* refresh ~1 Hz */
            last = t.second;

            char info[1024];
            sysinfo(info, sizeof info);

            gui_clear(rgb(250, 250, 252));
            char line[80];
            int ll = 0, y = 12;
            for (int i = 0;; i++) {
                char c = info[i];
                if (c == '\n' || c == 0) {
                    line[ll] = 0;
                    gui_text(14, y, rgb(50, 50, 58), line);
                    y += 16; ll = 0;
                    if (c == 0) break;
                    continue;
                }
                if (ll < 79) line[ll++] = c;
            }
            gui_flush();
        }
        sys_yield();
    }
}
