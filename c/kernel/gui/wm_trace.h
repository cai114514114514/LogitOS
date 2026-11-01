/* Slow-frame wire format, independent of kernel clocks and console I/O so the
 * host can check exactly what the next hardware run will report. */
#ifndef LOGIT_WM_TRACE_H
#define LOGIT_WM_TRACE_H

#include <stddef.h>
#include <stdint.h>

struct wm_slowframe {
    uint64_t seq, total_ns, cpu_ns, lockwait_ns, layers_ns, top_ns;
    uint64_t present_ns, glass_ns, px;
    unsigned rects, par, full_reasons;
};

/* Same inclusive 33 ms budget as the serial policy; accumulation is separate
 * from its one-second rate limit so a burst is not counted as one frame.
 * Kernel callers hold graphics_lock for note and while copying worst/count.
 * The returned pointer is borrowed, not an immutable snapshot across unlock. */
void wm_slowframe_note(const struct wm_slowframe *frame);
const struct wm_slowframe *wm_slowframe_worst(uint64_t *count);

/* snprintf's sizing contract: return the complete length, excluding NUL;
 * write at most n-1 characters and terminate whenever n is nonzero. */
size_t wm_slowframe_format(char *buf, size_t n, const struct wm_slowframe *frame);

/* The P10 diagnostic budget is 33 ms and one line per second. A zero last_ms
 * means no line has been emitted yet, including during the first second. */
int wm_slowframe_wanted(uint64_t total_ns, uint64_t now_ms, uint64_t *last_ms);

#endif
