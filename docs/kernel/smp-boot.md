# SMP boot

LogitOS starts application processors (APs) from the bootstrap processor (BSP), assigns each AP a stack and CPU slot, and admits it to the online CPU set after local initialization. ACPI supplies startup candidates, while CPUID sampled on each processor supplies topology. `c/kernel/cpu/smp/smp.c` coordinates startup, `c/kernel/cpu/smp/smp_boot_model.c` implements publication and admission, and `c/boot/ap_trampoline.asm` brings each AP into long mode.

## Boot placement and discovery

`kernel_main()` calls `smp_init()` after memory initialization, platform clock setup and the first desktop render. Scheduler initialization follows later in `wm_run()`. APs that complete startup wait between these stages with their local timers armed. The call order appears in `c/kernel/init/kmain.c` and `c/kernel/gui/wm.c`.

The BSP first initializes its local APIC through `lapic_virtual_wire()`, called after physical memory initialization and before platform clock setup. This configures LINT0 for PIC delivery, LINT1 for NMI delivery and task priority zero. The later `lapic_init()` call in `smp_init()` checks and reuses the selected backend.

`smp_init()` calls `acpi_init()` to enumerate processors from the Multiple APIC Description Table (MADT). The parser in `c/kernel/cpu/acpi/acpi.c` delegates processor record validation to `c/kernel/cpu/irq/apic_model.c`.

| MADT entry | Retained identity |
|---|---|
| Type 0, Processor Local APIC | Eight-bit APIC ID and ACPI processor UID. |
| Type 9, Processor Local x2APIC | 32-bit APIC ID and ACPI processor UID. |

Accepted records have the enabled flag set and pass length and identity checks. Matching APIC ID and UID pairs collapse into one candidate. Reuse of either identifier with a different partner rejects the incoming record. `acpi_cpu_apic_id()` exposes accepted candidates in firmware order.

`lapic_init()` selects memory mapped xAPIC access or MSR based x2APIC access using CPUID and `IA32_APIC_BASE`. Production initialization adopts the mode that firmware selected. Each AP checks that its local mode matches the BSP’s selection. This logic is in `c/kernel/cpu/irq/lapic.c`.

The BSP occupies slot zero. Each admitted AP extends a contiguous range of online slots. Hardware APIC IDs address processors; slots index kernel CPU state.

## Startup mailbox

The BSP copies the trampoline to physical address `0x8000`. A STARTUP IPI with vector 8 begins execution there. Four 64-bit mailbox words describe the assignment:

| Physical address | Contents |
|---|---|
| `0x8F00` | Kernel page table root from `vmm_kernel_cr3()`. |
| `0x8F08` | Top of the AP’s 64 KiB stack, aligned to 16 bytes. |
| `0x8F10` | Address of `ap_entry()`. |
| `0x8F18` | Assigned CPU slot. |

The assembly consumes the first three words; `ap_entry()` reads the fourth. The producer is in `c/kernel/cpu/smp/smp.c`, and the assembly consumer is in `c/boot/ap_trampoline.asm`. `vmm_kernel_cr3()` is implemented in `c/kernel/mm/virt/vmm.c`.

AP startup proceeds one candidate at a time because all APs share this mailbox. Admission completes before the BSP assigns the mailbox to another candidate.

## Startup sequence

1. The BSP registers its APIC ID in slot zero, calls `lapic_timer_calibrate()`, and initializes its power state through `pstate_init()`, `cpuidle_init()` and `cpu_power_sample()`. It also captures its local topology. These calls precede AP startup in `c/kernel/cpu/smp/smp.c`.

2. The BSP prepares device interrupt routing. It initializes the discovered I/O APICs and attempts to route the legacy timer, keyboard and mouse interrupts to itself. Successful routing masks the legacy PIC. ACPI retains multiple controllers through `c/kernel/cpu/acpi/madt_irq.c`; `c/kernel/cpu/irq/ioapic_map.c` selects the controller covering each global system interrupt (GSI). Network interrupt registration follows through `netdev_irq_route()` in `c/drivers/net/netdev.c`.

