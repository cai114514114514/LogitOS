/* Observation summary for the hardware HUD overlay.  It formats from a copy of
 * the counters: no recorder, ledger or control lock is held while text is
 * formatted, and no other module is called back while a module lock is held.
 * Design reference: section 10.4. */
#include "observe_hud.h"
#include "observe_control.h"
#include "../../diag/kprintf.h"

/* One short line (the HUD is 100 columns wide) that keeps the evidence limits
 * visible: lost counts the essential records the run cannot do without and
 * ring is the flight events that were overwritten; interrupts-off timing is
 * marked partial (bare cli/sti are not seen, so it is not the kernel's
 * maximum); CPUs beyond the recorder's limit are counted as unseen; and console
 * and serial bytes are shown because output during a measurement disturbs it.
 * The time is in TSC cycles. */
int observe_hud_format_summary(char *out, int capacity)
{
    struct sched_observe_status status;
    observe_snapshot_status(&status);
    uint64_t ring_overwrites = status.observation_lost_count - status.essential_lost_count;
    return ksnprintf(out, capacity,
        "sched run=%lu pend=%lu exp=%lu lost=%lu ring=%lu inv=%u unseen=%u "
        "irq-off(partial)=%lu con/uart=%lu/%lu",
        (unsigned long)status.header.run_id, (unsigned long)status.inputs_pending,
        (unsigned long)status.inputs_expired, (unsigned long)status.essential_lost_count,
        (unsigned long)ring_overwrites, status.run_invalid, status.unobserved_cpu_count,
        (unsigned long)status.irq_disabled_max_tsc,
        (unsigned long)status.console_bytes, (unsigned long)status.uart_bytes);
}
