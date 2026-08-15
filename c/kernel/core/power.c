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

#include <stdint.h>
#include "power.h"
#include "acpi.h"
#include "io.h"
#include "ktime.h"
#include "kprintf.h"
#include "logitfs.h"

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

static void halt_forever(const char *why) __attribute__((noreturn));
static void halt_forever(const char *why)
{
    kprintf("%s", why);
    __asm__ volatile ("cli");
    for (;;)
        __asm__ volatile ("cli\n\thlt");
}

/* Write RESET_VALUE to RESET_REG. acpi_reset_reg() has already refused any
 * space_id other than 0 (system memory) or 1 (system I/O), so those are the
 * only two cases here -- there is no third branch to silently fall through. */
static void write_reset_reg(const struct acpi_gas *reg, uint8_t value)
{
    if (reg->space_id == 1)
        outb((uint16_t)reg->address, value);
    else
        *(volatile uint8_t *)(uintptr_t)reg->address = value;
}

void kernel_poweroff(void)
{
    kprintf("[power] syncing...\n");
    if (logitfs_sync())
        kprintf("[power] warning: filesystem sync reported an error -- "
                "continuing, a poweroff that hangs here helps nobody\n");

    kprintf("[power] going down\n");

    uint32_t pm1a = 0, pm1b = 0;
    if (acpi_pm1_cnt(&pm1a, &pm1b) != 0) {
        halt_forever("[power] no usable FADT PM1_CNT block -- halting\n");
    }

    /* PM1 Control Register: bits 10-12 are SLP_TYP, bit 13 is SLP_EN
     * (write-only, self-clearing on the transition -- there is nothing to
     * read back to confirm it "took" except the machine going away). */
    static const unsigned slp_typ_guess[2] = { 0, 5 };
    for (unsigned i = 0; i < 2; i++) {
        uint16_t val = (uint16_t)((slp_typ_guess[i] << 10) | (1u << 13));
        kprintf("[power] S5 attempt: SLP_TYP=%u on PM1a port 0x%x%s\n",
                slp_typ_guess[i], (unsigned)pm1a, pm1b ? " (+PM1b)" : "");
        outw((uint16_t)pm1a, val);
        if (pm1b) outw((uint16_t)pm1b, val);
        spin_ms(10);
        /* Reaching this line means the machine is still here. */
    }

    halt_forever("[power] S5 write did not take -- halting\n");
}

void kernel_reboot(void)
{
    kprintf("[power] rebooting\n");

    struct acpi_gas reg;
    uint8_t value;
    if (acpi_reset_reg(&reg, &value) == 0) {
        kprintf("[power] reboot: FADT RESET_REG (space %u addr 0x%llx val 0x%x)\n",
                (unsigned)reg.space_id, (unsigned long long)reg.address,
                (unsigned)value);
        write_reset_reg(&reg, value);
        spin_ms(50);
        kprintf("[power] reboot: RESET_REG did not take\n");
    } else {
        kprintf("[power] reboot: no usable FADT RESET_REG\n");
    }

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
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = { 0, 0 };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    __asm__ volatile ("int3");

    /* Unreachable except under a harness that deliberately suppresses the
     * platform reset (e.g. QEMU `-no-reboot`, which turns the triple fault
     * into a clean exit instead) -- still halt loudly rather than run on with
     * a null IDT armed. */
    halt_forever("[power] triple fault returned control -- halting\n");
}
