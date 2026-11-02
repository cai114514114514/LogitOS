# Interrupts and time

LogitOS dispatches CPU exceptions, device interrupts and interprocessor requests through x86-64 interrupt vectors. The bootstrap processor maintains a nominal 100 Hz tick, while application processors use local APIC timers for scheduling. A timekeeper converts TSC, HPET or delivered tick counts into nanoseconds and provides monotonic clocks, wall time and kernel timer deadlines.

## Entry and return

`idt_init()` installs gates in a shared 256-entry Interrupt Descriptor Table. Application processors load the same table through `idt_load()`. Hardware interrupt gates have privilege level 0; vector 128 has privilege level 3 so user programs can invoke `int 0x80` (`c/kernel/cpu/irq/idt.c`).

The static stubs in `c/boot/isr.asm` push a vector number and reserve a slot for an error code. Stubs for vectors 8, 10–14 and 17 keep the CPU error code; the other static stubs supply zero. Their common entry path:

1. Saves general-purpose registers in the layout defined by `struct registers`.
2. Saves floating-point and SSE state with FXSAVE into a 512-byte area aligned to 16 bytes.
3. Clears the direction flag and calls `interrupt_handler()` with the register frame and FXSAVE address.
4. Restores floating-point state and registers, discards the vector number and error code, and returns through `iretq`.

The saved state resides on the current kernel stack and survives a context switch during handling. Passing the FXSAVE address to C lets signal delivery copy the interrupted program’s state and lets signal return replace the state restored by the assembly epilogue (`c/boot/isr.asm`, `c/kernel/cpu/irq/interrupts.h`).

Entry tracks nesting depth per CPU. Shared objects use their own locks. The dispatcher saves the previous depth on its stack and restores it on the CPU where execution resumes, accounting for migration during a blocking operation or scheduling (`c/kernel/cpu/irq/interrupts.c`).

## Vectors and exceptions

Selected assignments are:

| Vector | Use |
|---|---|
| 0–31 | CPU exceptions |
| 18 | Machine check through a dedicated entry |
| 32 | PIT on the bootstrap processor; local APIC timer on application processors |
| 33, 44 | PS/2 keyboard and mouse |
| 65 | Static e1000 handler |
| 96–127 | Device-model IRQ callbacks |
| 128 | `int 0x80` system calls |
| 240 | TLB shootdown IPI |
| 241 | Parallel framebuffer presentation IPI |
| 243 | Compositor wakeup and IRQ vector cleanup IPI |
| 255 | Local APIC spurious interrupt |

Static gates and dispatch branches live in `c/kernel/cpu/irq/idt.c` and `c/kernel/cpu/irq/interrupts.c`. Dynamic device vectors belong to `c/drivers/core/irq.c`.

Vector 18 enters `mce_entry()`, which saves general-purpose and floating-point state before calling `mce_handle()`. Machine-check diagnostics use raw polled serial output because the interrupted code may hold a logging lock. The handler resumes after corrected bank records when the global status marks the saved instruction pointer valid. Uncorrected errors, overflow, corrupt context and an unknown cause lead to panic (`c/kernel/cpu/power/mce.c`).

For vector 14, the common dispatcher first calls `mm_fault()`. Successful resolution, including demand paging or copy-on-write handling, returns through the saved frame and retries the instruction (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/mm/virt/fault.c`).

An unresolved exception from ring 3 passes through `fault_signal()`. For supported vectors, `ksig_fault()` records the fault for the current thread when a suitable unblocked handler is available and a slot in the fault record is free. Otherwise, the dispatcher attempts a core dump and terminates the process with status 139. An unresolved kernel exception prints diagnostic state, calls `serial_flush_blocking()`, and enters a loop that disables interrupts and halts the CPU (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/exec/signal/ksignal.c`, `c/drivers/char/serial.c`).

At the common exit, an outer entry drains pending softirqs. An outer return to ring 3 also checks for signal delivery while the saved register and floating-point frames remain available (`c/kernel/cpu/irq/interrupts.c`).

