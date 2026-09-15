#ifndef LOGIT_POWER_H
#define LOGIT_POWER_H

/* Ask the machine to stop -- the kernel's own side of "poweroff" / "reboot".
 * See power.c for the full order-of-operations argument; the short version:
 * a filesystem sync is the one thing here that must not be skipped, and
 * everything after it is best-effort hardware poking that DEGRADES LOUDLY
 * (a printed reason + a halt) rather than pretending to have worked.
 *
 * Both are noreturn for the same reason panic() is: by the time either could
 * return control to a caller, that caller's address space may already be
 * mid-teardown (poweroff) or the platform may already be resetting (reboot),
 * so there is no sane "and then what" for the syscall that invoked them to
 * do with a return value on the path that worked. The ONLY way a caller
 * observes a normal return is refusal before either of these runs at all
 * (not root -- see the SYS_POWEROFF/SYS_REBOOT cases in syscall.c). */

/* Sync the filesystem, tell ACPI to put the machine in S5 (soft-off), and if
 * that does not work within a couple of tries, halt the CPU forever having
 * said why on serial. Never returns. */
/* Correction: drain/sync/hardware failure restores admission and returns a
 * negative error. Successful poweroff still never returns. */
int kernel_poweroff(void);

/* Reset the machine through three tiers, each tried in turn and each logged:
 * the FADT's RESET_REG (if the table carries a usable one), the legacy 8042
 * keyboard-controller reset pulse, and finally a triple fault. Never returns
 * -- the triple fault tier cannot even in principle hand control back to C,
 * so "noreturn" is not an optimistic promise here, it is a fact about x86. */
/* Correction: preparation failure returns after restoring admission. */
int kernel_reboot(void);

/* Initialize/query real AML power devices after scheduler and ACPI setup.
 * Queries are serialized, may sleep, and return explicit unavailable state. */
struct power_acpi_status;
int kernel_power_init(void);
int kernel_power_status(struct power_acpi_status *snapshot);
/* Boot-only: queue discovery after the scheduler and shared worker exist. */
void kernel_power_start(void);

#endif /* LOGIT_POWER_H */
