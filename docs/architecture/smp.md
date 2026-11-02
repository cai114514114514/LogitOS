# Multiprocessor support

LogitOS starts additional logical CPUs through a shared boot trampoline and brings them into a scheduler with per-CPU execution state and a shared runnable ring. Subsystem and object locks coordinate concurrent kernel work. Local APIC interrupts carry translation invalidations, framebuffer copy requests, wakeups and diagnostic requests. TLB shootdowns also use polling so CPUs waiting for ticket locks can acknowledge requests with interrupts disabled.

## CPU discovery and startup

The bootstrap processor (BSP) calls `smp_init()` from `c/kernel/init/kmain.c`. ACPI discovery reads enabled processors from MADT type 0 and type 9 entries. `c/kernel/cpu/irq/apic_model.c` validates those records, preserves their APIC identifiers and checks for duplicate or conflicting entries.

Each application processor (AP) receives a dense software slot in `g_cpus`. The slot indexes kernel state, while the APIC identifier addresses the processor through interrupt hardware, which accommodates sparse hardware identifiers. The LAPIC backend in `c/kernel/cpu/irq/lapic.c` preserves the firmware’s active xAPIC or x2APIC mode and checks that APs use the same mode as the BSP.

Startup proceeds one AP at a time in `c/kernel/cpu/smp/smp.c`:

1. The BSP copies the trampoline from `c/boot/ap_trampoline.asm` to physical address `0x8000`. A mailbox at `0x8F00` carries the kernel CR3, stack top, C entry address and claimed CPU slot.
2. The BSP allocates a 64 KiB startup stack, records the APIC identifier and publishes the slot as `SMP_AP_CLAIMED`.
3. `lapic_start_ap()` sends INIT, waits 10 ms, then sends two startup IPIs with a 200 µs wait after each. Startup vector 8 selects address `0x8000`. These delays use the monotonic clock.
4. The trampoline enters protected mode, loads the kernel CR3 and enters long mode. It enables NX before paging when supported, allowing the AP to interpret existing page permissions. It also initializes x87 and SSE state and enables SMEP when supported.
5. `ap_entry()` initializes the LAPIC, loads the shared IDT, checks the claimed APIC identity and builds the CPU’s GDT and TSS.

Before publishing online, the AP calls `pat_init()`, `mce_init()`, `pstate_init()`, `cpuidle_init()` and `cpu_power_sample()`. PAT setup programs the same cache attribute table used by the BSP before the AP can copy framebuffer bands. Machine-check setup and power policy initialization operate on the processor executing those calls. The AP also records its topology and arms its periodic LAPIC timer.

The BSP initializes PAT and machine-check handling in `c/kernel/init/kmain.c`. Within `smp_init()`, it initializes its power and idle policies before starting APs and reports the collected power samples after the admission loop.

Publication passes through `SMP_AP_PUBLISHING` to `SMP_AP_ONLINE`. Release ordering publishes the AP’s initialization; the BSP observes that state with acquire ordering before advancing the online count. The transitions in `c/kernel/cpu/smp/smp_boot_model.c` keep admitted CPUs in a dense prefix.

The BSP resolves startup failure or timeout through atomic cancellation. If the AP has already published `SMP_AP_ONLINE`, admission proceeds. If cancellation wins, the BSP retains the startup stack and mailbox contents, so a late arrival can still use the resources that processor may consume.

## Joining the scheduler

An online AP waits for scheduler readiness, waking through its LAPIC timer to check again. In `c/kernel/gui/wm.c`, `wm_run()` calls `sched_init()` and then `smp_mark_sched_ready()`. Each AP creates its idle thread and enters `sched_become_idle()` using its startup stack.

The scheduler in `c/kernel/sched/sched.c` maintains a shared circular runnable ring protected by `g_sched_lock`. It selects eligible threads by their weighted virtual CPU time. A thread’s `running` flag reserves it for its executing CPU. Each CPU also has an idle thread outside the runnable ring.

Dispatch updates the CPU’s current thread, its TSS stack pointer when the incoming thread supplies a kernel stack, its address space and its thread-local storage state. A blocked thread can resume on another CPU, so resume paths obtain the current CPU again through `this_cpu()`.

The idle loop calls `cpuidle_enter()` in `c/kernel/cpu/power/cpuidle.c`. It uses HLT by default. MONITOR/MWAIT selection requires Intel feature checks, an always running APIC timer, support for waking on interrupts while interrupts are masked, and a suitable firmware idle hint, which keeps a wake source available while the CPU is idle.

## AP timer calibration

Earlier documentation described AP timer reloads as uncalibrated counts tuned under QEMU TCG. The startup path now calls `lapic_timer_calibrate()` on the BSP before starting APs.

