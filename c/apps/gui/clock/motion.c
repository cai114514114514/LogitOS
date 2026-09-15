#include "motion.h"
#include "openlogit_anim.h"
int clock_second(struct clock_motion *m, int second, uint64_t now, int reduced, int *active)
{
    *active = 0;
    if (!m->initialized || reduced || now < m->start) {
        *m = (struct clock_motion){1, second, second * 1024, now};
        return second * 1024;
    }
    if (m->second != second) {
        /* 59 -> 0 travels one tick clockwise. Clock edits and missed ticks
         * snap to truth instead of replaying animation after a long sleep. */
        m->from = (m->second + 1) % 60 == second ? (second - 1) * 1024 : second * 1024;
        m->second = second;
        m->start = now;
    }
    uint64_t elapsed = now - m->start;
    *active = elapsed < 180000000 && m->from != second * 1024;
#ifdef CLOCK_MOTION_DISABLED
    return second * 1024; /* The independent intermediate-position oracle fails. */
#else
    return ol_transition256(m->from, second * 1024, elapsed, 180000000, OL_EASE_OUT, 0);
#endif
}