3. The BSP walks the MADT candidates, skips its own APIC ID and checks that the active LAPIC backend can address each destination. The assigned slot equals the current online CPU count.

4. The BSP allocates the AP stack, registers the candidate’s APIC ID in that slot and fills the mailbox. A release store sets the slot to `SMP_AP_CLAIMED` before startup commands are issued.

5. `lapic_start_ap()` sends INIT, waits at least 10 ms, sends a STARTUP IPI for vector 8, waits at least 200 μs, sends another STARTUP IPI and waits another 200 μs. Production delays use the monotonic clock because instruction counts vary across processors and emulators. The sequence is implemented in `c/kernel/cpu/irq/lapic.c`.

6. The trampoline starts with interrupts disabled and enters protected mode. It enables PAE, clears five-level paging selection and loads the supplied page table root. It enables long mode and, when CPUID advertises NX, enables NX before paging. This ordering lets the AP interpret existing page table entries containing NX bits.

7. In 64-bit mode, the trampoline configures SSE/SSE2 support, initializes x87 and MXCSR, clears `CR4.OSXSAVE` and enables SMEP when advertised. Clearing OSXSAVE preserves the kernel’s FXSAVE state contract. The trampoline loads the assigned stack and calls `ap_entry()`.

8. `ap_entry()` validates the assigned slot and claimed state, initializes the local APIC, loads the shared IDT and checks its hardware APIC ID against the assignment. It installs its GDT and TSS through `percpu_ap_init()`, then calls `pat_init()`, `mce_init()`, `pstate_init()`, `cpuidle_init()` and `cpu_power_sample()`. It captures local topology, arms its periodic LAPIC timer and reports protection state.

9. The AP publishes completion of local initialization. The BSP observes that publication, validates the slot’s APIC identity and commits the slot to the online range. The AP waits for scheduler readiness before creating its idle thread record and entering scheduling.

## Local CPU state

`idt_load()` loads the shared IDT on each AP. `percpu_ap_init()` builds and loads that CPU’s GDT and TSS, initially supplying a 16 KiB stack for entry from user mode. `this_cpu()` identifies the running CPU from its loaded GDT base. These mechanisms are in `c/kernel/cpu/irq/idt.c` and `c/kernel/cpu/smp/percpu.c`.

PAT initialization programs the same cache type layout used by the BSP before the AP can participate in framebuffer work. Machine-check initialization configures error reporting on processors with the required capabilities. The shared IDT directs vector 18 to `mce_entry()`, whose handler reports machine-check banks through an emergency serial path that bypasses logging locks. These paths are implemented in `c/kernel/cpu/power/pat.c`, `c/kernel/cpu/power/mce.c` and `c/kernel/cpu/power/hardware.c`.

## Timer setup

Before publication, each AP arms a periodic LAPIC timer on vector 32 with divide value 16. `lapic_timer_count_ns()` converts `1.6 ms + slot * 0.2 ms` into a reload count. The earlier empirical reload, `100000 + slot * 12500`, remains the fallback when conversion returns zero. Varying the periods spreads timer activity across CPUs to reduce synchronized contention for scheduler state. The timer also wakes a parked AP so it can check scheduler readiness.

The BSP measures the divided timer against a calibrated TSC in `lapic_timer_calibrate()`. Each attempt observes a nominal 10 ms interval. Timestamp brackets surround the write of the initial count and the read of the current count; brackets longer than 10 μs cause sample rejection. The estimator uses bracket midpoints to calculate elapsed time.

Calibration seeks five valid samples within fifteen attempts and requires at least three. Samples are sorted, and the element at index `valid / 2` supplies the cached rate: the upper middle element when four samples survive. Each attempt stops the measurement timer. `c/kernel/cpu/irq/lapic.c` contains calibration, conversion and periodic timer programming.

