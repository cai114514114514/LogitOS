#include "app.h"
#include "../example_log.h"
#include <stdio.h>
#include <string.h>

static const struct gfx_rect bounds[CONTROL_COUNT] = {
    [PAGE_VECTOR] = {24, 62, 180, 32},  [PAGE_SPRITES] = {216, 62, 180, 32},
    [PAGE_LAYERS] = {408, 62, 180, 32}, [PLAY] = {648, 160, 204, 34},
    [CLIP] = {648, 210, 204, 34},       [DASH] = {648, 258, 204, 34},
    [VARIANT] = {648, 306, 204, 34},    [RESET] = {648, 400, 204, 34},
    [AMOUNT] = {648, 365, 204, 20}};

void controls_init(struct vector_app *app, uint64_t now)
{
    ol_ui_init(&app->ui, now);
    for (unsigned i = 0; i < CONTROL_COUNT; i++)
        ol_ui_define(&app->ui, i, bounds[i]);
    static const struct ol_keyframe keys[] = {{.offset = 0, .value = {0}},
                                              {.offset = 1, .value = {1}}};
    struct ol_animation_desc desc = {sizeof desc, OL_SCALAR,     2, OL_FORWARD,
                                     0,           5000000000ULL, 0, keys};
    ol_animation_init(&app->timeline, &desc, now);
    ol_animation_pause(&app->timeline, now);
    app->amount = .65f;
    app->clipped = app->dashed = 1;
    app->card_x = 184;
    app->card_y = 260;
    app->toolbar_dirty = app->inspector_dirty = app->art_dirty = 1;
}

void controls_action(struct vector_app *app, unsigned id, uint64_t now)
{
    if (id <= PAGE_LAYERS) {
        app->page = id;
        app->toolbar_dirty = app->art_dirty = 1;
    } else
        switch (id) {
        case PLAY:
            if (app->playing) {
                app->playing = 0;
                ol_animation_pause(&app->timeline, now);
            } else if (!app->reduced) {
                app->playing = 1;
                ol_animation_play(&app->timeline, now);
            }
            break;
        case CLIP:
            app->clipped = !app->clipped;
            app->art_dirty = 1;
            break;
        case DASH:
            app->dashed = !app->dashed;
            app->art_dirty = 1;
            break;
        case VARIANT:
            app->variant = (app->variant + 1) % 3;
            app->art_dirty = 1;
            break;
        case RESET:
            app->playing = 0;
            ol_animation_pause(&app->timeline, now);
            ol_animation_seek(&app->timeline, now, 0);
            app->amount = .65f;
            app->clipped = app->dashed = 1;
            app->variant = 0;
            app->card_x = 184;
            app->card_y = 260;
            app->art_dirty = 1;
            break;
        default:
            break;
        }
    app->inspector_dirty = 1;
    example_log("VECTOR ACTION id=%u page=%d playing=%d clip=%d dash=%d variant=%d\n", id,
                app->page, app->playing, app->clipped, app->dashed, app->variant);
}

void controls_event(struct vector_app *app, const struct logit_event *event, uint64_t now)
{
    if (event->type == EV_KEY) {
        int key = event->a;
        if (key >= '1' && key <= '3')
            controls_action(app, key - '1', now);
        if (key == ' ')
            controls_action(app, PLAY, now);
        if (key == 'c')
            controls_action(app, CLIP, now);
        if (key == 'd')
            controls_action(app, DASH, now);
        if (key == 's')
            controls_action(app, VARIANT, now);
        if (key == 'r')
            controls_action(app, RESET, now);
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            if (app->ui.focused == AMOUNT) {
                app->amount += (key == KEY_RIGHT ? .05f : -.05f);
                if (app->amount < 0)
                    app->amount = 0;
                if (app->amount > 1)
                    app->amount = 1;
                app->inspector_dirty = app->art_dirty = 1;
            } else
                app->card_x += key == KEY_RIGHT ? 24 : -24;
        }
        if (key == 9)
            ol_ui_event(&app->ui, &(struct ol_ui_event){OL_UI_NEXT, 0, 0});
        if (key == 13 || key == 10)
            ol_ui_event(&app->ui, &(struct ol_ui_event){OL_UI_ACTIVATE, 0, 0});
    } else if (event->type == EV_MOUSE || event->type == EV_MOUSE_UP ||
               event->type == EV_MOUSE_MOVE) {
        int x = event->a, y = event->b;
        if (event->type == EV_MOUSE && app->page == 2 && x >= app->card_x &&
            x < app->card_x + 156 && y >= app->card_y && y < app->card_y + 108) {
            app->dragging = 1;
            app->drag_x = x - app->card_x;
            app->drag_y = y - app->card_y;
            if (app->playing)
                controls_action(app, PLAY, now);
        }
        if (event->type == EV_MOUSE_MOVE && app->dragging) {
            app->card_x = x - app->drag_x;
            app->card_y = y - app->drag_y;
        }
        int type = event->type == EV_MOUSE      ? OL_UI_DOWN
                   : event->type == EV_MOUSE_UP ? OL_UI_UP
                                                : OL_UI_MOVE;
        ol_ui_event(&app->ui, &(struct ol_ui_event){type, x, y});
        if (ol_ui_slider_value(&app->ui, AMOUNT, &app->amount))
            app->inspector_dirty = app->art_dirty = 1;
        if (event->type == EV_MOUSE_UP)
            app->dragging = 0;
    }
    for (unsigned i = 0; i < CONTROL_COUNT; i++)
        if (ol_ui_take_activation(&app->ui, i))
            controls_action(app, i, now);
    if (app->card_x < 24)
        app->card_x = 24;
    if (app->card_x > 444)
        app->card_x = 444;
    if (app->card_y < 112)
        app->card_y = 112;
    if (app->card_y > 348)
        app->card_y = 348;
}

