#include "aui.h"

/* Activity Monitor: real uptime, memory, context switches, and the live process
 * list from the kernel (SYS_SYSINFO), rendered with the aui theme. */

#define WINW 380
#define WINH 264

void app_main(void)
{
    gui_create("Activity Monitor", WINW, WINH);

    int last = -1;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e))
            if (e.type == EV_CLOSE) app_exit(0);

        struct aqua_time t;
        get_time(&t);
        if (t.second != last) {                 /* refresh ~1 Hz */
            last = t.second;

            char info[1024];
            sysinfo(info, sizeof info);

            aui_begin(AUI_BG);
            aui_label(16, 14, "Activity Monitor", AUI_TEXT);
            char line[80];
            int ll = 0, y = 46;
            for (int i = 0;; i++) {
                char c = info[i];
                if (c == '\n' || c == 0) {
                    line[ll] = 0;
                    if (ll) aui_label(16, y, line, AUI_MUTED);
                    y += 18; ll = 0;
                    if (c == 0) break;
                    continue;
                }
                if (ll < 79) line[ll++] = c;
            }
            aui_end();
        }
        sys_yield();
    }
}
