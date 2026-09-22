#ifndef LOGIT_JS_TASK_BUDGET_H
#define LOGIT_JS_TASK_BUDGET_H

/* One turn budget for page phases and their interleaved workers. This is a
 * boundary check, not preemption: one synchronous JS/native call may exceed
 * it, but its return must not start another whole batch before input/paint.
 * 8 ms leaves part of a typical frame for layout and painting; the finite
 * scheduler fixture advances the injected clock by 20 ms inside one ordinary
 * arithmetic callback so the old batch behaviour is observable, not timed. */
#define JS_TASK_TURN_MS 8u
static inline int js_task_budget_expired(unsigned long long now,
                                         unsigned long long deadline)
{
#ifdef JS_TASK_UNBOUNDED_TURN
    (void)now; (void)deadline;
    return 0;
#else
    return now >= deadline;
#endif
}
#endif
