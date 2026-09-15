/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUI_SCROLL_MOTION_H
#define AUI_SCROLL_MOTION_H
#include "openlogit_anim.h"

/* AUI owns the caller's offset and input target; OpenLogit owns interpolation.
 * Keep pixel values here, not aui_anim's normalized 0..255: a long transcript
 * otherwise advances by whole lines at each quantized position. No clock or
 * wake is hidden here, so dropped frames and uint32 millisecond wrap are testable.
 */
struct aui_scroll_motion {
    int from, target, current, limit, running;
    unsigned start_ms;
};
static int aui_scroll_clamp(long long value, int limit)
{ return value < 0 ? 0 : value > limit ? limit : (int)value; }

static void aui_scroll_sync(struct aui_scroll_motion *s, int value, int limit)
{
    s->limit = limit < 0 ? 0 : limit;
    s->from = s->target = s->current = aui_scroll_clamp(value, s->limit);
    s->running = 0;
}

static int aui_scroll_sample(struct aui_scroll_motion *s, int external, int limit,
                             long long delta, unsigned now, unsigned duration,
                             int reset, int direct, int reduced)
{
    if (limit < 0) limit = 0;
    /* External assignments (Home/End, live transcript follow, drag) win over
     * a pending wheel target. A geometry change cancels the old leg at the
     * current clamped offset; it must never resurrect an out-of-range target.
     */
    if (reset || direct || external != s->current || limit != s->limit)
        aui_scroll_sync(s, external, limit);
    if (!direct && delta) {
        int goal = aui_scroll_clamp((long long)s->target + delta, limit);
        if (goal != s->target) {
            /* Retarget from the last displayed position. Accumulate inputs
             * against the target, so ten notches still travel ten notches
             * when several arrive before another frame can be shown. */
            s->from = s->current; s->target = goal; s->start_ms = now;
            s->running = s->from != s->target;
        }
    }
#if defined(AUI_SCROLL_INSTANT) || defined(AUI_ANIM_OFF)
    reduced = 1; /* control: same input/layout, remove only interpolation */
#endif
    if (s->running) {
        unsigned elapsed = now - s->start_ms;
        s->current = ol_transition256(s->from, s->target, elapsed, duration,
                                      OL_EASE_OUT_CUBIC, reduced);
        if (reduced || !duration || elapsed >= duration) {
            s->current = s->target; s->running = 0;
        }
    }
    return s->current;
}
#endif
