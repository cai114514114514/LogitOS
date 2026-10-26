#include "openlogit_ui.h"
#include <stdio.h>

/* Host tests for the openlogit interaction core (c/lib/gfx/ui/interaction.c). */

static unsigned failures;
static void check(int ok, const char *name)
{
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static int event(struct ol_ui *ui, int type, int x, int y)
{
    struct ol_ui_event e = {type, x, y};
    return ol_ui_event(ui, &e);
}

static void hit_overlap(void)
{
    struct ol_ui ui;
    struct ol_ui_feedback feedback;
    ol_ui_init(&ui, 0);
    ol_ui_define(&ui, 0, (struct gfx_rect){0, 0, 100, 100});
    ol_ui_define(&ui, 1, (struct gfx_rect){50, 50, 100, 100});
    ol_ui_begin(&ui, 0, 0);
    event(&ui, OL_UI_DOWN, 60, 60);
    ol_ui_feedback(&ui, 1, 0, &feedback);
    check(feedback.focused, "hit-test overlap picks the highest id slot");
    ol_ui_feedback(&ui, 0, 0, &feedback);
    check(!feedback.focused, "overlapped lower slot is not hit");
    event(&ui, OL_UI_UP, 60, 60);
    event(&ui, OL_UI_DOWN, 10, 10);
    check(event(&ui, OL_UI_UP, 10, 10) == 0,
          "non-overlapped point hits only the lower slot");
}

static void tab_order(void)
{
    struct ol_ui ui;
    ol_ui_init(&ui, 0);
    for (unsigned id = 0; id < 3; id++)
        ol_ui_define(&ui, id, (struct gfx_rect){0, (int)id * 20, 40, 10});
    ol_ui_enable(&ui, 1, 0);
    ol_ui_begin(&ui, 0, 0);
    event(&ui, OL_UI_NEXT, 0, 0);
    check(ui.focused == 0, "tab from no focus enters the first enabled slot");
    event(&ui, OL_UI_NEXT, 0, 0);
    check(ui.focused == 2, "tab skips a disabled slot");
    event(&ui, OL_UI_NEXT, 0, 0);
    check(ui.focused == 0, "tab wraps to the first enabled slot");
    event(&ui, OL_UI_PREVIOUS, 0, 0);
    check(ui.focused == 2, "shift-tab wraps backwards past disabled slots");
}

static void capture_activation(void)
{
    struct ol_ui ui;
    ol_ui_init(&ui, 0);
    ol_ui_define(&ui, 0, (struct gfx_rect){0, 0, 100, 20});
    ol_ui_begin(&ui, 0, 0);
    event(&ui, OL_UI_DOWN, 10, 10);
    check(event(&ui, OL_UI_UP, 10, 10) == 0 && ol_ui_take_activation(&ui, 0) &&
              !ol_ui_take_activation(&ui, 0),
          "press and release inside activates exactly once");
    event(&ui, OL_UI_DOWN, 10, 10);
    event(&ui, OL_UI_UP, 500, 500);
    check(!ol_ui_take_activation(&ui, 0), "release outside the captured slot does not activate");
}

static void slider_drag(void)
{
    struct ol_ui ui;
    float value = 0;
    ol_ui_init(&ui, 0);
    ol_ui_define(&ui, 2, (struct gfx_rect){100, 0, 200, 20});
    ol_ui_begin(&ui, 0, 0);
    event(&ui, OL_UI_DOWN, 200, 10);
    event(&ui, OL_UI_MOVE, 300, 10);
    check(ol_ui_slider_value(&ui, 2, &value) && value == 1.f,
          "slider drag to the right end maps to 1");
    event(&ui, OL_UI_MOVE, 200, 10);
    check(ol_ui_slider_value(&ui, 2, &value) && value == .5f,
          "slider drag back to the middle maps to .5");
    event(&ui, OL_UI_UP, 200, 10);
    check(!ol_ui_slider_value(&ui, 2, &value), "slider ignores moves after release");
}

static void disabled_hover(void)
{
    struct ol_ui ui;
    struct ol_ui_feedback feedback;
    ol_ui_init(&ui, 0);
    ol_ui_define(&ui, 0, (struct gfx_rect){0, 0, 50, 50});
    ol_ui_define(&ui, 1, (struct gfx_rect){60, 0, 50, 50});
    ol_ui_enable(&ui, 0, 0);
    event(&ui, OL_UI_MOVE, 10, 10);
    ol_ui_begin(&ui, 0, 0);
    ol_ui_feedback(&ui, 0, 0, &feedback);
    ol_ui_begin(&ui, 300000000, 0);
    ol_ui_feedback(&ui, 0, 0, &feedback);
    check(feedback.hover == 0, "disabled-hover");
    ol_ui_begin(&ui, 0, 0);
    ol_ui_event(&ui, &(struct ol_ui_event){OL_UI_MOVE, 80, 10});
    ol_ui_feedback(&ui, 1, 0, &feedback);
    ol_ui_begin(&ui, 300000000, 0);
    ol_ui_feedback(&ui, 1, 0, &feedback);
    check(feedback.hover > .1f, "enabled slot hover rises under the same pointer path");
    event(&ui, OL_UI_DOWN, 10, 10);
    ol_ui_begin(&ui, 300000000, 0);
    ol_ui_feedback(&ui, 0, 0, &feedback);
    check(feedback.press == 0 && ui.captured == -1, "disabled slot is never pressed");
}

static void bad_arguments(void)
{
    struct ol_ui ui;
    struct ol_ui_event e = {OL_UI_MOVE, 0, 0};
    ol_ui_init(&ui, 0);
    check(ol_ui_event(NULL, &e) == OL_ARGUMENT, "null ui returns OL_ARGUMENT");
    check(ol_ui_event(&ui, NULL) == OL_ARGUMENT, "null event returns OL_ARGUMENT");
}

int main(void)
{
    hit_overlap();
    tab_order();
    capture_activation();
    slider_drag();
    disabled_hover();
    bad_arguments();
    printf("%u failure(s)\n", failures);
    return failures ? 1 : 0;
}