## Publication and scheduler handoff

The startup state machine distinguishes resource assignment, AP publication and rejection:

| State | Meaning |
|---|---|
| `SMP_AP_EMPTY` | Initial slot state. |
| `SMP_AP_CLAIMED` | The BSP has assigned startup resources and a slot. |
| `SMP_AP_PUBLISHING` | The AP has entered the publication transition. |
| `SMP_AP_ONLINE` | The AP has published completion of local initialization. |
| `SMP_AP_REJECTED` | Initialization failed or the BSP cancelled startup. |

`smp_ap_publish_online()` uses atomic compare and exchange operations to move from claimed through publishing to online. Successful transitions use acquire and release ordering. The BSP acquires the published state, then `smp_bsp_commit_online()` advances the online count from the assigned slot to the following slot.

Publication exposes the AP’s initialized state. Admission extends the range used by online CPU consumers. Keeping admission under BSP control preserves contiguous slots even when a candidate fails. The protocol is implemented in `c/kernel/cpu/smp/smp_boot_model.c`.

In `c/kernel/gui/wm.c`, `wm_run()` calls `sched_init()` followed by `smp_mark_sched_ready()`. Each AP then calls `thread_create_idle()` and enters `sched_become_idle()`. Its startup stack becomes its idle execution context; the first context switch away saves that context. A timer tick during the earlier parking phase reaches a scheduler that returns when the CPU’s current thread is null. These paths are in `c/kernel/sched/sched.c`.

The scheduler’s idle loop calls `cpuidle_enter()`. Its local policy selects HLT or MONITOR/MWAIT according to CPU capabilities and available firmware idle hints. MONITOR/MWAIT eligibility includes the Always Running APIC Timer feature and support for waking on pending interrupts with interrupts disabled. The policy is in `c/kernel/cpu/power/cpuidle.c`.

## Topology detection

`capture_topology()` samples CPUID locally on the BSP and each AP through `cpu_platform_current_core()`. MADT enumeration supplies candidates, and successful startup admission determines the online count.

The decoder in `c/kernel/cpu/cpu_platform.c` prefers topology leaf `0x1F`, then tries `0x0B`. It validates hierarchy counts, shifts, level numbering, termination and consistent APIC IDs across subleaves. The SMT boundary supplies the thread ID; the highest accepted hierarchy boundary supplies the package ID. Bits between those boundaries form the core ID within the package, incorporating additional hierarchy levels.

Legacy decoding derives topology from leaf 1 and, where available, leaf 4. Hybrid processors require usable extended topology for a valid topology sample. On Intel processors with a supported leaf `0x1A` and nonzero EAX, the local sample also records core type and native model ID.

`c/kernel/cpu/smp/smp_topology.h` retains these core class values:

| Value | Class |
|---|---|
| `0x40` | Performance core. |
| `0x20` | Efficiency core. |
| `0x00` | Unknown core. |

`smp_topology_store()` joins each sample to the APIC identity established during startup. A valid sample whose CPUID APIC ID differs from that identity becomes an unknown record keyed by the startup APIC ID, and the mismatch is logged. Invalid samples receive the same grouping by identity.

`smp_topology_summarize()` groups valid records by package and core ID. Invalid records group by startup APIC ID. Conflicting sibling classes produce an unknown core and increment the conflict count. Sampling each logical CPU supports aggregation of packages containing both cores with SMT and cores with one thread. This bookkeeping is in `c/kernel/cpu/smp/smp_topology.c`.

`smp_cpu_topology_get()` exposes records by online slot. `smp_topology_get_summary()` aggregates present records within the admitted range.

## Failure handling

The coordinator responds according to startup progress:

