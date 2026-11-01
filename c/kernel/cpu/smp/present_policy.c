#include "present_policy.h"

/* Number of set bits in the history word, i.e. timeouts among the last 64
 * presents. Kept local so the file builds on the host and in the kernel. */
static int count_timeouts_in_window(uint64_t history)
{
    int count = 0;
    while (history) {
        history &= history - 1;
        count++;
    }
    return count;
}

/* WHY: the verdict must be a pure function of the present history so the host
 * test can prove it without a kernel; smp.c only supplies the timed_out flag
 * and acts on the single "disable" result. */
int smp_present_policy(struct smp_present_window *window, int timed_out)
{
    window->history = (window->history << 1) | (timed_out ? 1u : 0u);
    if (window->calls < 0xffffffffu) window->calls++;
    window->consecutive = timed_out ? window->consecutive + 1 : 0;
    if (window->disabled) return 0;
    int too_many_in_row = window->consecutive >= SMP_PRESENT_CONSECUTIVE_LIMIT;
    int window_full = window->calls >= SMP_PRESENT_WINDOW_SIZE;
    int too_many_in_window = window_full &&
        count_timeouts_in_window(window->history) >= SMP_PRESENT_WINDOW_TIMEOUTS;
    if (!too_many_in_row && !too_many_in_window) return 0;
    window->disabled = 1;
#ifdef SMP_PRESENT_NEGCTL_NEVER_DISABLE
    /* Negative control: a policy that never reports "disable" must turn the
     * consecutive-timeout test red. */
    return 0;
#else
    return 1;
#endif
}
