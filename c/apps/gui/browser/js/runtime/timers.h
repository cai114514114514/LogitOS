#ifndef LOGIT_JS_TIMERS_H
#define LOGIT_JS_TIMERS_H

#include "quickjs.h"
#include <stdint.h>
#include <limits.h>
#include <math.h>

/* HTML timer initialization, shared by Window and WorkerGlobalScope.
 * This is header-only because page.c and worker.c have independent host
 * consumers: another source-list entry would let a harness silently omit the
 * algorithm it claims to exercise. Scheduling and ownership stay with each
 * event loop; only the Web IDL conversion and timer rules belong here.
 * https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers */
struct js_timer_state {
    int32_t timeout;
    unsigned nesting;
    unsigned repeat;
    unsigned cancelled;
};

enum { JS_TIMER_NESTING_LIMIT = 5, JS_TIMER_MIN_DELAY = 4 };

static inline int js_timer_long(JSContext *ctx, int32_t *value, JSValueConst input)
{
    double number;
    if (JS_ToFloat64(ctx, &number, input) < 0) return -1;
    if (!isfinite(number) || number == 0) { *value = 0; return 0; }
    /* Previous claim: QuickJS ToInt32 supplies Web IDL long conversion.
     * Its large-negative-float path negates INT32_MIN: clearTimeout with
     * -2147483648.5 produced "negation of -2147483648" under UBSan. Convert
     * through ToNumber and IntegerPart, then the standard's modulo arithmetic;
     * cast only after the result is representable, never rely on signed wrap.
     * https://webidl.spec.whatwg.org/#abstract-opdef-converttoint */
    number = trunc(number);
    if (number < INT32_MIN || number > INT32_MAX) {
        const double modulus = (double)UINT32_MAX + 1.0;
        number = fmod(number, modulus);
        if (number < 0) number += modulus;
        if (number >= modulus / 2) number -= modulus;
    }
    *value = (int32_t)number;
    return 0;
}

static inline int js_timer_convert(JSContext *ctx, int argc,
                                   JSValueConst *argv, int32_t *timeout)
{
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "timer requires a handler");
        return -1;
    }
    int callable = JS_IsFunction(ctx, argv[0]);
    if (!callable) {
        /* The previous implementation returned handle 0 for every non-callable
         * handler. String timers are part of HTML, not a deprecated no-op, but
         * their TrustedScript/CSP sink and initiating-script fetch metadata are
         * not implemented here. Keep that refusal explicit. Do the Web IDL
         * conversion first so a throwing toString is never swallowed. */
        JSValue text = JS_ToString(ctx, argv[0]);
        if (JS_IsException(text)) return -1;
        JS_FreeValue(ctx, text);
    }
    *timeout = 0;
    /* Saturating to INT_MAX silently changes wrapped delays; ignoring the
     * conversion's return schedules a timer after a Symbol/valueOf exception. */
    if (argc > 1 && js_timer_long(ctx, timeout, argv[1]) < 0) return -1;
    if (!callable) {
        JS_ThrowTypeError(ctx, "string timer handlers are not supported (TrustedScript/CSP)");
        return -1;
    }
    return 0;
}

static inline void js_timer_init(struct js_timer_state *timer, int32_t timeout,
                                 unsigned nesting, int repeat)
{
    if (timeout < 0) timeout = 0;
    /* The clamp tests the RUNNING task's level before incrementing the newly
     * created task's level. Testing after increment clamps one invocation too
     * early. A repeating timer uses this same path even when timeout is zero;
     * repeat must never be encoded by timeout > 0. */
#ifndef JS_TIMER_NO_CLAMP
    /* Negative control: skip only the minimum delay, keeping conversion,
     * nesting increments, cancellation and task ordering intact. */
    if (nesting > JS_TIMER_NESTING_LIMIT && timeout < JS_TIMER_MIN_DELAY)
        timeout = JS_TIMER_MIN_DELAY;
#endif
    timer->timeout = timeout;
    timer->nesting = nesting < UINT_MAX ? nesting + 1 : UINT_MAX;
    timer->repeat = !!repeat;
    timer->cancelled = 0;
}

static inline void js_timer_repeat(struct js_timer_state *timer)
{
    js_timer_init(timer, timer->timeout, timer->nesting, 1);
}

static inline int js_timer_next_id(int *next)
{
    int id = *next > 0 ? *next : 1;
    /* Signed ++ overflow is undefined; wrapping to a negative handle also
     * makes a live timer uncancellable by a Web IDL long. Callers must still
     * check their own live ID map before accepting this candidate. */
    *next = id == INT32_MAX ? 1 : id + 1;
    return id;
}

/* js_dom_run_jobs uses this bridge around its entire checkpoint, including
 * checkpoints entered synchronously from inside a timer's DOM event callback.
 * A Promise/queueMicrotask job is a different task and starts timer nesting at
 * zero; the still-running outer timer resumes with its original level. */
unsigned js_page_timer_checkpoint_begin(JSContext *ctx);
void js_page_timer_checkpoint_end(JSContext *ctx, unsigned nesting);

#endif