Calibration in `c/kernel/cpu/irq/lapic.c` measures the LAPIC timer after division by 16 against the calibrated timestamp counter (TSC). It seeks five valid samples in at most 15 attempts, using a 10 ms observation window. Timestamp brackets surround the timer register accesses; samples with either bracket longer than 10 µs are rejected. With at least three accepted samples, the estimator selects the middle sorted sample, using the upper middle sample when four survive.

`lapic_timer_count_ns()` converts the measured rate into each AP’s reload value. The requested period is 1.6 ms plus 0.2 ms per software CPU slot. Different periods spread scheduling interrupts across CPUs and reduce simultaneous contention for `g_sched_lock`.

AP timers use vector 32. In `c/kernel/cpu/irq/interrupts.c`, the BSP advances the shared tick counter and expires timed sleeps, while timer interrupts on each CPU provide scheduling opportunities.

The `test-x2apic-host` target in `tests/x2apic.mk` builds calibration cases using the production LAPIC implementation with simulated clocks and registers. Its negative-control prerequisite includes an estimator without endpoint rejection. `tests/unit/x2apic_run.py` requires that variant to fail the endpoint-stall assertion.

## Per-CPU data

`struct cpu`, declared in `c/kernel/cpu/smp/percpu.h`, holds the execution state associated with one logical CPU.

| Fields | Purpose |
|---|---|
| `index`, `lapic_id` | Software slot and hardware interrupt destination |
| `current`, `idle` | Current thread and local idle thread |
| `gdt`, `gdtr`, `tss` | Local descriptor tables and privilege transition stack |
| `in_kernel` | Kernel entry nesting depth |
| `exit_discard` | Storage for the outgoing stack pointer during thread exit |

`c/kernel/cpu/smp/percpu.c` supplies an initial 16 KiB privilege transition stack per CPU. Scheduler dispatch updates the local TSS stack pointer for threads with their own kernel stacks.

`this_cpu()` executes `sgdt` and derives the CPU slot from the loaded GDT’s address within `g_cpus`. It verifies the result and falls back to the BSP slot during early initialization. The source records the reason for using the GDT address: LAPIC MMIO reads had proved unreliable from the system call path under QEMU TCG.

## Kernel entry and locking

Historical comments describe a big kernel lock (BKL) around kernel entry. The correction dated 2026-09-10 in `c/kernel/cpu/spinlock.c` records its removal. Current entry code maintains per-CPU nesting state, while subsystem and object locks protect shared data.

On entry, `c/kernel/cpu/irq/interrupts.c` increments `in_kernel`. Timer dispatch calls `schedule()` when the entry began at depth zero. The system call dispatcher runs with interrupts enabled when entry came from user mode at depth zero and the saved flags enabled interrupts. Nested timer interrupts observe the increased depth. On return, the entry path restores the saved depth on the CPU where execution resumes.

Ticket locks in `c/kernel/cpu/spinlock.c` assign contenders tickets. An acquire load observes when a ticket becomes current; unlock advances the serving counter with release ordering. Locks record their owning CPU and caller address. Release checks emit serial diagnostics for ownership errors and ticket desynchronization.

The locking interfaces establish these rules:

- Use `spin_lock_irqsave()` for a lock shared between thread and interrupt contexts. It saves the interrupt state and disables local interrupts before acquisition. Pair it with `spin_unlock_irqrestore()`.
- Use bare `spin_lock()` where the calling context prevents interrupt re-entry into the same lock.
- Call `sched_block_self_unlock()` from thread context with the condition’s lock acquired through `spin_lock_irqsave()`. Release other spinlocks before parking and recheck the condition after waking.

The parking protocol acquires `g_sched_lock` while holding the caller’s object lock. It marks the thread blocked and removes it from the runnable ring before releasing the object lock. The scheduler lock remains held across the context switch and is released on the incoming stack. A waker acquiring that lock consequently observes a thread whose saved context is ready for dispatch.

Every ticket-lock wait iteration calls `tlb_service()`, so a CPU waiting with interrupts disabled can perform a requested translation flush.

During a scheduler observation run, `c/kernel/sched/observe/flight_recorder.c` records events in a ring per CPU. Spinlock timing begins before ticket acquisition and includes contention with interrupts disabled. Context switch hooks close the outgoing CPU’s timing and interrupt nesting records before changing stacks. The incoming thread records its start after execution reaches its own stack, keeping migration observations associated with the CPU where each event occurred.

## Interprocessor interrupts

`lapic_send_ipi()` sends a fixed interrupt to one APIC identifier. The xAPIC backend writes the memory-mapped interrupt command registers and waits for delivery status. The x2APIC backend writes the interrupt command MSR. Receiving protocols track completion through acknowledgements or work state.

