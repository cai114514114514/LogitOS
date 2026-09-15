#include "openlogit_ui.h"

static unsigned mix_rgb(unsigned from, unsigned to, float amount)
{
    unsigned color = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        float a = (from >> shift) & 255;
        float b = (to >> shift) & 255;
        color |= (unsigned)(a + (b - a) * amount + .5f) << shift;
    }
    return color;
}

static int valid_feedback(const struct ol_ui_feedback *feedback)
{
    return feedback && feedback->hover >= 0 && feedback->hover <= 1 &&
           feedback->press >= 0 && feedback->press <= 1 &&
           feedback->value >= 0 && feedback->value <= 1;
}

static int rounded(struct ol_list *list, struct gfx_rect bounds, int radius,
                    unsigned rgb, int opacity)
{
    if (bounds.w < 1 || bounds.h < 1)
        return OL_OK;
    int points[1024], contours[8];
    struct gfx_path path;
    struct gfx_paint paint;
    gfx_path_init(&path, points, 512, contours, 8);
    gfx_path_rrect(&path, GFX_PX(bounds.x), GFX_PX(bounds.y), GFX_PX(bounds.w),
                   GFX_PX(bounds.h), GFX_PX(radius));
    gfx_paint_solid(&paint, rgb, opacity);
    return ol_cmd_fill(list, &path, GFX_NONZERO, &paint, 0, 4);
}

int ol_ui_panel(struct ol_list *list, struct gfx_rect bounds, unsigned rgb, int opacity)
{
    return rounded(list, bounds, 10, rgb, opacity);
}

int ol_ui_button(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback, int selected)
{
    if (!valid_feedback(feedback))
        return OL_ARGUMENT;
    if (feedback->focused) {
        struct gfx_rect ring = {bounds.x - 2, bounds.y - 2, bounds.w + 4, bounds.h + 4};
        int status = rounded(list, ring, 9, 0x70ddc8, 255);
        if (status != OL_OK)
            return status;
    }
    int inset = (int)(feedback->press * 2 + .5f);
    bounds.x += inset;
    bounds.y += inset;
    bounds.w -= inset * 2;
    bounds.h -= inset * 2;
    unsigned rgb = selected ? mix_rgb(0x246b64, 0x328479, feedback->hover) :
                              mix_rgb(0x253b50, 0x365269, feedback->hover);
    return rounded(list, bounds, 7, rgb, 255);
}

int ol_ui_toggle(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback)
{
    if (!valid_feedback(feedback))
        return OL_ARGUMENT;
    int status = rounded(list, bounds, bounds.h / 2,
                          mix_rgb(0x304357, 0x329b86, feedback->value), 255);
    if (status != OL_OK)
        return status;
    int diameter = bounds.h - 6;
    struct gfx_rect knob = {bounds.x + 3 + (int)((bounds.w - bounds.h) * feedback->value),
                            bounds.y + 3, diameter, diameter};
    return rounded(list, knob, diameter / 2, 0xe4f5ee, 255);
}

int ol_ui_slider(struct ol_list *list, struct gfx_rect bounds,
                 const struct ol_ui_feedback *feedback)
{
    if (!valid_feedback(feedback))
        return OL_ARGUMENT;
    struct gfx_rect track = {bounds.x, bounds.y + bounds.h / 2 - 3, bounds.w, 6};
    int status = rounded(list, track, 3, 0x334c62, 255);
    if (status != OL_OK)
        return status;
    track.w = (int)(bounds.w * feedback->value);
    status = rounded(list, track, 3, 0x6bdcc3, 255);
    if (status != OL_OK)
        return status;
    int radius = feedback->focused ? 8 : 6;
    struct gfx_rect knob = {bounds.x + track.w - radius,
                            bounds.y + bounds.h / 2 - radius, radius * 2, radius * 2};
    return rounded(list, knob, radius, 0xeaf7f2, 255);
}

int ol_ui_progress(struct ol_list *list, struct gfx_rect bounds, float progress)
{
    if (!(progress >= 0 && progress <= 1))
        return OL_ARGUMENT;
    int status = rounded(list, bounds, bounds.h / 2, 0x263d51, 255);
    if (status != OL_OK)
        return status;
    bounds.w = (int)(bounds.w * progress);
    return rounded(list, bounds, bounds.h / 2, 0x6bdcc3, 255);
}