## IRQ routing and acknowledgement

Early boot remaps the two 8259 PICs to vectors 32–47. Their initial masks enable IRQ0 for the timer, IRQ1 for the keyboard, IRQ2 for the slave cascade and IRQ12 for the mouse (`c/drivers/timer/pic.c`).

After physical memory initialization, `lapic_virtual_wire()` initializes the local APIC and configures the bootstrap processor’s LINT0 for ExtINT delivery, LINT1 for NMI and task priority to admit all vectors. This establishes the PIC delivery path using kernel settings (`c/kernel/init/kmain.c`, `c/kernel/cpu/irq/lapic.c`).

The local APIC backend follows the firmware access mode: xAPIC uses MMIO registers and x2APIC uses MSRs. During `smp_init()`, the kernel discovers I/O APICs and attempts to move the timer, keyboard and mouse routes to the bootstrap processor’s local APIC (`c/kernel/cpu/irq/lapic.c`, `c/kernel/cpu/smp/smp.c`).

I/O APIC discovery associates usable MADT controller descriptions with their GSI ranges, rejects overlapping ranges and verifies initial masking before publishing readiness. Routing selects the controller covering the requested GSI. ISA routing applies ACPI interrupt-source overrides, including polarity and trigger flags (`c/kernel/cpu/irq/ioapic.c`, `c/kernel/cpu/irq/ioapic_map.c`).

A route update masks the redirection entry, writes the destination and vector, verifies readback, then enables and verifies the route. The three legacy routes form a transaction:

- Success masks both PICs and commits I/O APIC delivery.
- Rejection with verified masking retains PIC delivery.
- Failure to establish a safe masking state stops boot.

These checks give the fallback a verified controller state (`c/kernel/cpu/irq/ioapic.c`, `c/kernel/cpu/smp/smp.c`).

End-of-interrupt acknowledgement follows the delivery path. Legacy IRQs acknowledge the PIC or local APIC according to the committed routing state. An application processor’s vector-32 timer acknowledges its local APIC even when the bootstrap processor retains PIC delivery. Vector 255 returns directly through the spurious path, without EOI (`c/kernel/cpu/irq/apic_model.c`, `c/kernel/cpu/irq/interrupts.c`).

## PCI interrupts and vector lifetime

`dev_irq_request()` tries MSI-X, MSI and shared INTx in that order, subject to device capabilities and the configured maximum mode. The dynamic pool contains `IRQ_NVEC` vectors, currently 32, beginning at `IRQ_VEC_BASE`, currently `0x60` (`c/kernel/pci/pci_msi.c`, `c/drivers/core/irq.h`).

NIC setup chooses the maximum per interface: an interface advertising `msi_ok` tries MSI before INTx; other interfaces request INTx (`c/drivers/net/netdev.c`).

Correction to the routing description on the previous page: PCI INTx lookup now prefers an evaluated ACPI `_PRT` route, then supported Intel PCH routing registers, then the interrupt line supplied by firmware. The PCH path maps PIRQA–H to GSIs 16–23. Routing logs disagreements between ACPI and PCH results (`c/kernel/pci/pci_msi.c`).

Following a successful I/O APIC handoff, ACPI initialization selects the I/O APIC interrupt model through `_PIC(1)` after namespace loading and before namespace initialization. Routing resolves direct GSIs and interrupt link devices, and swizzles pins while walking upstream bridges. After initialization, the ACPI worker revisits existing INTx bindings using evaluated routes (`c/kernel/pci/pci_prt_aml.c`, `c/kernel/init/power.c`, `c/drivers/power/acpi/devices.c`).

Each shared INTx GSI owns one vector. Its dispatcher visits registered members, allowing each device handler to inspect and acknowledge its device status. A route change disables the source and retires its old membership before binding the new route. Successful rebinding invokes the callback to collect pending status; a rejected new binding triggers an attempt to restore the saved route (`c/kernel/pci/pci_msi.c`).

