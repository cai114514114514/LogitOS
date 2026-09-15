/* Ask the machine to stop -- poweroff and reboot, the first time this kernel
 * has ever been ABLE to stop itself rather than being yanked from outside.
 *
 * THE ORDER IS THE FEATURE. kernel_poweroff() is not "write S5 to a PM1
 * register" with a sync bolted on for safety; the sync IS the correctness
 * requirement and the ACPI write is the part that is allowed to fail loudly.
 * Concretely, in order:
 *
 *   1. kprintf "[power] syncing..." -- a greppable marker BEFORE anything that
 *      could go wrong, so a hang here is distinguishable on serial from a hang
 *      after.
 *   2. logitfs_sync() -- LogitFS's own whole-device sync (bcache_sync(), which
 *      writes every dirty buffer and then issues blk_flush()'s write barrier;
 *      see c/fs/bcache.h and c/drivers/block/blkdev.h). This is the exact call
 *      logitfs.c's own fsck and "sync for shutdown" paths use -- see the
 *      comment on logitfs_sync() in c/fs/logitfs.h, which says "for shutdown"
 *      outright. Reimplementing that ordering here (a hand-rolled loop over
 *      bcache + a second blk_flush()) would risk drifting from the one
 *      ordering argument the journal's barrier comment (log_commit() in
 *      logitfs.c) actually proves -- calling the function that already proves
 *      it is strictly safer than restating it.
 *   3. kprintf "[power] going down" -- the marker a poweroff test greps for.
 *      Distinguishing a clean shutdown from a crash is BY ABSENCE: the SIGKILL
 *      harness (tests/boot/run-fscrash-test.sh) counts "[fs] log: replayed" on
 *      the NEXT boot as proof a crash happened. A clean poweroff leaves no
 *      open transaction, so there is nothing to replay -- the marker here is
 *      not itself the durability proof, the absence of a replay line on the
 *      following boot is.
 *   4. The ACPI S5 write, tried twice (SLP_TYP=0 then 5, see below), and if
 *      neither takes, a LOUD halt -- never a silent one and never a return.
 *
 * WHAT IS DELIBERATELY NOT HERE: a per-process teardown pass. Processes do
 * not get goodbye letters. The filesystem sync above is the only state that
 * matters at shutdown -- an F_VFS file's contents are already durable at the
 * point a write() returns (LogitFS commits per-op, not per-close; see
 * logitfs.h), so a process that still has one open when the machine goes down
 * loses nothing a plain power cut would not also have lost, and the journal
 * that survives a SIGKILL sweep (tests/boot/run-fscrash-test.sh, 4 kills) by
 * construction survives this. Walking the process table to notify anybody
 * would be inventing a guarantee this kernel does not otherwise make and does
 * not need to make here.
 *
 * THE SLP_TYP VALUE IS THE HONEST PROBLEM. The true value is whatever the
 * platform's DSDT declares in its \_S5 package, and reading a DSDT means
 * running AML -- this kernel has no AML interpreter (a real one is a named
 * follow-up, not a silent gap: see the note at the bottom of this file).
 * Two values cover every machine this kernel actually boots on: QEMU's
 * \_S5 is SLP_TYP=0, and 5 is the SeaBIOS/Bochs-era convention older or
 * alternate BIOS/VMM ACPI tables use for the same state. Trying both, in
 * that order, with a short spin after each, is not a guess dressed up as a
 * protocol -- it is what actually happens after each write is observable
 * (the machine either powers off, in which case nothing after it runs, or it
 * does not, in which case control returns here and the next tier fires).
 *
 * kernel_reboot() is a separate, similarly honest three tiers: RESET_REG (if
 * the FADT carries one this kernel can act on), the legacy 8042 pulse, and a
 * triple fault -- the one tier that genuinely cannot fail to stop the CPU,
 * because a fault with nowhere to go is a platform reset by definition on
 * this architecture, not merely by convention. */

