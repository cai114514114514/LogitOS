# Timer hardware

LogitOS uses PIT interrupts to advance the shared tick counter, TSC or HPET counters to read elapsed time at finer resolution, and the CMOS RTC to establish calendar time. The PIC supplies early legacy interrupt delivery, followed by an I/O APIC handoff when routing succeeds. Hardware access lives in `c/drivers/timer/`; clock selection, calibration and conversion to nanoseconds live in `c/kernel/core/ktime.c`.

## Hardware roles

| Hardware | Interface | Kernel use |
|---|---|---|
| PIT channel 0 | Data port `0x40`, command port `0x43` | Periodic IRQ0 and shared ticks |
| PIT channel 2 | Ports `0x42`, `0x43` and `0x61` | Reference interval for TSC calibration |
| TSC | CPU timestamp counter | Preferred clocksource after feature checks and calibration |
| PIC | Master ports `0x20`/`0x21`, slave ports `0xA0`/`0xA1` | Legacy interrupt routing, masking and acknowledgement |
| HPET | ACPI-described MMIO registers | Elapsed-time counter and reference for TSC calibration |
| LAPIC timer | MMIO registers or x2APIC MSRs | Periodic scheduling interrupts on application processors |
| RTC | CMOS index/data ports `0x70`/`0x71` | Polled calendar readings and clock cross-checks |

The PIT and LAPIC timers supply interrupt events. Clocksource selection determines how elapsed time is measured between events. RTC readings establish the wall-clock offset and provide another oscillator for diagnostics. These roles are implemented in `c/drivers/timer/`, `c/kernel/cpu/irq/lapic.c` and `c/kernel/core/ktime.c`.

## Clock selection during boot

In `c/kernel/init/kmain.c`, `kernel_main()` initialises the interrupt tables, calls `pic_remap()` and programs channel 0 through `pit_init(TIMER_HZ)`. The PIT driver calls `time_init()`, which attempts TSC calibration against channel 2.

`tsc_usable()` in `c/kernel/core/ktime.c` requires a TSC and either the invariant TSC feature or the CPUID hypervisor flag. `calibrate_tsc()` accepts a median frequency between 10 MHz and 100 GHz. Successful calibration selects TSC; otherwise, initial timekeeping uses the PIT tick counter.

After `pmm_init()`, the boot path calls `lapic_virtual_wire()`. When LAPIC initialisation succeeds, it configures LINT0 for ExtINT delivery, LINT1 for NMI delivery and task priority zero. This supplies the PIC route through the bootstrap processor’s LAPIC.

The boot path then supplies firmware information through `acpi_set_mb2_info()` and calls `time_platform_init()`. The resulting clocksource policy is:

| Available clocks | Selection |
|---|---|
| TSC passed feature checks and channel 2 calibration | Keep TSC; register a usable HPET as standby |
| TSC passed feature checks, channel 2 calibration failed, and HPET calibration succeeds | Select TSC calibrated against HPET |
| TSC remains unavailable and HPET initialisation succeeds | Select HPET |
| TSC remains unavailable and HPET initialisation fails | Keep PIT |

The late stage allows ACPI and MMIO discovery to use the memory mapping facilities established during physical memory initialisation. It also gives TSC calibration a second reference when channel 2 fails. The policy is implemented in `c/kernel/core/ktime.c`.

`time_set_source()` folds elapsed cycles from the old source into the nanosecond accumulator, then publishes the new source and counter baseline under a sequence counter. `time_mono_ns()` applies an atomic maximum across readings from different CPUs to suppress backward steps.

## PIT: periodic ticks and calibration

`TIMER_HZ` is `100` in `c/drivers/timer/pit.h`. `pit_init()` divides the 1,193,182 Hz oscillator frequency by the requested rate and writes the divisor to channel 0. Command byte `0x34` selects mode 2, the rate generator. Integer division makes 100 Hz the nominal system rate.

The interfaces in `c/drivers/timer/pit.c` expose this tick model:

| Function | Result |
|---|---|
| `timer_ticks()` | Timer interrupts counted on the bootstrap processor |
| `timer_ms()` | Tick count converted to milliseconds, in nominal 10 ms steps |
| `timer_ns_per_tick()` | Nominal duration of a tick in nanoseconds |

