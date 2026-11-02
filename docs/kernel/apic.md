# Local APIC and I/O APIC

LogitOS uses local APICs for CPU timer interrupts and interprocessor interrupts (IPIs). I/O APICs route device interrupt lines to CPU vectors, while PCI MSI and MSI-X send interrupt messages to a local APIC. The controller implementations live in `c/kernel/cpu/irq/`, boot routing policy lives in `c/kernel/cpu/smp/smp.c`, and PCI interrupt management lives in `c/kernel/pci/pci_msi.c`.

## Local APIC setup

`lapic_init()` reads CPUID leaf 1 and `IA32_APIC_BASE`. The decoder in `apic_model_access_mode()` checks CPU support, the hardware enable bit and the selected register interface.

| Mode | Register access | APIC ID |
|---|---|---|
| xAPIC | Memory mapped registers at the base from `IA32_APIC_BASE` | Upper eight bits of the ID register |
| x2APIC | MSRs addressed as `0x800 + (register_offset >> 4)` | Full 32 bit ID register |

Production initialization preserves the mode supplied by firmware. Each application processor (AP) checks its mode against the mode established by the bootstrap processor (BSP). These checks and register backends are in `c/kernel/cpu/irq/lapic.c` and `c/kernel/cpu/irq/apic_model.c`.

For xAPIC, initialization identity maps the register page with `VMM_WRITABLE | VMM_NOCACHE`. Both backends set the software enable bit, select spurious vector 255 and write zero to the EOI register. The common interface in `c/kernel/cpu/irq/lapic.h` provides `lapic_id()` and `lapic_eoi()`.

## Discovery and boot routing

The MADT parser in `c/kernel/cpu/acpi/acpi.c` collects CPU identities, I/O APIC addresses and GSI bases, and ISA interrupt source overrides. A **GSI**, or global system interrupt, identifies an interrupt input. Its redirection table index is the GSI minus the controller’s GSI base.

CPU discovery accepts enabled processor records of type 0 and type 9. Helpers in `c/kernel/cpu/irq/apic_model.c` retain the APIC ID and ACPI processor UID, deduplicate matching records and reject conflicting identities. I/O APIC discovery validates register addresses and retains the first record for a repeated address or GSI base, using `c/kernel/cpu/acpi/madt_irq.c`.

Early boot calls `pic_remap()` and `pit_init(TIMER_HZ)`. After physical memory initialization, `lapic_virtual_wire()` configures BSP LINT0 for ExtINT delivery from the PIC, LINT1 for NMI delivery and task priority zero. This establishes a PIC delivery path across firmware handoffs. Platform clock initialization also disarms armed HPET comparators and clears legacy replacement routing before the PIT tick probe. The call order is in `c/kernel/init/kmain.c`; controller setup is in `c/kernel/cpu/irq/lapic.c` and `c/drivers/timer/hpet.c`.

Later, `smp_init()` initializes the BSP local APIC and attempts to transfer legacy device delivery to the I/O APIC. `ioapic_route_legacy_set()` installs these routes to the BSP:

| ISA IRQ | Source | CPU vector |
|---|---|---|
| 0 | PIT | 32 |
| 1 | PS/2 keyboard | 33 |
| 12 | PS/2 mouse | 44 |

Each route takes its GSI, polarity and trigger mode from the MADT override mapping. The transaction checks destination support and duplicate GSIs before programming. A programming failure triggers masking and draining of all three entries.

After all routes succeed, `smp_init()` calls `pic_disable()` and records the I/O APIC handoff. A safe rejection retains PIC delivery. An unsafe initialization or rollback result halts boot, preventing an uncertain I/O APIC route from operating alongside the PIC. This policy is in `c/kernel/cpu/smp/smp.c`.

PCI enumeration and network initialization precede `smp_init()`. After APIC routing setup and before AP startup, `netdev_irq_route()` registers NIC interrupts. The earlier page described an INTx preference for every NIC. The current policy tries MSI followed by INTx for NICs with `msi_ok`, and INTx for the others, then restores the previous global preference. The RTL8169 driver sets `msi_ok` in `c/drivers/net/rtl8169.c`.

