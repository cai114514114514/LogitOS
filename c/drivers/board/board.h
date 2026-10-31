/* Board bring-up: identify the motherboard, make the PCH safe to run on
 * (TCO watchdog), and give the reboot path the PCH's own reset.
 * X79-VG4 phase 1, task T8 of docs/superpowers/plans/2026-10-31-x79-vg4-phase1.md.
 * Model and QEMU evidence only; not verified on X79-VG4 hardware. */
#ifndef BOARD_H
#define BOARD_H

/* Find the PCH LPC bridge and halt a TCO watchdog firmware left counting.
 * Called once from kernel_main before storage probing, while it is the only
 * thread; everything below relies on that for the LPC snapshot it publishes. */
void board_early(void);

/* SMBIOS report, TCO result, Super I/O identification ("[board]" lines).
 * Runs once from the acpi-init thread; user programs may already be running. */
void board_init(void);

/* Last reboot tier before the legacy ones: the PCH's 0xCF9 cold reset. */
void board_reset(void);

#endif
