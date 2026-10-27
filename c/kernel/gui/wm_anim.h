/* wm_anim.h -- window motion: what the other window-manager files use of wm_anim.c.
 * Include wm_internal.h (which includes this), not this file on its own. */
#ifndef LOGIT_WM_ANIM_H
#define LOGIT_WM_ANIM_H

#include "wm_internal.h"

#define NGHOST 4
struct ghost {
    int used;
    struct surface surf;
    int x, y, w, h, dark, tbh;
    uint64_t t0;
};

/* The animation clock: one instant per frame (g_frame_ms, set by wm.c). */
extern uint64_t g_frame_ms;
extern struct rtc_time g_frame_rtc;
void frame_rtc(struct rtc_time *t);
uint64_t anim_stamp(void);
uint64_t anim_elapsed(uint64_t stamp);
uint64_t anim_elapsed_at(uint64_t stamp, uint64_t now);
int ease_out(int t);

/* The open pop and the dock fly. */
int win_open_scale(const struct win *w);
int min_prog(const struct win *w);
void win_set_min(struct win *w, int on);
void win_fly_settle(struct win *w);

/* The close fade's ghosts. */
extern struct ghost g_ghost[NGHOST];
extern int g_nghost;
void ghost_box(const struct ghost *g, struct drect *r);
void draw_ghost(const struct wm_target *t, const struct drect *R, const struct ghost *g);
int ghost_take(int wi);

/* Each feature's part of the animation pass (wm.c: wm_anim_tick), and its trace line. */
void wm_anim_open_tick(uint64_t t);
void wm_anim_close_tick(uint64_t t);
void wm_anim_fly_tick(uint64_t t);
void anim_trace(const char *what, int wi, const struct win *w, int p);

#endif /* LOGIT_WM_ANIM_H */
