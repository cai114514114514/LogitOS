#include "../../lib/agent/gui.h"
#include "model.h"
#include "../gui/clock/view.h"
#include <stdio.h>

/* Clock owns events and wall time. Rendering and layout live in ../gui/clock/;
 * OpenLogit owns rasterization, cached layers and transition sampling. */
void app_main(void)
{
    struct clock_state app = {.hour24 = 1, .seconds = 1};
    struct clock_ui ui = {0};
    gui_create("Clock", 620, 400);
    /* Window lifecycle applies the GUI's minimum so text and controls remain
     * usable after an ordinary resize all the way to the size limit. */
    _sys(SYS_GUI_WIN_MIN, ((long)CLOCK_MIN_WIDTH << 16) | CLOCK_MIN_HEIGHT, 0, 0);
    aui_set_size(620, 400);
    if (clock_dial_init()) {
        printf("CLOCK ERROR graphics initialization\n");
        app_exit(1);
    }
    printf("OPENLOGIT_CLOCK api=1.1 backend=software ready ui=2\n");
    int force = 1, was_active = 0, last_hour = -1, last_minute = -1, last_second = -1;
    for (;;) {
        struct logit_time time;
        get_time(&time);
        uint64_t now = monotonic_ns();
        int reduced = setting_int("ui.reduce_motion", 0) != 0;
        struct logit_event event;
        while (poll_event(&event)) {
            if (ag_gui_event(&event))
                continue;
            if (event.type == EV_CLOSE)
                app_exit(0);
            if (event.type == EV_RESIZE) {
                aui_set_size(event.a, event.b);
                clock_dial_invalidate();
                force = 1;
            }
            if (event.type == EV_THEME) {
                clock_dial_invalidate();
                force = 1;
            }
            if (event.type == EV_KEY && (event.a == 'h' || event.a == 'H')) {
                clock_apply(&app, CLOCK_TOGGLE_FORMAT);
                force = 1;
            }
            if (event.type == EV_KEY && (event.a == 's' || event.a == 'S')) {
                clock_apply(&app, CLOCK_TOGGLE_SECONDS);
                force = 1;
            }
            /* Feed and draw each event: collapsing a press and release into
             * one feed would silently drop quick clicks. */
            aui_feed(&event);
            if (ol_window_visible() && (force || aui_want_repaint())) {
                enum clock_action action = clock_view(&ui, &app, &time, now, reduced, 1);
                clock_apply(&app, action);
                force = action != CLOCK_NO_ACTION;
            }
            aui_feed_done();
        }
        if (!ol_window_visible()) {
            ui.motion.initialized = 0;
            was_active = 0;
            force = 1;
            wait_idle(0);
            continue;
        }
        int active = 0;
        clock_second(&ui.motion, time.second, now, reduced || !app.seconds, &active);
        /* Publish the endpoint once after active becomes false; otherwise the
         * needle stays on the penultimate sample until the next whole second. */
        if (force || aui_anim_due() || active || was_active || time.hour != last_hour ||
            time.minute != last_minute || (app.seconds && time.second != last_second)) {
            int refresh = force || aui_anim_due() || time.hour != last_hour ||
                          time.minute != last_minute || time.second != last_second;
            enum clock_action action = clock_view(&ui, &app, &time, now, reduced, refresh);
            clock_apply(&app, action);
            last_hour = time.hour;
            last_minute = time.minute;
            last_second = time.second;
            force = action != CLOCK_NO_ACTION;
        }
        was_active = active;
        int wait = aui_anim_wait();
        if (!wait || wait > 100)
            wait = 100;
        if (active && wait > 20)
            wait = 20;
        wait_idle(wait);
    }
}

const char *ag_gui_context(unsigned *bytes)
{
    struct logit_time time;
    get_time(&time);
    static char context[120];
    *bytes =
        (unsigned)snprintf(context, sizeof context, "Current clock: %04d-%02d-%02d %02d:%02d:%02d",
                           time.year, time.month, time.day, time.hour, time.minute, time.second);
    return context;
}