The main SMP vectors are installed by `c/kernel/cpu/irq/idt.c` and dispatched through `c/boot/isr.asm` and `c/kernel/cpu/irq/interrupts.c`:

| Vector | Purpose | Dispatch |
|---|---|---|
| 240 | TLB shootdown | `tlb_ipi()` |
| 241 | Framebuffer row-band copy | `smp_present_ipi()` |
| 242 | Delivery probe in builds defining `LOGIT_RAPTOR_SMP_QEMU_TEST` | `smp_test_ipi_interrupt()` |
| 243 | Wakeup and device IRQ vector retirement | LAPIC cleanup hook and acknowledgement |

Diagnostics install additional interrupt gates. `c/kernel/diag/kprof.c` sends profiling IPIs on vector 237. `c/kernel/diag/kdiag.c` installs vector 238 for stopping CPUs during panic handling and vector 239 for an interrupt-context logging probe.

After making compositor workers runnable, `smp_kick_others()` sends vector 243 to the other online CPUs. An idle CPU wakes and reaches `schedule()` through its idle loop.

Device IRQ retirement in `c/drivers/core/irq.c` also calls `smp_kick_others()`. The cleanup hook runs before LAPIC acknowledgement and checks each CPU’s local pending and in-service bits through `lapic_vector_pending()`. A retired vector remains quarantined until all potential target CPUs clear their cleanup bits. The kick reaches the BSP even when its timer uses the PIT and PIC. Dynamic IRQ completion also retries cleanup and kicks peers when retirement remains pending.

## Parallel presentation

For parallel framebuffer presentation, `c/kernel/cpu/smp/smp.c` divides a rectangle into disjoint row bands. Each worker atomically claims its band and publishes completion after copying it. After a bounded initial wait, the presenter claims pending bands itself and waits for copies already executing. The band table remains in use until those copies complete.

`fb_present_rect()` in `c/kernel/gui/fb/fb.c` serializes presentation through `fb_graphics_lock()` and uses the parallel callback for rectangles at least 128 rows tall when that path is reached. It loads the callback once with acquire ordering; callback updates use release ordering.

Timeouts feed the policy in `c/kernel/cpu/smp/present_policy.c`. Four consecutive timeouts, or at least eight timeouts among the last 64 parallel presents once the window is full, trigger callback removal after the current fallback finishes. Later CPU copies use the local path, which avoids repeatedly paying the acknowledgement wait and fallback costs when peers service presentation IPIs slowly.

`smp_present_stats()` reports calls, successful waits, timeouts, fallback bands, per-CPU misses, wait cycles and counters captured at disablement. Its `missing_mask` identifies CPUs with recorded misses since boot. TSC measurements cover the initial acknowledgement wait. The hardware panel in `c/kernel/diag/hwhud.c`, selected by `HWHUD=1` in `Makefile`, displays these statistics and the parallel path’s state.

## TLB shootdown and page lifetime

`tlb_flush_all()` in `c/kernel/cpu/tlb.c` reloads the initiating CPU’s CR3, then acquires `g_tlb_send` with interrupts disabled. That lock serializes publication and completion of the shared shootdown transaction.

The initiator publishes a request flag for every other online CPU before sending vector 240. A receiver reaches `tlb_service()` through the IPI handler or a polling path such as a ticket-lock wait. An atomic exchange claims its request flag. The receiver reloads its current CR3 to flush non-global translations, then increments the acknowledgement count. The claim gives each target one acknowledgement per transaction.

The initiator retains transaction ownership until every peer acknowledges. An acknowledgement mismatch emits a raw UART diagnostic and halts the initiating CPU while retaining the sender lock. The diagnostic uses bounded UART writes, allowing it to proceed even when a missing CPU owns a console lock.

Frame reclamation depends on invalidation preceding release. In `c/kernel/mm/virt/vmm.c`, `vmm_unmap_range_in()` clears mappings, invalidates local translations when the space is active locally, and holds removed frame references in batches of 64. When another CPU is using that address space, `unmap_drain()` completes a shootdown before releasing the batch. `vmm_protect_range_in()` also requests a shootdown after changing permissions when the address space is busy elsewhere.

`vmm_switch()` publishes both the previous and incoming address spaces around the CR3 write. `vmm_space_busy_elsewhere()` examines both records, covering the transition between spaces.

The scheduler also maintains a global TLB generation and a flushed generation per CPU. Thread stack cleanup in `c/kernel/sched/uthread.c` bumps the generation after unmapping the stack. Scheduler dispatch and `sched_tlb_gen_check()` refresh CPUs whose generation is behind. System call entry invokes that check through `c/kernel/exec/syscall.c`, covering reuse of mappings between threads sharing an address space.