The general `dev_probe_all()` pass follows `smp_init()`. Then `ahci_irq_attach()` attempts MSI attachment for disk-bearing AHCI controllers. Its command completion probe checks interrupt delivery and selects polling after a failed probe. The implementation is in `c/drivers/block/ahci.c`.

## I/O APIC route transactions

`ioapic_init()` maps discovered controllers with uncached access, reads their redirection table capacities and masks each accepted controller’s entries. It verifies each mask before clearing the destination field. The registry in `c/kernel/cpu/irq/ioapic_map.c` checks register addresses, GSI range overlap, arithmetic overflow and redirection capacities against the register selector range.

Runtime routing selects the controller containing the requested GSI. Controller selection and register selector/data accesses share a lock, so concurrent CPUs address the intended controller and register. The lock saves and restores local interrupt state. See `c/kernel/cpu/irq/ioapic.c` and `c/drivers/core/io_lock.h`.

`ioapic_route()` updates a route in four stages:

1. Mask the old entry, verify the mask and drain its previous delivery.
2. Write the destination and route fields while retaining the mask.
3. Read back and verify the writable fields.
4. Enable the entry and verify the enabled route.

Keeping the entry masked during programming prevents a failed destination write from exposing a stale target. Readback compares writable fields while allowing Delivery Status and Remote IRR to change as hardware processes interrupts.

The interface in `c/kernel/cpu/irq/ioapic.h` distinguishes these outcomes:

| Result | Meaning |
|---|---|
| `IOAPIC_ROUTE_OK` | The operation succeeded. |
| `IOAPIC_ROUTE_SAFE_REJECT` | Validation rejected the request before programming, or recovery confirmed a masked entry. |
| `IOAPIC_ROUTE_UNSAFE` | Masking could not be confirmed. |

`ioapic_mask()` verifies the mask, then waits for Delivery Status and, for a level route, Remote IRR to clear. Each observation takes a short controller lock. The wait has a one millisecond budget when the TSC is calibrated and a ceiling of 100000 observations. The old vector and handler remain installed during this wait so an outstanding interrupt can complete and send EOI.

After an expired wait, the code attempts controller acknowledgement while retaining the mask and old vector. Controllers with version `0x20` or later receive a direct EOI write at MMIO offset `0x40`; older controllers receive a masked transition from level to edge and back. The vector stays in place because controller EOI matches the current redirection entry’s vector. Another drain check follows the acknowledgement.

PCI teardown retains the route and vector when masking or draining fails. CPU acceptance is checked through the vector retirement protocol described below.

## Local APIC timer

`lapic_timer_init()` selects a divisor of 16, programs periodic mode and writes the supplied initial count. The caller supplies the vector. The implementation is in `c/kernel/cpu/irq/lapic.c`.

The earlier page described AP timer reloads as uncalibrated. `smp_init()` now calls `lapic_timer_calibrate()` on the BSP before starting APs. Calibration measures a masked countdown against the calibrated TSC in nominal 10 ms windows. Timestamps bracket both the initial count write and current count read; endpoint brackets exceeding 10 µs cause sample rejection.

The estimator seeks five accepted samples within fifteen attempts and selects the middle sorted sample after accepting at least three. Bracketing and repeated samples reduce bias from delayed register accesses, including delays caused by system management interrupts.

During AP startup, `ap_entry()` arms vector 32 before parking the CPU. The timer wakes the AP to check scheduler readiness and later supplies preemption interrupts. `lapic_timer_count_ns()` converts a requested period of 1.6 ms plus 0.2 ms per CPU index into a reload count.

When calibration is unavailable, the AP uses the earlier count expression:

```c
LAPIC_AP_TIMER_COUNT + (uint32_t)idx * (LAPIC_AP_TIMER_COUNT / 8)
```

`LAPIC_AP_TIMER_COUNT` is 100000. Different periods spread timer arrivals across CPUs to reduce scheduler lock convoys and correlated observations, as explained in `c/kernel/cpu/smp/smp.c`.

The BSP’s scheduling tick comes from the PIT at vector 32. Its interrupt path calls `timer_tick()` and expires scheduled sleeps. On every CPU, vector 32 acknowledges the interrupt and calls `schedule()` when the saved kernel nesting depth is zero. This dispatch is in `c/kernel/cpu/irq/interrupts.c`.