In `c/kernel/cpu/irq/interrupts.c`, vector 32 on CPU index 0 calls `timer_tick()` and expires timed sleeps near the start of interrupt handling. `timer_tick()` increments the shared counter and calls `time_tick()`. The latter folds clocksource cycles into the nanosecond accumulator, samples CPU accounting and runs due software timers.

When TSC or HPET supplies the clocksource, clock reads include counter movement since the last tick. Software timer callbacks run from the periodic tick. This gives clock readings finer resolution while retaining periodic callback delivery.

TSC calibration uses channel 2 so channel 0 keeps its configuration for system ticks. `calibrate_tsc_once()` in `c/kernel/core/ktime.c` programs mode 0 with `0x7FFF` oscillator counts, controls the gate through port `0x61`, and polls the output while measuring TSC cycles. Each polling loop has an iteration limit. `calibrate_tsc()` takes three measurements and chooses their median to reduce the influence of a delayed sample.

The later `calibrate_tsc_hpet()` takes three measurements spanning about 20 ms each. HPET reads bracket each TSC endpoint; brackets wider than about 10 µs cause retries. Each sample has up to eight attempts, and the counter polling loop has an iteration limit. The accepted median uses the same 10 MHz to 100 GHz range as channel 2 calibration.

## PIC and the APIC handoff

`pic_remap()` in `c/drivers/timer/pic.c` maps master IRQs to vectors 32–39 and slave IRQs to vectors 40–47. Its masks enable IRQ0 for the PIT, IRQ1 for the keyboard, IRQ2 for the slave cascade and IRQ12 for the mouse. `pic_eoi()` acknowledges the slave first for IRQs 8–15, then acknowledges the master.

Later, `smp_init()` attempts to route the PIT, keyboard and mouse through the I/O APIC to the bootstrap processor. `ioapic_route_legacy_set()` applies ACPI interrupt source overrides and validates route programming. Successful routing leads to `pic_disable()` and publication of the APIC routing state. A safe rejection retains PIC delivery. The boot path halts if initial masking or route rollback reports an unsafe state.

ACPI discovery records multiple I/O APICs. Routing selects the controller whose global system interrupt range covers the requested interrupt, using `c/kernel/cpu/irq/ioapic_map.c`.

Application processors receive vector 32 from their LAPIC timers. The acknowledgement policy in `c/kernel/cpu/irq/apic_model.c` selects LAPIC acknowledgement for those timers, including configurations where bootstrap processor device interrupts retain PIC delivery. CPU index 0 advances the shared tick counter.

Before starting application processors, `smp_init()` calls `lapic_timer_calibrate()`. It measures a masked LAPIC countdown with divisor 16 against the calibrated TSC over intervals of about 10 ms. Fenced TSC reads bracket the timer register accesses. Endpoint uncertainty above 10 µs causes rejection. The routine seeks five valid samples within 15 attempts, requires at least three, and selects the middle entry of the sorted samples. Accepted divided frequencies range from 1 MHz to 2 GHz.

Application processor setup converts a requested period of 1.6 ms plus 0.2 ms per CPU index through `lapic_timer_count_ns()`. Staggering the periods spreads scheduler lock acquisition across CPUs. The earlier description of AP timers as fixed, uncalibrated reloads applies to the fallback under Limitations; the primary path in `c/kernel/cpu/smp/smp.c` now uses calibrated reloads.

## HPET: discovery and firmware handover

`hpet_init()` obtains the HPET table through `acpi_find_table()`. The ACPI walker bounds table lengths and verifies checksums before returning a matching table. The HPET parser checks the minimum table size, memory address format, register alignment and address arithmetic. See `c/kernel/cpu/acpi/acpi.c`, `c/kernel/cpu/acpi/acpi_integrity.c` and `c/drivers/timer/hpet.c`.

Before register access, the driver checks the proposed MMIO pages against the physical allocator’s RAM classification and existing mappings. It requests writable supervisor mappings with caching disabled, then verifies each page’s physical address and supervisor access. These checks reject firmware addresses that overlap RAM or conflict with existing mappings.

