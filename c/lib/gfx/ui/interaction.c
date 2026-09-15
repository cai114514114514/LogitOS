#include "openlogit_ui.h"

static int contains(struct gfx_rect bounds, int x, int y)
{
    return x >= bounds.x && y >= bounds.y &&
           (long long)x - bounds.x < bounds.w && (long long)y - bounds.y < bounds.h;
}

void ol_ui_init(struct ol_ui *ui, uint64_t now)
{
    if (!ui)
        return;
    /* This core is also linked into freestanding apps without libc. A large
     * aggregate assignment can introduce an unresolved compiler memset call. */
    volatile unsigned char *bytes = (volatile unsigned char *)ui;
    for (unsigned long index = 0; index < sizeof *ui; index++)
        bytes[index] = 0;
    ui->now = now;
    ui->captured = ui->focused = ui->activated = -1;
    ui->pointer_x = ui->pointer_y = -1;
}

void ol_ui_begin(struct ol_ui *ui, uint64_t now, int reduced)
{
    if (!ui)
        return;
    ui->now = now;
    ui->reduced = reduced != 0;
    ui->active = 0;
    ui->dirty = 0;
}

int ol_ui_define(struct ol_ui *ui, unsigned id, struct gfx_rect bounds)
{
    if (!ui || id >= OL_UI_SLOTS || bounds.w < 1 || bounds.h < 1)
        return OL_ARGUMENT;
    struct ol_ui_slot *slot = &ui->slots[id];
    slot->bounds = bounds;
    if (!slot->initialized) {
        ol_spring_init(&slot->hover, 0, 0, 0, 5, 1, ui->now, 260000000);
        ol_spring_init(&slot->press, 0, 0, 0, 7, 1, ui->now, 180000000);
        ol_spring_init(&slot->value, 0, 0, 0, 5, 1, ui->now, 320000000);
        slot->initialized = 1;
        slot->enabled = 1;
    }
    return OL_OK;
}

int ol_ui_enable(struct ol_ui *ui, unsigned id, int enabled)
{
    if (!ui || id >= OL_UI_SLOTS || !ui->slots[id].initialized)
        return OL_ARGUMENT;
    if (ui->slots[id].enabled == (enabled != 0))
        return OL_OK;
    ui->slots[id].enabled = enabled != 0;
    if (!enabled) {
        if (ui->focused == (int)id)
            ui->focused = -1;
        if (ui->captured == (int)id) {
            ui->captured = -1;
            ui->dragging = 0;
        }
    }
    ui->dirty = 1;
    return OL_OK;
}

int ol_ui_focus(struct ol_ui *ui, unsigned id)
{
    if (!ui || id >= OL_UI_SLOTS || !ui->slots[id].initialized || !ui->slots[id].enabled)
        return OL_ARGUMENT;
    ui->focused = (int)id;
    ui->dirty = 1;
    return OL_OK;
}

int ol_ui_event(struct ol_ui *ui, const struct ol_ui_event *event)
{
    if (!ui || !event)
        return -1;
    ui->dirty = 1;
    if (event->type == OL_UI_PREVIOUS || event->type == OL_UI_NEXT) {
        int direction = event->type == OL_UI_NEXT ? 1 : -1;
        int next = ui->focused;
        for (unsigned attempt = 0; attempt < OL_UI_SLOTS; attempt++) {
            next = (next + direction + OL_UI_SLOTS) % OL_UI_SLOTS;
            if (ui->slots[next].initialized && ui->slots[next].enabled) {
                ui->focused = next;
                break;
            }
        }
        return -1;
    }
    if (event->type == OL_UI_ACTIVATE) {
        ui->activated = ui->focused;
        return ui->activated;
    }
    ui->pointer_x = event->x;
    ui->pointer_y = event->y;
    int hit = -1;
    for (unsigned id = 0; id < OL_UI_SLOTS; id++) {
        if (ui->slots[id].initialized && ui->slots[id].enabled &&
            contains(ui->slots[id].bounds, event->x, event->y))
            hit = (int)id;
    }
    if (event->type == OL_UI_DOWN) {
        ui->captured = hit;
        ui->dragging = hit >= 0;
        if (hit >= 0)
            ui->focused = hit;
    } else if (event->type == OL_UI_UP) {
        if (ui->captured >= 0 && ui->captured == hit)
            ui->activated = hit;
        ui->captured = -1;
        ui->dragging = 0;
        return ui->activated;
    }
    return -1;
}

int ol_ui_take_activation(struct ol_ui *ui, unsigned id)
{
    if (!ui || ui->activated != (int)id)
        return 0;
    ui->activated = -1;
    return 1;
}

int ol_ui_slider_value(struct ol_ui *ui, unsigned id, float *value)
{
    if (!ui || !value || id >= OL_UI_SLOTS || !ui->slots[id].initialized ||
        !ui->dragging || ui->captured != (int)id)
        return 0;
    struct gfx_rect bounds = ui->slots[id].bounds;
    float position = (float)(ui->pointer_x - bounds.x) / bounds.w;
    if (position < 0)
        position = 0;
    if (position > 1)
        position = 1;
    int changed = *value != position;
    *value = position;
    return changed;
}

static float sample(struct ol_ui *ui, struct ol_spring *spring, float target)
{
    if (spring->target != target)
        ol_spring_retarget(spring, ui->now, target);
    if (ui->reduced)
        return target;
    int active = 0;
    float value = ol_spring_sample(spring, ui->now, 0, &active);
    ui->active |= active;
    return value;
}

int ol_ui_feedback(struct ol_ui *ui, unsigned id, float target, struct ol_ui_feedback *out)
{
    if (!ui || !out || id >= OL_UI_SLOTS || !ui->slots[id].initialized ||
        !(target >= 0 && target <= 1))
        return OL_ARGUMENT;
    struct ol_ui_slot *slot = &ui->slots[id];
    int hover = contains(slot->bounds, ui->pointer_x, ui->pointer_y);
    out->hover = sample(ui, &slot->hover, hover);
    out->press = sample(ui, &slot->press, ui->captured == (int)id && hover);
    out->value = sample(ui, &slot->value, target);
    out->focused = ui->focused == (int)id;
    out->active = ui->active;
    return OL_OK;
}

int ol_ui_needs_frame(const struct ol_ui *ui)
{
    return ui && (ui->active || ui->dirty);
}