Dynamic vectors have assembly stubs that preserve general-purpose and floating-point state and call `irq_isr_entry()`. Gate installation finds the shared IDT through `sidt`. A short registry lock pins the callback and argument; the callback runs with that lock released, and the pin remains through EOI (`c/drivers/core/irq.c`).

Vector retirement accounts for both controller state and CPU state. The last INTx member masks its GSI and waits for delivery status and Remote IRR to drain. `irq_free_vector()` drains active dispatches and quarantines the vector until every online CPU has checked its local APIC IRR and ISR. Vector-243 IPIs prompt remote checks; EOI and dispatch completion provide cleanup opportunities. This protects a replacement callback from a delivery accepted under the old route (`c/kernel/pci/pci_msi.c`, `c/kernel/cpu/irq/ioapic.c`, `c/drivers/core/irq.c`, `c/kernel/cpu/irq/lapic.c`).

## Deferred work

Deferred work uses these execution contexts:

| Facility | Execution context |
|---|---|
| Softirqs and tasklets | The common dispatcher’s outer exit |
| Workqueue | The `kworker` thread, where callbacks can block |

Softirq draining makes up to eight passes through the current CPU’s pending bits. Each tasklet action processes up to 64 callbacks and retains further work for another pass. These bounds limit work performed while returning from an interrupt (`c/kernel/sync/work.c`).

Dynamic device entries return through their own epilogue. Softirqs raised by their callbacks remain pending for a later outer entry through the common dispatcher (`c/drivers/core/irq.c`, `c/kernel/cpu/irq/interrupts.c`).

## The periodic tick

`kernel_main()` initializes the IDT, GDT, PIC and PIT early in boot. `pit_init(TIMER_HZ)` programs PIT channel 0 as a mode-2 rate generator and calls `time_init()`. `TIMER_HZ` is 100, giving a nominal 10 ms tick interval (`c/kernel/init/kmain.c`, `c/drivers/timer/pit.c`, `c/drivers/timer/pit.h`).

A vector-32 entry on the bootstrap processor:

1. `timer_tick()` increments the count of delivered interrupts and calls `time_tick()`.
2. `time_tick()` folds elapsed source cycles into nanoseconds, samples CPU accounting and runs due kernel timers.
3. `sched_timer_expire()` makes threads with expired tick deadlines runnable.
4. The dispatcher services queued serial output and acknowledges the controller.
5. An outer entry performs timer work related to signals, checks the running thread’s CPU limit and calls `schedule()`.