`hpet_parse_caps()` checks agreement between the hardware capability identifier and the firmware description, validates the revision and period, and records the counter width. It accepts 32-bit and 64-bit counters with derived frequencies between 10 MHz and 10 GHz.

The previous version described rejection when firmware left a comparator armed. The current driver clears each armed comparator’s interrupt-enable bit and clears legacy replacement routing. The handover comment in `c/drivers/timer/hpet.c` records why: rejecting the device had left firmware’s route active, diverting IRQ0 from the PIT. Clearing that route restores PIT and RTC ownership of IRQ0 and IRQ8.

The driver then enables the main counter with legacy replacement routing cleared and polls for counter movement. If the counter stays stationary, it restores the general configuration saved after comparator disarming and route cleanup.

`hpet_read()` masks counter readings to the advertised width. `c/kernel/core/ktime.c` converts those cycles into nanoseconds and folds them into accumulated time during PIT ticks.

## RTC: calendar time and cross-checks

`rtc_now()` in `c/drivers/timer/rtc.c` reads calendar fields through the CMOS index/data ports. A lock protects the indexed register sequence so concurrent readers access the intended registers. Each wait for the update-in-progress flag has an iteration limit. The function reads the calendar and repeats the seconds, minutes and hours until those fields agree. Register B supplies the binary/BCD and hour-format settings.

`rtc_unix()` converts the decoded calendar into whole seconds since the Unix epoch using Gregorian leap-year rules. During time initialisation, `time_wall_sync()` uses that value to establish an offset from monotonic time. `time_real_ns()` adds the offset to each monotonic reading, giving subsequent wall-clock readings the active clocksource’s granularity. See `c/kernel/core/ktime.c`.

For timer callbacks, `rtc_second()` tries the CMOS lock and returns its cached second when the lock is busy, an update is in progress or the decoded second is invalid. This keeps clock diagnostics from waiting for another CMOS reader.

The boot cross-check measures an interval spanning five observed RTC second transitions. It reports elapsed monotonic time, elapsed tick time, timing error, estimated lost ticks and the worst tick gap. A diagnostic negative control feeds doubled elapsed time through the same acceptance function. Both paths are in `c/kernel/core/ktime.c`.

## Diagnostics and test coverage

`time_report()` prints the selected source, frequency, reported resolution and TSC policy flags. Its calibration label distinguishes `pit-ch2`, `hpet` and `none`. When printed, the calibration sample line contains the saved channel 2 measurements, including after successful HPET calibration.

The early PIT probe in `kernel_main()` runs after `time_platform_init()`, placing HPET route cleanup before the observation. It enables interrupts and reports delivered ticks over a target interval of 50 ms, with about five ticks expected. A ceiling of 200 million iterations bounds the probe.

The `HWHUD=1` build option in `Makefile` enables a desktop panel that displays the selected clocksource, observed PIT ticks per second and measured LAPIC timer frequency. The panel is implemented in `c/kernel/diag/hwhud.c`. The `TIMENOCH2=1` option forces channel 2 calibration samples to fail, providing a way to exercise later HPET calibration.

Boot diagnostics also switch temporarily to PIT and restore the saved source, measure software timer lateness, and sample monotonic readings across CPUs. These probes are implemented in `c/kernel/core/ktime.c`.

| Test source | Coverage expressed by its assertions |
|---|---|
| `tests/unit/hpet_test.c` | Table parsing, capabilities, firmware/hardware agreement, helper that disarms comparators and helper that clears legacy routes |
| `tests/unit/time_test.c` | Counter conversion, wrap handling, monotonic clamping, source switching and software timers |
| `tests/unit/x2apic_test.c` | Timer acknowledgement policy and LAPIC calibration with delayed endpoints, sample thresholds, median selection and bounded retries |
| `tests/boot/run-hpet-test.py` | QEMU HPET activation, RTC cross-check, restoration after a PIT switch and monotonic readings observed across four CPUs |

The HPET guest harness hides the hypervisor flag and uses a CPU configuration without invariant TSC to exercise HPET selection. In `tests/hpet.mk`, the guest test with HPET absent is a prerequisite of the positive guest target. The host target requires negative controls covering address space, block identifier and revision validation.