int controls_update(struct vector_app *app, uint64_t now)
{
    if (app->reduced && app->playing)
        controls_action(app, PLAY, now);
    struct ol_anim_sample sample;
    ol_animation_sample(&app->timeline, now, 0, &sample);
    if (app->phase != sample.value[0] && app->page < 2)
        app->art_dirty = 1;
    app->phase = sample.value[0];
    if (app->playing && app->page == 2)
        app->card_x = 184 + (int)(gfx_sin((int)(app->phase * 360 * 256)) * 140 / 65536LL);
    for (unsigned i = 0; i < CONTROL_COUNT; i++) {
        struct ol_ui_feedback feedback;
        ol_ui_feedback(&app->ui, i, i == AMOUNT ? app->amount : 0, &feedback);
        struct ol_ui_feedback *old = &app->feedback[i];
        if (old->hover != feedback.hover || old->press != feedback.press ||
            old->value != feedback.value || old->focused != feedback.focused) {
            if (i < 3)
                app->toolbar_dirty = 1;
            else
                app->inspector_dirty = 1;
        }
        *old = feedback;
    }
    return app->playing || app->ui.active;
}

static int panel(struct vector_app *app, struct bitmap *bitmap, unsigned first, unsigned last,
                 int ox, int oy)
{
    ol_list_reset(app->list);
    ol_cmd_clear(app->list, first ? 0x1c3042 : 0x101b2a, 255);
    for (unsigned i = first; i < last; i++) {
        struct gfx_rect rect = bounds[i];
        rect.x -= ox;
        rect.y -= oy;
        if (i == AMOUNT)
            ol_ui_slider(app->list, rect, &app->feedback[i]);
        else
            ol_ui_button(app->list, rect, &app->feedback[i], i < 3 && i == (unsigned)app->page);
    }
    return ol_list_close(app->list) == OL_OK &&
           ol_submit(app->device, app->list, bitmap->surface, NULL) == OL_OK;
}

int controls_draw(struct vector_app *app)
{
    if (app->toolbar_dirty && !panel(app, &app->toolbar, 0, 3, 0, 0))
        return 0;
    if (app->inspector_dirty && !panel(app, &app->inspector, 3, CONTROL_COUNT, 624, 112))
        return 0;
    app->toolbar_dirty = app->inspector_dirty = 0;
    return 1;
}

static void label(int x, int y, int size, unsigned rgb, const char *text)
{
    ol_window_text_run(x, y, size, 0, rgb, text, (int)strlen(text));
}

void controls_labels(struct vector_app *app, struct gfx_rect damage)
{
    /* Labels live in the native font adapter. Clip them to the region just
     * restored from SDK pixels, otherwise repeated partial paints accumulate
     * antialiasing over unchanged text outside the damaged region. */
    ol_window_clip_region(damage);
    label(24, 20, 25, 0xf1f7fa, "OpenLogit / Vector Studio");
    label(44, 71, 14, 0xd8eee8, "Vector paths");
    label(238, 71, 14, 0xd8eee8, "Sprite atlas");
    label(430, 71, 14, 0xd8eee8, "Retained layers");
    label(648, 128, 17, 0xe8f2f7, "Properties");
    label(714, 170, 13, 0xe8f2f7, app->playing ? "Pause" : "Play");
    label(674, 220, 13, 0xe8f2f7, app->clipped ? "Circle clip enabled" : "Circle clip disabled");
    label(680, 268, 13, 0xe8f2f7, app->dashed ? "Dashed stroke" : "Solid stroke");
    label(684, 316, 13, 0xe8f2f7, "Cycle atlas sprites");
    label(725, 410, 13, 0xe8f2f7, "Reset");
    const char *names[] = {"Stroke width", "Nine-slice width", "Layer opacity"};
    char text[80];
    snprintf(text, sizeof text, "%s  %d%%", names[app->page], (int)(app->amount * 100));
    label(648, 345, 12, 0x9ab6c6, text);
    label(24, 482, 14, 0xb5d1de,
          "One 2D SDK: paths, gradients, crop sampling, cached layers and shared motion.");
    label(24, 516, 12, 0x89a8bb,
          "1 / 2 / 3 pages    Space play    C clip    D dash    S sprites    R reset    Drag the "
          "retained card");
    ol_window_reset_clip();
}