/* BKL-removal correction to the original shutdown contract above: sync
 * failure no longer powers through, and unsuccessful hardware poweroff no
 * longer strands all filesystems. Admission is drained per explicit request
 * and restored on failure; successful hardware shutdown still never returns. */

#include <stdint.h>
#include "power.h"
#include "acpi.h"
#include "io.h"
#include "ktime.h"
#include "kprintf.h"
#include "logitfs.h"
#include "vfs.h"
#include "wait.h"
#include "sched.h"
#include "../../drivers/power/acpi/devices.h"
#include "../../drivers/power/acpi/native.h"

/* Only power requests serialize here. Drain is a reversible token: already
 * admitted filesystem work finishes with its CPUs running; every failure
 * below returns the token before returning to the caller. */
static struct mutex power_lock = MUTEX_INIT;
static int power_prepare(struct vfs_drain *token)
{
    mutex_lock(&power_lock);
    int rc = vfs_drain_begin(token);
    if (rc) {
        mutex_unlock(&power_lock);
        return rc;
    }
    kprintf("[power] syncing...\n");
    if (logitfs_sync()) {
        vfs_drain_end(token);
        mutex_unlock(&power_lock);
        kprintf("[power] sync failed; filesystem admission restored\n");
        return -5;
    }
    return 0;
}
static int power_cancel(struct vfs_drain *token, const char *why)
{
    vfs_drain_end(token);
    mutex_unlock(&power_lock);
    kprintf("[power] %s; filesystem admission restored\n", why);
    return -5;
}

/* Bounded busy-wait, in the style of the driver-probe udelay()s elsewhere in
 * the tree (c/drivers/audio/hda.c): the ns clock has been up since long
 * before any code could reach here, so there is no "coarse fallback" branch
 * to carry -- this is always the ktime path. */
static void spin_ms(unsigned ms)
{
    uint64_t end = time_mono_ns() + (uint64_t)ms * 1000000ull;
    while (time_mono_ns() < end)
        __asm__ volatile ("pause");
}

/* Correction to the original guessed _S5/physical RESET_REG description:
 * uACPI now evaluates the platform's namespace and drives its Generic Address
 * registers. A missing interpreter/platform service is an error, never a
 * reason to try arbitrary sleep types or dereference a physical address. */
static struct mutex aml_lock = MUTEX_INIT;
static int aml_initialization_attempted;
static int aml_initialization_error;

static int initialize_aml_locked(void)
{
    if (aml_initialization_attempted)
        return aml_initialization_error;
    aml_initialization_attempted = 1;
    aml_initialization_error = power_acpi_native_start();
    if (!aml_initialization_error)
        aml_initialization_error = power_acpi_start();
    if (aml_initialization_error)
        kprintf("[power] ACPI interpreter unavailable: status=%d\n",
                aml_initialization_error);
    return aml_initialization_error;
}

int kernel_power_status(struct power_acpi_status *snapshot)
{
    if (!snapshot)
        return -22;
    mutex_lock(&aml_lock);
    int status = initialize_aml_locked();
    /* Even initialization failure produces an explicit unavailable snapshot. */
    int query_status = power_acpi_query(snapshot);
    if (status)
        snapshot->error = status;
    mutex_unlock(&aml_lock);
    return status || query_status ? -5 : 0;
}

int kernel_power_init(void)
{
    struct power_acpi_status snapshot;
    int status = kernel_power_status(&snapshot);
    kprintf("[power] AML ready=%d batteries=%d thermal_zones=%d AC=%d error=%d\n",
            snapshot.ready, snapshot.battery_count, snapshot.thermal_count,
            snapshot.ac_online, snapshot.error);
    kprintf("[power] EC bound=%u error=%d\n", snapshot.ec_count, snapshot.ec_error);
    for (int index = 0; index < snapshot.battery_count; index++) {
        const struct power_battery *battery = &snapshot.batteries[index];
        kprintf("[power] battery %s present=%d state=%u remaining=%u unit=%u error=%d\n",
                battery->path, battery->present, battery->state, battery->remaining,
                battery->unit, battery->status_error);
    }
    for (int index = 0; index < snapshot.thermal_count; index++) {
        const struct power_thermal *thermal = &snapshot.thermals[index];
        kprintf("[power] thermal %s millicelsius=%d error=%d\n",
                thermal->path, thermal->temperature_millic, thermal->status_error);
    }
    return status;
}

