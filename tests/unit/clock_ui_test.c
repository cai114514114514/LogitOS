#include "model.h"
#include "layout.h"
#include "motion.h"
#include <stdio.h>
#include <string.h>
static int failed, checks;
static void check(int pass, const char *name)
{
    checks++;
    if (!pass)
        failed++;
    printf("%s %s\n", pass ? "PASS" : "FAIL", name);
}
int main(void)
{
    char value[6];
    clock_hm(value, 0, 7, 0);
    check(!strcmp(value, "12:07"), "midnight uses 12 AM");
    clock_hm(value, 12, 9, 0);
    check(!strcmp(value, "12:09"), "noon uses 12 PM");
    clock_hm(value, 23, 59, 0);
    check(!strcmp(value, "11:59"), "12-hour evening");
    clock_hm(value, 23, 59, 1);
    check(!strcmp(value, "23:59"), "24-hour evening");
    int valid = 1;
    for (int s = 100; s <= 300; s += 50)
        for (int w = CLOCK_MIN_WIDTH; w <= 1000; w += 40)
            for (int h = CLOCK_MIN_HEIGHT; h <= 720; h += 40) {
                struct clock_layout l = clock_layout(w, h, s);
                valid &= l.face_size * s / 100 <= 384 && l.face_x >= 0 && l.text_x >= 0 &&
                         l.text_w > 0 && l.footer_y >= 0 &&
                         l.text_y + (l.compact ? 87 : 103) < l.footer_y;
            }
    check(valid, "compact wide and HiDPI layouts keep readout above controls");
    struct clock_motion motion = {0};
    int active;
    check(clock_second(&motion, 59, 0, 0, &active) == 59 * 1024 && !active,
          "initial hand snaps to wall time");
    check(clock_second(&motion, 0, 1000000000, 0, &active) == -1024 && active,
          "59 to zero starts one tick behind");
    int mid = clock_second(&motion, 0, 1090000000, 0, &active);
    check(mid > -1024 && mid < 0 && active, "shared easing has real intermediate hand position");
    check(clock_second(&motion, 0, 1200000000, 0, &active) == 0 && !active,
          "hand stops at exact final tick");
    check(clock_second(&motion, 24, 2000000000, 0, &active) == 24 * 1024 && !active,
          "time correction snaps without replay");
    check(clock_second(&motion, 25, 3000000000, 1, &active) == 25 * 1024 && !active,
          "reduced motion snaps without animation");
    printf("Clock UI: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