The profiler uses the BSP local APIC timer for sampling and sends sampling IPIs to APs. Its arming path executes on the BSP, preserving the AP scheduling timers. See `c/kernel/diag/kprof.c`.

## Interprocessor interrupts

`lapic_send_ipi()` sends a fixed interrupt to one physical APIC ID. `apic_model_icr()` encodes the destination for the selected backend.

In xAPIC mode, submission waits for Delivery Status to clear, writes the destination half and command half, then waits again. The bounded wait reports a timeout. In x2APIC mode, a single 64 bit write to MSR `0x830` submits the command. Both paths are in `c/kernel/cpu/irq/lapic.c`.

The main static IPI vectors are:

| Vector | Purpose | Implementation |
|---|---|---|
| 240 | TLB shootdown | `c/kernel/cpu/tlb.c` |
| 241 | Parallel framebuffer presentation | `c/kernel/cpu/smp/smp.c` |
| 243 | Wake idle CPUs and prompt vector retirement checks | `c/kernel/cpu/smp/smp.c`, `c/drivers/core/irq.c` |

`c/kernel/cpu/irq/idt.c` installs these gates. `c/kernel/cpu/irq/interrupts.c` dispatches them and sends local APIC EOI. The profiler and diagnostics install further gates through `idt_install_gate()`.

IPI users implement completion protocols around the interrupt delivery. TLB shootdown publishes a request for each target CPU before sending IPIs and waits for acknowledgements after flushing. `tlb_service()` also consumes requests from polling paths, allowing a CPU spinning with interrupts disabled to acknowledge a shootdown. An atomic claim counts each request once. An acknowledgement timeout enters a fatal stop path in `c/kernel/cpu/tlb.c`.

Parallel framebuffer presentation tracks completion of vector 241 work. After a bounded wait, the initiating CPU claims pending bands for a fallback copy and waits for bands already being copied. The policy in `c/kernel/cpu/smp/present_policy.c` disables parallel presentation after four consecutive timeouts or at least eight timeouts among the last 64 presents once the window is full.

AP startup uses `lapic_start_ap()` to send INIT followed by two startup IPIs. It waits 10 ms after INIT and 200 µs after each startup IPI using `time_mono_raw_ns()`. Elapsed time supplies the startup intervals across processor and emulator speeds; an unavailable or stalled clock aborts the sequence.

## PCI interrupts and MSI

Drivers request interrupts through `dev_irq_request()`. Its default preference is MSI-X, then MSI, then INTx. `dev_irq_prefer()` sets a global ceiling on that preference. Both operations are in `c/kernel/pci/pci_msi.c`.

MSI-X setup programs table entry zero; MSI setup enables one message. Both target the requesting CPU’s local APIC ID with message address `0xFEE00000 | (apic_id << 12)` and message data containing the allocated vector, using fixed delivery and edge triggering.

Before selecting a delivery mechanism, request setup disables and verifies existing MSI and MSI-X sources. MSI setup verifies address, data, supported masking controls and enable state. MSI-X setup masks its table entry while writing and checking the target. Failure to establish a quiet message source enters a halt path, preserving callback and vector ownership.

INTx uses the I/O APIC. Route selection follows this priority:

| Priority | Routing source |
|---|---|
| 1 | Evaluated ACPI `_PRT`, including interrupt link devices |
| 2 | Supported Intel PCH routing registers, mapping PIRQA through PIRQH to GSIs 16 through 23 |
| 3 | PCI firmware interrupt line, with MADT source overrides applied to values below 16 |

The earlier page described firmware-line routing alone. Current routing evaluates `_PRT` and follows PCI bridges, swizzling the pin using the child slot at each upstream step. When `_PRT` and PCH routing registers disagree, the log records both GSIs and selects `_PRT`.

The AML integration requests `_PIC(1)` when `smp_irq_via_apic()` reports a committed I/O APIC handoff. The initialization hook runs after namespace loading and before device initialization. This integration spans `c/kernel/pci/pci_prt_aml.c`, `c/kernel/init/power.c` and `c/drivers/power/acpi/devices.c`.