static void power_boot_worker(void)
{
    (void)kernel_power_init();
}

void kernel_power_start(void)
{
    /* AML initialization can sleep and dispatch GPE work. Use a separate
     * thread after scheduler startup: running on kworker would wait for that
     * same worker's startup probe, and running under wm_lock stalls input. */
    thread_create(power_boot_worker, "acpi-init");
}

int kernel_poweroff(void)
{
    struct vfs_drain token = {0};
    int rc = power_prepare(&token);
    if (rc)
        return rc;

    kprintf("[power] going down\n");

    mutex_lock(&aml_lock);
    int status = initialize_aml_locked();
    if (!status)
        status = power_acpi_prepare_off();
    if (status) {
        mutex_unlock(&aml_lock);
        return power_cancel(&token, "ACPI S5 preparation failed");
    }

    kprintf("[power] entering firmware-evaluated S5\n");
    status = power_acpi_enter_off();
    /* A normal return means the machine is still executing. Restore ACPI's
     * working-state event setup before reopening filesystem admission. */
    int restore_status = power_acpi_restore_working();
    kprintf("[power] S5 returned status=%d restore=%d\n", status, restore_status);
    mutex_unlock(&aml_lock);
    return power_cancel(&token, "S5 did not power off the machine");
}

int kernel_reboot(void)
{
    struct vfs_drain token = {0};
    int rc = power_prepare(&token);
    if (rc)
        return rc;
    kprintf("[power] rebooting\n");

    mutex_lock(&aml_lock);
    int reset_status = initialize_aml_locked();
    if (!reset_status)
        reset_status = power_acpi_reset();
    mutex_unlock(&aml_lock);
    kprintf("[power] ACPI reset returned status=%d; trying legacy reset\n",
            reset_status);

    /* Tier 2: the legacy 8042 keyboard-controller pulse -- write the
     * "pulse output port, drive reset low" command byte. Every PC-compatible
     * platform this kernel targets, real or emulated, still honours it. */
    kprintf("[power] reboot: 8042 pulse\n");
    outb(0x64, 0xFE);
    spin_ms(50);
    kprintf("[power] reboot: 8042 pulse did not take -- triple fault\n");

    /* Tier 3: load a zero-limit IDT, then fault. With IDTR.limit == 0, EVERY
     * vector lookup is out of bounds -- so int3 takes #GP (its own IDT entry
     * is unreachable), the CPU's attempt to deliver THAT #GP takes another
     * #GP (same reason), which is the double-fault condition and escalates to
     * #DF -- whose delivery is unreachable for the same reason again, which
     * is the triple-fault condition and resets the CPU. Each step is a fact
     * about a zero IDT limit, not a hope, so this is not a "best effort"
     * tier: there is no C code path that continues past it on real hardware
     * or under QEMU's default (non `-no-reboot`) behaviour. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = { 0, 0 }, previous;
    __asm__ volatile ("sidt %0" : "=m"(previous));
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    __asm__ volatile ("int3");

    /* Unreachable except under a harness that deliberately suppresses the
     * platform reset (e.g. QEMU `-no-reboot`, which turns the triple fault
     * into a clean exit instead) -- previously halted loudly. If a monitor
     * returns control at all, restore the IDT before cancelling the drain. */
    __asm__ volatile ("lidt %0" : : "m"(previous));
    return power_cancel(&token, "triple fault returned control");
}
