#ifndef OPENLOGIT_UI_H
#define OPENLOGIT_UI_H

#include "openlogit_anim.h"

/* Small reusable interaction primitives, not a layout/font engine. The caller
 * owns model values, layout, event routing, text and the frame's command list.
 * Slots have stable numeric IDs and use one caller-supplied monotonic time. */
#define OL_UI_SLOTS 64
enum ol_ui_event_type {
    OL_UI_MOVE = 1, OL_UI_DOWN, OL_UI_UP, OL_UI_ACTIVATE, OL_UI_PREVIOUS, OL_UI_NEXT
};
struct ol_ui_event { int type, x, y; };
struct ol_ui_slot {
    struct gfx_rect bounds;
    struct ol_spring hover, press, value;
    int initialized, enabled;
};
struct ol_ui {
    struct ol_ui_slot slots[OL_UI_SLOTS];
    uint64_t now;
    int pointer_x, pointer_y, captured, focused, activated;
    int reduced, active, dragging, dirty;
};
struct ol_ui_feedback { float hover, press, value; int focused, active; };

void ol_ui_init(struct ol_ui *ui, uint64_t now);
void ol_ui_begin(struct ol_ui *ui, uint64_t now, int reduced);
int ol_ui_define(struct ol_ui *ui, unsigned id, struct gfx_rect bounds);
int ol_ui_enable(struct ol_ui *ui, unsigned id, int enabled);
/* Returns activated control ID, or -1. Pointer capture survives leaving the
 * rectangle; buttons activate only on release inside their captured control. */
int ol_ui_event(struct ol_ui *ui, const struct ol_ui_event *event);
int ol_ui_take_activation(struct ol_ui *ui, unsigned id);
int ol_ui_feedback(struct ol_ui *ui, unsigned id, float target, struct ol_ui_feedback *out);
int ol_ui_slider_value(struct ol_ui *ui, unsigned id, float *value);
int ol_ui_focus(struct ol_ui *ui, unsigned id);
int ol_ui_needs_frame(const struct ol_ui *ui);

/* Draw geometry only. Labels can be drawn after submission by the caller's
 * font adapter, then combined into the same window present. */
int ol_ui_panel(struct ol_list *list, struct gfx_rect bounds, unsigned rgb, int opacity);
int ol_ui_button(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback, int selected);
int ol_ui_toggle(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback);
int ol_ui_slider(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback);
int ol_ui_progress(struct ol_list *list, struct gfx_rect bounds, float progress);

#endif