A direct `_PRT` entry supplies a GSI. A link entry selects a descriptor from the complete `_CRS` resource template. ISA IRQ descriptors pass through MADT overrides; Extended IRQ descriptors supply GSIs. Link allocation chooses a usable IRQ from `_PRS`, constructs a complete resource configuration and programs `_SRS`. It preserves resources used by cached link consumers and checks the resulting IRQ, electrical attributes and enabled status.

After AML initialization, `pci_prt_aml_ready()` publishes the resolver and invokes `pci_intx_reroute_all()` from the ACPI worker. A changed route passes through source suppression, callback draining and rebinding. Successful moves invoke the driver callback to collect pending status. A failed bind attempts to restore the saved route; quarantine leaves the device requiring polling. Busy bindings receive bounded retries with worker sleeps.

Devices sharing a GSI share a vector and dispatcher. Each registered handler inspects and acknowledges its device’s status. Direct `_PRT` routes and fallback routes request active low polarity and level triggering; link resources supply their own attributes. Shared routes retain the highest priority accepted electrical description and reject conflicting peer requests at equal priority. `c/kernel/pci/pci_platform.h` applies an edge trigger workaround for the TCG CPUID vendor, based on the interrupt storm recorded in its source comment.

## EOI and vector retirement

Acknowledgement follows the delivery mechanism:

| Interrupt path | Acknowledgement |
|---|---|
| BSP legacy IRQ with PIC routing | PIC EOI |
| Legacy IRQ through an I/O APIC | Local APIC EOI |
| AP timer at vector 32 | Local APIC EOI |
| Dynamic device vector with a ready local APIC | Local APIC EOI |
| Static IPIs at vectors 240, 241 and 243 | Local APIC EOI |
| Local APIC spurious vector 255 | Return from the handler |

`apic_model_legacy_irq_uses_lapic()` accounts for AP timers while the BSP retains PIC routing. Dynamic device dispatch selects local APIC acknowledgement through `lapic_ready()`, because MSI reaches the local APIC under either legacy routing policy. See `c/kernel/cpu/irq/interrupts.c` and `c/drivers/core/irq.c`.

The dynamic allocator owns vectors `0x60` through `0x7F` and installs their IDT gates. PCI release suppresses the device source before retiring its callback. Removing the last INTx member also masks and drains the GSI.

After active dispatch references drain, `irq_free_vector()` quarantines the vector until every online CPU has checked its own local APIC IRR and ISR bits through `lapic_vector_pending()`. Retirement sends vector 243 to other CPUs to prompt cleanup, including a BSP whose PIT still uses PIC acknowledgement.

`lapic_eoi()` runs the cleanup hook before writing EOI, keeping an in-service interrupt visible to the pending check. Dynamic dispatch holds its vector reference through EOI, releases that reference, then retries local cleanup. If retirement remains pending, it sends another cleanup kick to other CPUs. These steps prevent reuse while an old delivery is queued or completing.

## Diagnostics and test coverage

With `make HWHUD=1`, the hardware panel displays shared INTx lines, redirection entries and PCI assertion status, together with the measured local APIC timer rate and parallel presentation statistics. The build switch is in `Makefile`; the panel is in `c/kernel/diag/hwhud.c`.

`tests/x2apic.mk` defines `make test-x2apic-host` with a negative control prerequisite. The calibration scenarios in `tests/unit/x2apic_run.py` include delayed endpoints, insufficient accepted samples and median selection. The `LAPIC_NEGCTL_UNBRACKETED` control requires the delayed endpoint case to fail.

`tests/boot/run-x2apic.py` defines BIOS and UEFI scenarios with an AP at APIC ID 256. The associated test build forces x2APIC mode to exercise the MSR backend.

## Limitations