| Condition | Response |
|---|---|
| BSP local APIC initialization fails, or ACPI returns an error or zero candidates | Return with the BSP as the online CPU. |
| LAPIC timer calibration returns zero | Continue with the fixed AP timer reloads. |
| Candidate APIC ID exceeds the active backend’s addressing range | Log the ID and continue to another candidate. |
| AP stack allocation fails | Continue to another candidate. |
| AP reads an invalid slot or a state other than claimed | Halt that AP with interrupts disabled. |
| AP local APIC initialization fails or hardware identity differs from the assignment | Mark the slot rejected and halt the AP. |
| Startup command fails or the readiness wait expires | Resolve publication against cancellation; rejection ends further AP attempts and retains startup resources. |
| Published slot identity or online count fails admission checks | Log the failure and halt the BSP. |
| I/O APIC initialization or legacy routing reports a safe rejection | Retain PIC delivery and continue. |
| I/O APIC masking or legacy route rollback reports an unsafe state | Halt the BSP. |

After a successful command sequence, `wait_ap_terminal()` waits for publication or rejection, checking a deadline of one second. A stalled clock also ends the wait after `SMP_AP_WAIT_STALL_SPINS`, whose default is 100 million consecutive iterations that read the same timestamp.

A timeout captures one moment in concurrent startup. `smp_bsp_cancel_unpublished()` settles the race by atomically changing a claimed or publishing slot to rejected. If publication reaches online first, the BSP proceeds with validation and admission. If cancellation wins, the AP’s publication operation fails and the AP halts.

For a rejected startup attempt, the coordinator stops starting further APs and keeps the stack, the mailbox and the slot assignment in place, which preserves the assignment for a processor that arrives late. `smp_ap_stack_may_free()` encodes the retention rule in `c/kernel/cpu/smp/smp_boot_model.c`.

The LAPIC backend bounds polling of xAPIC delivery status and reports unavailable or stalled startup timing. Errors propagate through `lapic_start_ap()` into the cancellation path.

Before fatal BSP halts for unsafe interrupt routing or failed admission checks, `smp_init()` calls `serial_flush_blocking()`. Its bounded drain attempts to transmit queued diagnostics before the halt loop disables interrupts.

## Diagnostics and test coverage

Startup logs report detected and online CPU counts, admissions and failures. Topology logs include the source leaf, package/core/thread IDs, core class, native model data and APIC mismatches. The aggregate report counts logical CPUs and cores by class.

Calibration logs report the divided timer frequency or the reason for retaining fixed counts. After the startup loop, `cpu_power_report()` prints captured package and CPU data, including temperature, ratio and microcode information where available. Power sampling and reporting are in `c/kernel/cpu/power/thermal.c`.

`tests/unit/x2apic_test.c` contains hosted assertions for MADT decoding, APIC destination encoding, startup command order, requested delays, stack retention and publication races. Its calibration scenarios inject delays around timer register accesses and check sample rejection, minimum sample count, median selection, bounded retries and timer shutdown.

`tests/unit/x2apic_run.py` runs these scenarios and a negative control using the earlier unbracketed estimator. In `tests/x2apic.mk`, `test-x2apic-host` depends on `test-x2apic-negctl`.

`tests/unit/raptor_smp_test.c` checks a synthetic hybrid topology with 28 logical CPUs and 20 cores, wide APIC IDs, identity mismatches and cancellation during publication.

## Limitations