`tests/x2apic.mk` makes `test-x2apic-negctl` a prerequisite of `test-x2apic-host`. Through `tests/unit/x2apic_run.py`, the control removes LAPIC endpoint bracket checks and requires an assertion failure caused by calibration bias.

## Limitations

- PIT time counts delivered interrupts. Coalesced pending interrupts can lose increments and lengthen durations measured in ticks. See `c/drivers/timer/pit.h`.
- HPET scheduling through comparator interrupts and its suspend/resume routing policy remain unimplemented. See `c/drivers/timer/hpet.c`.
- Signed counter differences require folding within half the counter’s wrap period. Long gaps matter especially for 32-bit HPET counters. See `c/kernel/core/ktime.c` and `tests/unit/time_test.c`.
- AP timers use fixed reload counts staggered by CPU index when LAPIC calibration lacks a calibrated TSC or collects fewer than three valid samples. See `c/kernel/cpu/irq/lapic.c` and `c/kernel/cpu/smp/smp.c`.
- RTC conversion assumes UTC and forms the year as 2000 plus the decoded year. Its current conversion from the 12-hour clock maps 12 AM to hour 12 and 12 PM to hour 0. The calendar agreement loop also has no overall retry limit. See `c/drivers/timer/rtc.c` and `c/drivers/timer/rtc.h`.
- QEMU coverage establishes the emulated ACPI/MMIO path. Physical firmware handover and timer behaviour require device-specific evidence. See `tests/boot/run-hpet-test.py` and `c/kernel/cpu/irq/lapic.c`.

## See also

- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Local APIC and I/O APIC](../kernel/apic.md)
- [ACPI](../kernel/acpi.md)
- [Time, randomness and settings](../kernel/time-rng-settings.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/timer/pit.c` | Channel 0 programming, shared ticks and clock initialisation |
| `c/drivers/timer/pit.h` | Tick rate, timing interfaces and an explanation of lost ticks |
| `c/drivers/timer/pic.c` | PIC remapping, masks and acknowledgement |
| `c/drivers/timer/hpet.c` | HPET validation, mapping, firmware cleanup and counter access |
| `c/drivers/timer/hpet.h` | HPET descriptions, capabilities and interfaces |
| `c/drivers/timer/rtc.c` | CMOS locking, calendar decoding and epoch conversion |
| `c/drivers/timer/rtc.h` | Calendar structure and RTC interfaces |
| `c/kernel/core/ktime.c` | Clock selection, calibration, conversion, software timers and diagnostics |
| `c/kernel/init/kmain.c` | Timer boot order, virtual-wire setup and PIT probe |
| `c/kernel/cpu/acpi/acpi.c` | Firmware table discovery and interrupt source information |
| `c/kernel/cpu/acpi/acpi_integrity.c` | ACPI length, signature and checksum validation |
| `c/kernel/cpu/irq/interrupts.c` | Bootstrap ticks, scheduling and interrupt acknowledgement |
| `c/kernel/cpu/irq/apic_model.c` | PIC/LAPIC acknowledgement policy |
| `c/kernel/cpu/irq/ioapic.c` | Legacy routing, readback validation and rollback |
| `c/kernel/cpu/irq/ioapic_map.c` | I/O APIC selection by interrupt range |
| `c/kernel/cpu/irq/lapic.c` | Virtual-wire setup and LAPIC timer calibration |
| `c/kernel/cpu/smp/smp.c` | PIC handoff and application processor timer setup |
| `c/kernel/diag/hwhud.c` | Desktop timer diagnostics |
| `Makefile` | Hardware panel and channel 2 calibration build options |
| `tests/unit/hpet_test.c` | Assertions for the HPET parser, capabilities and cleanup helpers |
| `tests/unit/time_test.c` | Clock arithmetic, source switching and software timer assertions |
| `tests/unit/x2apic_test.c` | APIC acknowledgement and LAPIC calibration assertions |
| `tests/unit/x2apic_run.py` | LAPIC calibration scenarios and negative control |
| `tests/boot/run-hpet-test.py` | QEMU HPET presence and clock diagnostics |
| `tests/hpet.mk` | HPET targets and prerequisites for the negative controls |
| `tests/x2apic.mk` | APIC targets and prerequisites for the negative controls |