- Calibration requires a calibrated TSC and at least three accepted samples. APs otherwise use fixed reload counts whose periods depend on the timer input rate.
- Earlier documentation stated that discovery retained one I/O APIC. The current capacity is eight. AML PCI routing supports segment zero, retains up to sixteen root bridges and bounds bridge traversal at 32 steps. Link allocation rejects disabled links and configurations requiring further resource allocation.
- Interrupt remapping is unconfigured. I/O APIC and MSI destinations accept APIC IDs through `0xFE`; x2APIC IPIs accept wider unicast IDs, reserving `0xFFFFFFFF`. These bounds are documented in `c/kernel/cpu/irq/apic_model.h` and implemented in `c/kernel/cpu/irq/apic_model.c` and `c/kernel/pci/pci_msi.c`.
- Source comments identify timer calibration, multiple I/O APIC routing, AML PCI routing and vector retirement as model or QEMU evidence, with X79-VG4 hardware validation pending. The synthetic x2APIC fixture also covers emulation.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/cpu/irq/lapic.c` | Register backends, initialization, virtual wire setup, timer calibration, EOI and IPIs |
| `c/kernel/cpu/irq/lapic.h` | Local APIC interface |
| `c/kernel/cpu/irq/apic_model.c` | Mode selection, CPU identity rules, destination encoding and EOI policy |
| `c/kernel/cpu/irq/apic_model.h` | Shared APIC contracts and destination limits |
| `c/kernel/cpu/irq/ioapic.c` | Controller initialization, route programming, masking and delivery draining |
| `c/kernel/cpu/irq/ioapic.h` | Routing interface and result codes |
| `c/kernel/cpu/irq/ioapic_map.c` | Controller registry and GSI range lookup |
| `c/kernel/cpu/acpi/acpi.c` | MADT parsing and ISA source overrides |
| `c/kernel/cpu/acpi/madt_irq.c` | I/O APIC record validation and deduplication |
| `c/kernel/cpu/acpi/madt_irq.h` | Discovery records and controller capacity |
| `c/kernel/cpu/smp/smp.c` | Boot routing policy, AP startup, timers and presentation IPIs |
| `c/kernel/cpu/smp/present_policy.c` | Parallel presentation timeout policy |
| `c/kernel/cpu/smp/present_policy.h` | Presentation policy thresholds |
| `c/kernel/cpu/irq/idt.c` | Static gates and subsystem gate installation |
| `c/kernel/cpu/irq/interrupts.c` | Timer and static IPI dispatch, acknowledgement selection |
| `c/kernel/cpu/tlb.c` | Shootdown publication, polling and acknowledgements |
| `c/kernel/init/kmain.c` | Controller and driver initialization order |
| `c/kernel/init/power.c` | AML initialization hook and rerouting worker |
| `c/drivers/power/acpi/devices.c` | Namespace initialization order |
| `c/drivers/timer/pic.c` | PIC remapping, masking and EOI |
| `c/drivers/timer/pit.c` | PIT programming and BSP tick accounting |
| `c/drivers/timer/hpet.c` | HPET setup and firmware interrupt routing cleanup |
| `c/drivers/core/io_lock.h` | Controller locking and interrupt state handling |
| `c/drivers/core/irq.c` | Dynamic vectors, dispatch references, quarantine and cleanup |
| `c/drivers/net/netdev.c` | NIC interrupt registration and delivery preference |
| `c/drivers/net/rtl8169.c` | NIC MSI eligibility |
| `c/drivers/block/ahci.c` | Late MSI attachment and delivery probe |
| `c/kernel/pci/pci_msi.c` | MSI-X, MSI, shared INTx management and rerouting |
| `c/kernel/pci/pci_prt_aml.c` | Interrupt model selection, bridge traversal and link resources |
| `c/kernel/pci/pci_platform.h` | INTx trigger selection |
| `c/kernel/diag/kprof.c` | BSP sampling timer and sampling IPIs |
| `c/kernel/diag/kdiag.c` | Diagnostic interrupt gates |
| `c/kernel/diag/hwhud.c` | Hardware diagnostics panel |
| `Makefile` | Hardware panel build switch |
| `tests/x2apic.mk` | APIC test targets and control prerequisites |
| `tests/unit/x2apic_run.py` | APIC controls and timer calibration scenarios |
| `tests/boot/run-x2apic.py` | Synthetic BIOS and UEFI x2APIC scenarios |

## See also

- [ACPI](acpi.md)
- [SMP boot](smp-boot.md)
- [PCI](pci.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Device and driver model](../architecture/device-model.md)