## Limitations

- Production CPU enumeration and per-CPU storage have a capacity of 32 logical CPUs, defined by `ACPI_MAX_CPUS` and `PERCPU_MAXCPU`.
- Scheduling applies a uniform policy across online CPUs. Performance and efficiency core topology serves diagnostic purposes only.
- When timer calibration returns zero, APs use a reload of 100,000 plus 12,500 per software CPU slot. These counts were tuned under QEMU TCG, so their period depends on the platform.
- AP admission requires usable ACPI discovery, compatible LAPIC modes and addressable IPI destinations. Cancellation of a started AP ends further admission for that boot.
- Entry depth suppresses timer preemption during kernel operations; blocking paths can switch threads.
- Scheduler observation measures interrupt masking through the spinlock API and context switch hooks. Bare `cli` and assembly intervals fall outside that coverage.
- A CPU remaining in a region with interrupts masked outside the polling paths can exhaust the shootdown wait budget. The fatal stop retains transaction ownership and prevents the initiating caller from continuing frame release.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/init/kmain.c` | BSP initialization and SMP startup call |
| `c/kernel/cpu/acpi/acpi.c`, `c/kernel/cpu/acpi/acpi.h` | MADT discovery and enumeration capacity |
| `c/kernel/cpu/irq/apic_model.c` | CPU record validation and APIC destination encoding |
| `c/kernel/cpu/irq/lapic.c` | APIC modes, startup IPIs, fixed IPIs, timer calibration and vector status |
| `c/boot/ap_trampoline.asm` | AP transition into long mode and C entry |
| `c/kernel/cpu/smp/smp.c`, `c/kernel/cpu/smp/smp.h` | AP admission, readiness, topology, presentation IPIs and statistics |
| `c/kernel/cpu/smp/smp_boot_model.c`, `c/kernel/cpu/smp/smp_boot_model.h` | Publication, cancellation and stack retention rules |
| `c/kernel/cpu/smp/percpu.c`, `c/kernel/cpu/smp/percpu.h` | CPU records, GDT/TSS setup and CPU lookup |
| `c/kernel/cpu/power/pat.c`, `c/kernel/cpu/power/mce.c` | Per-CPU cache attributes and machine-check setup |
| `c/kernel/cpu/power/pstate.c`, `c/kernel/cpu/power/cpuidle.c` | Frequency policy and idle state selection |
| `c/kernel/cpu/power/hardware.c`, `c/kernel/cpu/power/thermal.c` | CPU hardware operations, power sampling and reporting |
| `c/kernel/cpu/spinlock.c`, `c/kernel/cpu/spinlock.h` | Ticket locks, interrupt state handling and ownership diagnostics |
| `c/boot/isr.asm`, `c/kernel/cpu/irq/idt.c`, `c/kernel/cpu/irq/interrupts.c` | Interrupt entry, vector assignments, nesting depth and IPI dispatch |
| `c/drivers/core/irq.c` | Device IRQ vector quarantine and CPU cleanup |
| `c/kernel/cpu/smp/present_policy.c`, `c/kernel/cpu/smp/present_policy.h` | Presentation timeout history and disable thresholds |
| `c/kernel/cpu/tlb.c` | Serialized shootdowns, polling, acknowledgements and fatal timeout |
| `c/kernel/mm/virt/vmm.c` | Address-space tracking, unmap batches and permission invalidation |
| `c/kernel/sched/sched.c`, `c/kernel/sched/sched.h` | Runnable ring, dispatch, parking and TLB generations |
| `c/kernel/sched/observe/flight_recorder.c`, `c/kernel/sched/observe/flight_recorder.h` | Per-CPU observation rings and timing hooks |
| `c/kernel/sched/uthread.c`, `c/kernel/exec/syscall.c` | Stack cleanup generation bump and system call entry check |
| `c/kernel/gui/wm.c`, `c/kernel/gui/fb/fb.c` | Scheduler release, compositor wakeups and framebuffer presentation |
| `c/kernel/diag/kdiag.c`, `c/kernel/diag/kprof.c` | Diagnostic and profiling IPIs |
| `c/kernel/diag/hwhud.c`, `Makefile` | Presentation statistics display and hardware panel option |
| `tests/x2apic.mk`, `tests/unit/x2apic_run.py`, `tests/unit/x2apic_test.c` | APIC/SMP host cases, calibration scenarios and negative controls |

## See also

- [SMP boot](../kernel/smp-boot.md)
- [Local APIC and I/O APIC](../kernel/apic.md)
- [Synchronisation](../kernel/synchronization.md)
- [Processes and threads](processes-and-threads.md)
- [Memory model](memory-model.md)
- [Graphics and windowing](graphics-and-windowing.md)
