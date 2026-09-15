#include "view.h"
#include <stdio.h>
#include <string.h>

static void centered(int x, int y, int width, const char *text, unsigned color, int size)
{
    int measured = text_measure_px(text, (int)strlen(text), size, 0);
    aui_text_sz(x + (width - measured) / 2, y, text, color, size);
}

enum clock_action clock_view(struct clock_ui *ui, const struct clock_state *app,
                             const struct logit_time *time, uint64_t now, int reduced, int refresh)
{
    uint64_t start = monotonic_ns();
    int w = aui_width(), h = aui_height();
    enum clock_action action = CLOCK_NO_ACTION;
    struct clock_layout layout = clock_layout(w, h, aui_scale());
    if (refresh) {
        aui_begin(AUI_BG);
        aui_text_sz(24, 20, "Local time", AUI_TEXT, 23);
        if (w >= 520)
            aui_text_sz(w - 170, 27, "System clock", AUI_MUTED, 13);
        aui_round(16, 58, w - 32, h - 122, 18, AUI_SURFACE);
    }
    int active;
    int second = clock_second(&ui->motion, time->second, now, reduced || !app->seconds, &active);
    int status = clock_dial_draw(layout, time, second, app->seconds);
    if (status) {
        printf("CLOCK ERROR draw status=%d\n", status);
        aui_text_sz(24, 66, ol_status_string(status), AUI_ACCENT, 14);
    }
    if (!refresh) {
        /* In-between hand samples change no text/control pixels. The composed
         * dial is opaque, so copying it restores the previous needle too.
         * Skipping aui_begin avoids a full-window clear and repeated font work. */
        ol_window_present_region((struct gfx_rect){layout.face_x, layout.face_y,
                                                   layout.face_size, layout.face_size});
        goto logged;
    }
    int x = layout.text_x, y = layout.text_y, width = layout.text_w;
    int size = layout.compact ? 38 : 54;
    if (width < 190)
        size = 32;
    char hm[6], detail[48], date[48];
    clock_hm(hm, time->hour, time->minute, app->hour24);
    centered(x, y, width, hm, AUI_TEXT, size);
    if (app->seconds)
        snprintf(detail, sizeof detail, "%02d seconds  /  %s", time->second,
                 app->hour24       ? "24-hour"
                 : time->hour < 12 ? "AM"
                                   : "PM");
    else
        snprintf(detail, sizeof detail, "%s",
                 app->hour24       ? "24-hour time"
                 : time->hour < 12 ? "AM"
                                   : "PM");
    centered(x, y + size + 8, width, detail, AUI_MUTED, 13);
    snprintf(date, sizeof date, "%04d / %02d / %02d", time->year, time->month, time->day);
    centered(x, y + size + 34, width, date, AUI_MUTED, 15);
    int gap = 10, button_w = (w - 58) / 2;
    if (button_w > 170)
        button_w = 170;
    int left = (w - button_w * 2 - gap) / 2;
    if (aui_button_ex(left, layout.footer_y, button_w, 34, app->hour24 ? "24-hour" : "12-hour",
                      AUI_V_SECONDARY, 1)) {
        action = CLOCK_TOGGLE_FORMAT;
    }
    if (aui_button_ex(left + button_w + gap, layout.footer_y, button_w, 34,
                      app->seconds ? "Seconds on" : "Seconds off",
                      app->seconds ? AUI_V_PRIMARY : AUI_V_SECONDARY, 1)) {
        action = CLOCK_TOGGLE_SECONDS;
    }
    aui_end_rect(layout.face_x, layout.face_y, layout.face_size, layout.face_size);
logged:
    printf("CLOCK FRAME n=%u size=%dx%d scale=%d compact=%d seconds=%d hour24=%d hand=%d active=%d "
           "reduced=%d ns=%lu refresh=%d\n",
           ++ui->frames, w, h, aui_scale(), layout.compact, app->seconds, app->hour24, second,
           active, reduced, (unsigned long)(monotonic_ns() - start), refresh);
    return action;
}
