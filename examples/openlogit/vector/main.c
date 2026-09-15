#include "app.h"
#include "../example_log.h"
#include <stdlib.h>

static int initialize(struct vector_app *app)
{
    app->device_storage = malloc(ol_device_size());
    app->list_storage = malloc(1024 * 1024);
    app->transfer = malloc(900 * 560 * 4);
    if (!app->device_storage || !app->list_storage || !app->transfer ||
        ol_device_create(app->device_storage, ol_device_size(), OL_API_VERSION, OL_CAP_LAYER,
                         &app->device) ||
        ol_list_create(app->device, app->list_storage, 1024 * 1024, &app->list))
        return 0;
    if (!bitmap_create(app, &app->frame, 900, 560) ||
        !bitmap_create(app, &app->background, 900, 560) ||
        !bitmap_create(app, &app->toolbar, 900, 102) ||
        !bitmap_create(app, &app->inspector, 252, 344) ||
        !bitmap_create(app, &app->stage, 576, 344) || !bitmap_create(app, &app->card, 156, 108))
        return 0;
    controls_init(app, monotonic_ns());
    ol_scene2d_init(&app->scene, app->device, 900, 560);
    return artwork_init(app);
}

static int frame(struct vector_app *app)
{
    ol_list_reset(app->list);
    if (!controls_draw(app))
        return 0;
    if (app->art_dirty && !artwork_stage(app))
        return 0;
    app->art_dirty = 0;
    ol_scene2d_set(&app->scene, 0, app->toolbar.surface, (struct gfx_rect){0, 0, 900, 102}, 255, 1);
    ol_scene2d_set(&app->scene, 1, app->stage.surface, (struct gfx_rect){24, 112, 576, 344}, 255,
                   1);
    ol_scene2d_set(&app->scene, 2, app->inspector.surface, (struct gfx_rect){624, 112, 252, 344},
                   255, 1);
    ol_scene2d_set(&app->scene, 3, app->card.surface,
                   (struct gfx_rect){app->card_x, app->card_y, 156, 108}, (int)(app->amount * 255),
                   app->page == 2);
    struct ol_submit_info info = {.size = sizeof info};
    uint64_t start = monotonic_ns();
    if (ol_scene2d_render(&app->scene, app->list, app->frame.surface, app->background.surface,
                          &info))
        return 0;
    if (!info.commands)
        return 1;
    if (ol_window_composite_region(app->frame.surface, info.damage, app->transfer, 900 * 560 * 4))
        return 0;
    controls_labels(app, info.damage);
    if (ol_window_present_region(info.damage))
        return 0;
    example_log(
        "VECTOR FRAME n=%u page=%d playing=%d x=%d y=%d damage=%d,%d,%d,%d copied=%lu ns=%lu\n",
        ++app->frames, app->page, app->playing, app->card_x, app->card_y, info.damage.x,
        info.damage.y, info.damage.w, info.damage.h, (unsigned long)info.copied_bytes,
        (unsigned long)(monotonic_ns() - start));
    return 1;
}

int main(void)
{
    struct vector_app *app = calloc(1, sizeof *app);
    int result = 1;
    if (!app || !initialize(app))
        goto done;
    gui_create("OpenLogit Vector Studio", 900, 560);
    _sys(SYS_GUI_WIN_MIN, (900L << 16) | 560, 0, 0);
    app->reduced = setting_int("ui.reduce_motion", 0) != 0;
    example_log("VECTOR READY canvas=1 atlas=1 retained=1\n");
    result = 0;
    for (;;) {
        uint64_t now = monotonic_ns();
        ol_ui_begin(&app->ui, now, app->reduced);
        struct logit_event event;
        while (poll_event(&event)) {
            if (event.type == EV_CLOSE || (event.type == EV_KEY && event.a == 27))
                goto done;
            if (event.type == EV_THEME || event.type == EV_WINDOW_FOCUS ||
                event.type == EV_RESIZE) {
                app->reduced = setting_int("ui.reduce_motion", 0) != 0;
                ol_scene2d_invalidate(&app->scene);
                example_log("VECTOR MOTION reduced=%d\n", app->reduced);
            }
            controls_event(app, &event, now);
        }
        if (!ol_window_visible()) {
            wait_idle(0);
            continue;
        }
        int active = controls_update(app, now);
        if (!frame(app)) {
            example_log("VECTOR ERROR frame\n");
            result = 1;
            goto done;
        }
        wait_idle(active ? 16 : 0);
    }
done:
    if (app) {
        if (app->list)
            ol_list_reset(app->list);
        artwork_destroy(app);
        bitmap_destroy(&app->card);
        bitmap_destroy(&app->stage);
        bitmap_destroy(&app->inspector);
        bitmap_destroy(&app->toolbar);
        bitmap_destroy(&app->background);
        bitmap_destroy(&app->frame);
        if (app->list)
            ol_list_destroy(app->list);
        if (app->device)
            ol_device_destroy(app->device);
        free(app->transfer);
        free(app->list_storage);
        free(app->device_storage);
        free(app);
    }
    return result;
}