- MADT candidate storage and CPU state arrays each have a production capacity of 32. `ACPI_MAX_CPUS` is defined in `c/kernel/cpu/acpi/acpi.h`; `PERCPU_MAXCPU` is defined in `c/kernel/cpu/smp/percpu.h`. The online range includes the BSP.
- The trampoline uses fixed low physical addresses and loads the page table root through a 32-bit register, requiring that root below 4 GiB.
- Topology serves diagnostics. The scheduler policy reported by `c/kernel/cpu/smp/smp.c` treats online CPUs uniformly.
- AP timer reloads share the BSP’s measured rate. The fallback with fixed counts depends on the machine. Source comments record pending X79-VG4 hardware verification for calibration, virtual-wire setup, PAT programming and multiple I/O APIC routing.
- Online publication precedes scheduler initialization and idle thread allocation. Allocation failures in `sched_init()` and `thread_create_idle()` log an error and return, while their callers continue the handoff.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/init/kmain.c` | Boot ordering, early BSP LAPIC setup and placement of SMP startup. |
| `c/kernel/cpu/smp/smp.c` | Coordinator, mailbox, AP entry, timers, topology accessors and failure handling. |
| `c/kernel/cpu/smp/smp_boot_model.h` | Startup states and publication contracts. |
| `c/kernel/cpu/smp/smp_boot_model.c` | Atomic publication, admission, cancellation and stack retention. |
| `c/boot/ap_trampoline.asm` | Execution mode transitions, CPU setup and entry into C. |
| `c/kernel/mm/virt/vmm.c` | Kernel page table root accessor. |
| `c/kernel/cpu/acpi/acpi.c` | MADT enumeration and candidate accessors. |
| `c/kernel/cpu/acpi/acpi.h` | ACPI CPU capacity and enumeration interface. |
| `c/kernel/cpu/acpi/madt_irq.c` | MADT I/O APIC decoding and controller deduplication. |
| `c/kernel/cpu/irq/apic_model.c` | Processor record validation, deduplication and APIC destination encoding. |
| `c/kernel/cpu/irq/lapic.c` | APIC backends, virtual-wire setup, startup IPIs and timer calibration. |
| `c/kernel/cpu/irq/ioapic.c` | Controller initialization, legacy routing, masking and rollback. |
| `c/kernel/cpu/irq/ioapic_map.c` | Controller GSI ranges and selection. |
| `c/kernel/cpu/irq/idt.c` | Shared IDT loading and machine-check vector installation. |
| `c/kernel/cpu/smp/percpu.h` | CPU state structure and slot capacity. |
| `c/kernel/cpu/smp/percpu.c` | CPU GDTs, TSSs, entry stacks and CPU identification. |
| `c/kernel/cpu/cpu_platform.c` | Local CPUID topology and core type decoding. |
| `c/kernel/cpu/smp/smp_topology.h` | Topology records, classes and summary fields. |
| `c/kernel/cpu/smp/smp_topology.c` | Topology validation and aggregation. |
| `c/kernel/cpu/power/pat.c` | PAT programming before CPU publication. |
| `c/kernel/cpu/power/mce.c` | Machine-check initialization, exception entry and diagnostics. |
| `c/kernel/cpu/power/pstate.c` | CPU frequency policy initialization. |
| `c/kernel/cpu/power/cpuidle.c` | CPU idle policy and idle entry. |
| `c/kernel/cpu/power/hardware.c` | Privileged power operations and emergency serial output. |
| `c/kernel/cpu/power/thermal.c` | CPU power sampling and reporting. |
| `c/drivers/net/netdev.c` | Network interrupt registration through the device model. |
| `c/drivers/char/serial.c` | Bounded serial draining before fatal stops. |
| `c/kernel/gui/wm.c` | Scheduler initialization and release of parked APs. |
| `c/kernel/sched/sched.c` | Idle thread adoption and scheduling entry. |
| `tests/unit/x2apic_test.c` | APIC, startup protocol and timer calibration assertions. |
| `tests/unit/x2apic_run.py` | Hosted scenarios and negative controls. |
| `tests/x2apic.mk` | Test targets and negative-control prerequisites. |
| `tests/unit/raptor_smp_test.c` | Hybrid topology and publication race fixtures. |

## See also

- [Multiprocessor support](../architecture/smp.md)
- [Kernel initialisation](init.md)
- [ACPI](acpi.md)
- [Local APIC and I/O APIC](apic.md)
- [CPU setup](cpu-setup.md)
- [Scheduling](scheduling.md)
