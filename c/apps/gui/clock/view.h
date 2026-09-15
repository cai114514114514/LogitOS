#ifndef CLOCK_VIEW_H
#define CLOCK_VIEW_H
#include "aui.h"
#include "openlogit_canvas.h"
#include "openlogit_window.h"
#include "../../clock/model.h"
#include "layout.h"
#include "motion.h"
struct clock_ui {
    unsigned frames;
    struct clock_motion motion;
};
int clock_dial_init(void);
void clock_dial_invalidate(void);
int clock_dial_draw(struct clock_layout, const struct logit_time *, int second, int show_seconds);
enum clock_action clock_view(struct clock_ui *, const struct clock_state *,
                            const struct logit_time *, uint64_t now, int reduced, int refresh);
#endif
