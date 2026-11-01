/* Observation summary for the hardware HUD overlay.  It formats from a copy of
 * the counters: no recorder, ledger or control lock is held while text is
 * formatted, and no other module is called back while a module lock is held.
 * Design reference: section 10.4. */
#include "observe_hud.h"
#include "observe_control.h"
#include "../../diag/kprintf.h"

/* One short line that keeps the evidence limits visible: interrupts-off timing
 * is marked partial (bare cli/sti are not seen, so it is not the kernel's
 * maximum), and console and serial bytes are shown because output during a
 * measurement disturbs it.  The time is in TSC cycles. */
int observe_hud_format_summary(char *out, int capacity)
{
    struct sched_observe_status status;
    observe_snapshot_status(&status);
    return ksnprintf(out, capacity,
        "sched run=%lu pending=%lu expired=%lu lost=%lu invalid=%u "
        "irq-off(partial)=%lu console/uart=%lu/%lu",
        (unsigned long)status.header.run_id, (unsigned long)status.inputs_pending,
        (unsigned long)status.inputs_expired, (unsigned long)status.observation_lost_count,
        status.run_invalid, (unsigned long)status.irq_disabled_max_tsc,
        (unsigned long)status.console_bytes, (unsigned long)status.uart_bytes);
}
