#include "wm_trace.h"

/* P10's inclusive 33 ms budget is shared by history and serial selection.
 * Keeping it here prevents the HUD and log from disagreeing at the boundary. */
#define SLOWFRAME_BUDGET_NS UINT64_C(33000000)

static struct wm_slowframe worst_frame;
static uint64_t slow_count;

/* Remember every over-budget frame, even when serial output is rate-limited.
 * graphics_lock serializes the production caller and the HUD's snapshot;
 * this pure module deliberately takes no scheduler or console lock. */
void wm_slowframe_note(const struct wm_slowframe *frame)
{
    if (!frame || frame->total_ns < SLOWFRAME_BUDGET_NS) {
        return;
    }
    if (!slow_count || frame->total_ns > worst_frame.total_ns) {
        worst_frame = *frame;
    }
    slow_count++;
}

/* Return the matching count and borrowed record under the caller's graphics
 * lock. NULL distinguishes no measured slow frame from a fabricated sample. */
const struct wm_slowframe *wm_slowframe_worst(uint64_t *count)
{
    if (count) {
        *count = slow_count;
    }
    return slow_count ? &worst_frame : NULL;
}

/* A bounded writer avoids a libc dependency in the kernel. Its length keeps
 * growing after truncation, which preserves snprintf's sizing contract. */
struct trace_writer {
    char *buf;
    size_t capacity;
    size_t length;
};

/* Count every byte even when it cannot be stored; always reserve room for the
 * terminator, which the public formatter writes once at the end. */
static void trace_char(struct trace_writer *writer, char value)
{
    if (writer->capacity && writer->length < writer->capacity - 1) {
        writer->buf[writer->length] = value;
    }
    writer->length++;
}

/* Emit a literal without formatting machinery: this pure module also runs in
 * host tests that deliberately do not link the kernel's console formatter. */
static void trace_text(struct trace_writer *writer, const char *text)
{
    while (*text) {
        trace_char(writer, *text++);
    }
}

/* A uint64_t needs at most 20 decimal digits (16 hexadecimal digits). Reverse
 * into local storage first so zero and UINT64_MAX share the same path. */
static void trace_number(struct trace_writer *writer, uint64_t value, unsigned base)
{
    static const char digits[] = "0123456789abcdef";
    char reversed[20];
    size_t count = 0;
    do {
        reversed[count++] = digits[value % base];
        value /= base;
    } while (value);
    while (count) {
        trace_char(writer, reversed[--count]);
    }
}

/* Keep labels and units in one place. Durations are truncated to microseconds,
 * but subtraction happens in ns first so fractional microseconds cannot wrap. */
size_t wm_slowframe_format(char *buf, size_t n, const struct wm_slowframe *frame)
{
    struct trace_writer writer = {buf, n, 0};
    uint64_t blocked_ns = frame->total_ns > frame->cpu_ns ?
                          frame->total_ns - frame->cpu_ns : 0;
    const char *const labels[] = {
        "[wm] slowframe seq=", " total_us=", " cpu_us=", " blocked_us=",
        " lock_us=", " layers_us=", " top_us=", " present_us=", " glass_us=",
        " rects=", " px=", " par="
    };
    const uint64_t values[] = {
        frame->seq, frame->total_ns / 1000, frame->cpu_ns / 1000,
        blocked_ns / 1000, frame->lockwait_ns / 1000, frame->layers_ns / 1000,
        frame->top_ns / 1000, frame->present_ns / 1000, frame->glass_ns / 1000,
        frame->rects, frame->px, frame->par
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        trace_text(&writer, labels[i]);
        trace_number(&writer, values[i], 10);
    }
    trace_text(&writer, " full=0x");
    trace_number(&writer, frame->full_reasons, 16);
    trace_char(&writer, '\n');
    if (n) {
        size_t end = writer.length < n ? writer.length : n - 1;
        buf[end] = '\0';
    }
    return writer.length;
}

/* Serial output is itself expensive. P10's inclusive 33 ms threshold and
 * 1000 ms interval bound that perturbation; short frames never spend the slot.
 * The negative control must break the actual policy, not the test's clock. */
int wm_slowframe_wanted(uint64_t total_ns, uint64_t now_ms, uint64_t *last_ms)
{
#ifdef GFX_NEGCTL_TRACE_NO_RATELIMIT
    (void)total_ns;
    (void)now_ms;
    (void)last_ms;
    return 1;
#else
    if (total_ns < SLOWFRAME_BUDGET_NS) {
        return 0;
    }
    if (*last_ms && (now_ms < *last_ms || now_ms - *last_ms < 1000)) {
        return 0;
    }
    *last_ms = now_ms;
    return 1;
#endif
}
