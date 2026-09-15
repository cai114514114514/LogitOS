# ACPI power services

`acpi/devices.c` evaluates firmware methods through the full uACPI interpreter.
`acpi/native.c` implements the kernel services it calls. Upstream sources are
kept unchanged in `third_party/uacpi`; that directory records the pinned commit
and license. There is no AML byte-pattern scanner and no guessed sleep type.

## Entry points and ownership

The current consumer is `c/kernel/init/power.c`. `kernel_power_status` initializes
services on demand and samples battery, external-power and thermal devices.
`kernel_power_init` additionally prints a snapshot. Correction to the earlier
"not called at boot" limitation: `wm_run` now calls `kernel_power_start` after
scheduler setup and releasing the WM lock. A separate `acpi-init` thread runs
discovery and publishes battery, thermal and EC diagnostics without waiting
for a shutdown request. Poweroff and reboot also initialize on demand. Call these in sleepable thread
context after `sched_init`, clock setup, ACPI discovery and IOAPIC setup.

The consumer's AML mutex serializes initialization, status queries and sleep
transitions. Initialization failure is retained rather than retrying over a
partly initialized interpreter. Shutdown keeps the existing filesystem drain
and sync. It evaluates `_S5` and `_PTS` through uACPI, then enters S5; a return
means shutdown did not complete. The return path attempts working-state ACPI
event restoration and releases filesystem admission. Reboot uses uACPI's FADT
reset mechanism before the existing 8042 and triple-fault fallbacks.

The native backend owns one SCI vector and will not replace an already active
IOAPIC route. It masks and acknowledges the source before retiring its vector.
An unconfirmed mask retains the vector and callback; the initialization failure
remains latched. The callback's argument must remain alive until retirement.

Deferred work uses a bounded, preallocated 64-entry queue and the existing
sleepable `kworker`; IRQ submission never allocates. Queue exhaustion returns
an error. The initial handshake confirms that `kworker` actually executed a
job before AML initialization begins. uACPI recommends CPU0 for GPE methods to
avoid certain SMI firmware bugs; the current shared worker can migrate. No CPU0
affinity guarantee is advertised.

The RSDP is copied from the kernel's checksum-validated handoff. Its CPU address
is not assumed to be a firmware physical address. Native initialization gives
uACPI the translated physical address of a stable kernel copy. Physical RAM
uses its established direct-map alias; other mappings share ACPI's supervisor
alias and are bounded to 1 MiB per request. Aliases remain mapped because
another table or OperationRegion may still use the same pages.

## Query results

Battery discovery uses `PNP0C0A` and checks `_STA`'s battery-present bit before
sampling `_BST` and `_BIF`. The snapshot preserves ACPI's unknown sentinel and
native units; it does not invent percentages. External power uses `ACPI0003`
and `_PSR`. Thermal zones evaluate `_TMP` and optional `_CRT`, converting tenths
of kelvin to signed millidegrees Celsius. Package shape, integer width, state
flags, unit selection and conversion bounds are checked before publication.

Each method has an explicit error field. No AC adapter result means unknown,
not "on battery". At most eight batteries and sixteen thermal zones are retained;
a truncation flag reports additional devices. Paths have fixed storage bounds.

SystemMemory, SystemIO and segment-zero PCI configuration accesses have native
backends. Correction to the original "Embedded-controller ... handlers are not
implemented" boundary: the private SystemIO EC subset now has a real transport,
OperationRegion handler and deferred GPE query path; see `acpi/ec/README.md` for
its exact resource restrictions. Shared `_GLK` ECs, IO aliases, GPIO interrupts
and early ECDT bootstrap remain unsupported. SMBus and other OperationRegion
handlers are still absent. Discovery does not establish a successful method
measurement. `_BIX`, notifications to a desktop UI, suspend/resume, CPU power
state writes and physical-machine validation remain outside this implementation.

## Verification

Run `make BUILD=build/drivers/power test-power-aml-host`. The ASan/UBSan host gate
links production `devices.c` with the unchanged interpreter and a bounded test
board backend. The board's FADT explicitly declares hardware-reduced ACPI;
unsupported host I/O, PCI and IRQ operations return errors. This tests AML
execution and decoding, not native hardware interrupts.

The thermal method increments a namespace counter, so successive queries must
produce different measured values. Cases cover an empty battery bay, unknown
capacity, a wrongly typed battery package, a missing method, an invalid thermal
value, an invalid `_S5` object and a corrupted DSDT checksum. A prerequisite
negative control changes the production Kelvin conversion in a build-directory
copy and must fail exactly three consumer assertions.

The existing `test-acpi-integrity-host` separately checks RSDP/SDT integrity and
its checksum/signature negative controls. The integrated kernel was also tested
with `tests/boot/run-power-test.sh`: QEMU loaded its real firmware AML, powered
off by itself, preserved the test file without journal replay, and booted again
after the reboot request. That proves this path in QEMU, not on the user's X79
or newer physical board.