Tick advancement and scheduler deadline expiry also run during nested timer entries. Entry depth gates preemption, allowing deadlines to progress while a kernel operation is interrupted (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/core/ktime.c`, `c/kernel/sched/sched.c`).

Every CPU’s vector-32 entry calls `serial_tx_tick()` before controller acknowledgement. It tries to acquire the transmit lock and sends a bounded burst of queued output: up to 16 bytes with a working FIFO, or one byte on the fallback path (`c/drivers/char/serial.c`).

Each application processor arms a periodic local APIC timer on vector 32. Its target period is 1.6 ms plus 0.2 ms times the CPU index. Staggering the periods reduces repeated simultaneous contention on the scheduler lock (`c/kernel/cpu/smp/smp.c`).

The previous page described AP reload counts as uncalibrated. That description now applies to the fallback: 100,000 plus 12,500 times the CPU index. The primary path uses `lapic_timer_calibrate()` to measure the timer after division by 16 against a calibrated TSC, before AP startup. Calibration seeks five samples of at least 10 ms within 15 attempts, requires at least three accepted samples and selects the median rate. TSC readings bracket each timer-register endpoint; brackets wider than 10 µs reject the sample (`c/kernel/cpu/irq/lapic.c`, `c/kernel/cpu/smp/smp.c`).

The bootstrap processor advances the global tick count. `timer_ticks()` exposes delivered ticks, and `timer_ms()` multiplies them by 10 for `SYS_MONOTONIC_MS`. Scheduler timed waits expire against the tick count (`c/drivers/timer/pit.c`, `c/kernel/exec/syscall.c`, `c/kernel/sched/sched.c`).

## Clock sources and calibration

The timekeeper selects its elapsed-time counter through this policy:

| Source | Selection and role |
|---|---|
| TSC | Accepted when CPUID reports TSC support and either invariant TSC or a hypervisor; selected after successful calibration |
| PIT tick count | Initial fallback, converted at `TIMER_HZ` |
| HPET main counter | Discovered after physical memory initialization; provides another TSC calibration reference and replaces PIT when TSC remains unavailable |
| CMOS RTC | Establishes wall time and supplies seconds readings for runtime comparisons |

The selection policy lives in `c/kernel/core/ktime.c`; hardware access lives in `c/drivers/timer/hpet.c` and `c/drivers/timer/rtc.c`.

Initial TSC calibration measures three PIT channel-2 intervals of `0x7FFF` counts and selects the median frequency. Channel 2 supplies a polled interval while channel 0 provides the periodic tick. The median must lie between 10 MHz and 100 GHz (`c/kernel/core/ktime.c`).

After physical memory initialization, `time_platform_init()` discovers HPET. The driver validates the ACPI description, MMIO mapping and hardware capabilities. It disarms firmware comparator interrupts, clears legacy replacement routing, enables the main counter and checks that it advances. Clearing the legacy route returns IRQ0 and IRQ8 to PIT and RTC routing. Boot then probes PIT delivery over a nominal 50 ms clock interval with an additional iteration bound (`c/drivers/timer/hpet.c`, `c/kernel/init/kmain.c`).

When initial TSC calibration failed, the timekeeper tries HPET as a calibration reference. It collects three samples spanning at least 20 ms, with up to eight attempts per sample. HPET reads bracket each TSC endpoint; brackets wider than about 10 µs trigger a retry. The median passes the same 10 MHz to 100 GHz check. Success selects TSC and retains HPET as an available source (`c/kernel/core/ktime.c`).

The `make TIMENOCH2=1` build option forces channel-2 calibration attempts to fail, providing a way to exercise HPET calibration during boot (`Makefile`, `c/kernel/core/ktime.c`).

## Nanosecond clocks and wall time

The kernel interfaces are declared in `c/kernel/core/ktime.h`:

| Interface | Meaning |
|---|---|
| `time_mono_ns()` | Nanoseconds since timekeeping initialization, with an atomic monotonicity clamp |
| `time_mono_raw_ns()` | Nanoseconds before the clamp |
| `time_real_ns()` | Monotonic time plus the wall-clock offset |
| `time_wall_sync()` | Refreshes the offset from the RTC |

The timekeeper stores an accumulated nanosecond base and a corresponding counter reading. Readers sample that state through a sequence counter and interpolate using the current source. Conversion uses a fixed-point multiplier and shift with a 128-bit intermediate product (`c/kernel/core/ktime.c`).

Counter subtraction applies the mask for the source width and sign-extends the result. Negative deltas contribute zero elapsed time. `time_mono_ns()` publishes an atomic maximum across readers, clamping a reading behind the published value and recording the correction count and largest correction.

`time_set_source()` first folds elapsed cycles from the old source. It publishes the new source and its matching counter baseline within a single update of the sequence counter, carrying the accumulated nanoseconds forward. Pairing source selection with the baseline prevents a reader from combining values from different counters (`c/kernel/core/ktime.c`).

At initialization, `time_wall_sync()` reads the RTC and establishes the offset used by `time_real_ns()`. Subsequent realtime reads advance with the elapsed-time source. RTC index/data accesses use a lock. The seconds reader on the interrupt side takes a trylock and a cached value to keep sampling bounded (`c/drivers/timer/rtc.c`).

User programs read clocks through `SYS_CLOCK_GETTIME`. `SYS_CLOCK_INFO` reports source, frequency, resolution, clock readings, backstep diagnostics and timer statistics. Its second argument can request a source change (`c/kernel/exec/syscall.c`, `include/abi/logit_abi.h`).

## Kernel timers and diagnostics

Kernel timers occupy a binary min-heap with capacity `KTIMER_MAX`, currently 128. Each `struct ktimer` stores its heap index, allowing cancellation to locate its entry directly. Insertion and removal take logarithmic work; the root supplies the earliest deadline (`c/kernel/core/ktime.c`, `c/kernel/core/ktime.h`).

`ktimer_run()` drains timers due at its supplied timestamp. Periodic timers advance from the previous deadline, avoiding accumulated service latency. When the next deadline is already due, the timer advances to the supplied timestamp plus one period. Callbacks run with the timer lock released, allowing them to arm or cancel timers and queue work.

Timekeeping initialization reports the source, frequency, resolution and calibration samples. It arms runtime checks for RTC agreement, timer lateness, source switching and monotonic reads across CPUs. The check for source switching temporarily selects PIT and restores the saved source. The RTC comparison also feeds a doubled elapsed interval to the checker as a negative control. Diagnostics for lost ticks compare elapsed source time with delivered interrupts (`c/kernel/core/ktime.c`).

During scheduler observation, common and dynamic interrupt entries record timestamps in C after their assembly register saves. Records retain the vector and nesting context. `SYS_SCHED_OBSERVE` controls and exports observation runs; scheduler tick housekeeping expires overdue observation inputs, releases queued groups in bounded batches and advances shutdown of a run (`c/kernel/sched/observe/flight_recorder.c`, `c/kernel/sched/observe/observe_control.c`, `c/kernel/sched/sched.c`, `c/kernel/exec/syscall.c`).

The relevant checks are:

| Target or file | Coverage |
|---|---|
| `test-time-host` | Timer heap operations, conversion, counter wrap, backward readings, monotonic clamping and source publication |
| `test-time` | Guest clock diagnostics, RTC comparison, a negative control that doubles elapsed time, PIT source switching and reporting of timer lateness |
| `test-time-smp` | Reported clock reads, participating CPUs and observed regressions in boots configured with one and four CPUs |
| `tests/unit/hpet_test.c` | HPET table and capability validation, comparator disarming and legacy-route clearing |

The time targets are defined in `Makefile`; their implementations are `tests/unit/time_test.c`, `tests/boot/run-time-test.sh` and `tests/boot/run-time-smp-test.sh`.

## Limitations

- Interrupt entry preserves state through FXSAVE/FXRSTOR. AVX stays disabled because its extended register state requires a broader save mechanism (`c/boot/isr.asm`, `c/boot/long.asm`).
- Kernel timer callbacks execute from the bootstrap processor’s periodic tick and must avoid blocking. Callback delivery keeps tick granularity whatever the clock resolution. HPET comparator scheduling and tickless operation are absent; AP timers use fixed fallback counts when calibration is unavailable (`c/kernel/core/ktime.c`, `c/kernel/core/ktime.h`, `c/drivers/timer/hpet.c`, `c/kernel/cpu/smp/smp.c`).
- Durations derived from delivered ticks lose elapsed periods when interrupt delivery coalesces edges. Signed counter-delta interpretation also requires sampling within half the counter’s wrap period (`c/drivers/timer/pit.h`, `c/kernel/core/ktime.c`).
- The reader that samples through the sequence counter stops retrying after 1,000 attempts and can then calculate from an unstable snapshot (`c/kernel/core/ktime.c`).
- The timekeeper’s CPU accounting samples the process on the bootstrap processor. No scheduler path calls its `time_account_switch()` hook, so these samples omit execution on application processors (`c/kernel/core/ktime.c`, `c/kernel/sched/sched.c`).
- I/O APIC and MSI destinations use eight-bit encodings and reject APIC IDs of 255 or greater. ACPI PCI routing supports segment zero, with bridge traversal bounded to 32 hops (`c/kernel/cpu/irq/apic_model.c`, `c/kernel/pci/pci_prt_aml.c`).
- Source comments identify X79-VG4 hardware validation as pending for ACPI/PCH PCI routing, vector retirement and LAPIC calibration. Their recorded evidence comes from models or QEMU (`c/kernel/pci/pci_prt_aml.c`, `c/kernel/pci/pci_msi.c`, `c/drivers/core/irq.c`, `c/kernel/cpu/irq/lapic.c`).

## See also

- [Local APIC and I/O APIC](../kernel/apic.md)
- [Timer hardware](../drivers/timers.md)
- [Scheduling](../kernel/scheduling.md)
- [Signals](../kernel/signals.md)
- [Multiprocessor support](smp.md)
- [System calls](system-calls.md)

## Source map

| File | What it contains |
|---|---|
| `c/boot/isr.asm` | Static stubs, register and floating-point saves, interrupt return |
| `c/boot/long.asm` | CPU state setup and AVX save-state requirements |
| `c/kernel/cpu/irq/idt.c` | Shared IDT construction and gate installation |
| `c/kernel/cpu/irq/interrupts.h` | Saved register layout and dispatcher interface |
| `c/kernel/cpu/irq/interrupts.c` | Exceptions, static IRQs, timer dispatch and common exit |
| `c/kernel/cpu/power/mce.c` | Machine-check entry, bank diagnostics and recovery decisions |
| `c/kernel/cpu/irq/lapic.c` | APIC access modes, virtual wire, EOI, IPIs and timer calibration |
| `c/kernel/cpu/irq/ioapic.c` | GSI routing, readback verification and controller draining |
| `c/kernel/cpu/irq/ioapic_map.c` | Controller ranges and GSI lookup |
| `c/kernel/cpu/irq/apic_model.c` | Destination encoding and acknowledgement rules |
| `c/kernel/cpu/smp/smp.c` | Legacy routing handoff, cleanup IPIs and AP timers |
| `c/drivers/core/irq.c`, `c/drivers/core/irq.h` | Dynamic vectors, callback pins and vector quarantine |
| `c/kernel/pci/pci_msi.c` | MSI-X, MSI, shared INTx and route changes |
| `c/kernel/pci/pci_prt_aml.c` | ACPI interrupt model, routing tables, links and bridge swizzling |
| `c/kernel/init/power.c` | ACPI initialization worker and PCI rerouting handoff |
| `c/drivers/power/acpi/devices.c` | Namespace loading and initialization order |
| `c/drivers/net/netdev.c` | NIC interrupt mode selection and registration |
| `c/drivers/char/serial.c` | Queued output, timer draining and blocking flush |
| `c/drivers/timer/pic.c` | PIC remapping, masking and acknowledgement |
| `c/drivers/timer/pit.c`, `c/drivers/timer/pit.h` | PIT programming and interfaces for delivered ticks |
| `c/drivers/timer/hpet.c` | HPET validation, firmware cleanup and counter access |
| `c/drivers/timer/rtc.c` | CMOS locking, calendar reads and Unix-time conversion |
| `c/kernel/core/ktime.c`, `c/kernel/core/ktime.h` | Clock selection, conversion, timer heap and diagnostics |
| `c/kernel/init/kmain.c` | Boot initialization order and PIT delivery probe |
| `c/kernel/mm/virt/fault.c` | Page-fault classification and resolution |
| `c/kernel/exec/signal/ksignal.c` | Fault recording and periodic signal work |
| `c/kernel/sched/sched.c` | Timed-wait expiry and CPU-limit checks |
| `c/kernel/sync/work.c` | Softirqs, tasklets and workqueue execution |
| `c/kernel/sched/observe/flight_recorder.c` | Interrupt timestamps and nesting records |
| `c/kernel/sched/observe/observe_control.c` | Observation control, export and periodic housekeeping |
| `c/kernel/exec/syscall.c`, `include/abi/logit_abi.h` | Clock and scheduler-observation syscall interfaces |
| `Makefile` | Time test targets and channel-2 calibration fault injection |
| `tests/unit/time_test.c` | Timer, conversion, wrap, clamp and publication checks |
| `tests/unit/hpet_test.c` | HPET description, capability and firmware state checks |
| `tests/boot/run-time-test.sh`, `tests/boot/run-time-smp-test.sh` | Guest timekeeping and checks for CPU participation |
