# Scheduling

LogitOS schedules kernel and user threads from a shared circular run list. It charges elapsed execution time to each thread, scales that charge by priority, and selects the eligible thread with the smallest accumulated virtual runtime. Scheduling, blocking, and thread exit all go through the same selection helper. The implementation is in `c/kernel/sched/sched.c`, with its public interface in `c/kernel/sched/sched.h`.

## Run queues and thread state

The scheduler maintains several lists under `g_sched_lock`:

| Object | Purpose |
|---|---|
| `g_ring` | Circular doubly linked list scanned for runnable work. |
| `g_all` | Directory of live threads, including sleepers and idle threads. |
| `g_timer_wait` | Unsorted list of threads parked with deadlines. |
| `dead_threads` | Exited threads awaiting descriptor and stack reclamation. |

The private `struct thread` holds its saved stack pointer, address space, TLS base, list links, blocking state, priority, and accounting fields. `THREAD_READY` and `THREAD_BLOCKED` describe readiness. The `running` flag records CPU ownership, allowing selection to distinguish an available ready thread from one executing on another CPU.

Blocking removes a thread from `g_ring`, reducing the number of entries visited during selection. There is one exception: `ring_unlink()` keeps a sole ring member, whose blocked state excludes it from dispatch. ID lookup walks `g_all`, so parked threads remain reachable. `schedule()` reclaims descriptors and stacks queued on `dead_threads`. These operations are in `c/kernel/sched/sched.c`.

During desktop startup, `wm_run()` calls `sched_init()` and then `smp_mark_sched_ready()`. Initialization adopts the boot context as the `wm` thread, creates the bootstrap CPU’s idle thread, and starts deferred work and diagnostic threads. Application processors enter the scheduler after the readiness flag is published. Initialization also prepares the observation facility through `observe_control_init()`. See `c/kernel/gui/wm.c`, `c/kernel/cpu/smp/smp.c`, and `c/kernel/sched/sched.c`.

## Selection algorithm and parameters

Accounting paths pass a reading from `time_mono_raw_ns()` to `cpu_fold()`. The helper adds the elapsed interval to `cpu_ns` and advances weighted virtual runtime using the same interval:

```c
t->vtime += (dt * (uint64_t)t->vmul) >> SCHED_VSHIFT;
```

`prio_apply()` precomputes the reciprocal multiplier:

```c
t->vmul = (unsigned)((SCHED_W_REF << SCHED_VSHIFT) / t->weight);
```

A higher weight produces a smaller charge to virtual runtime for the same execution interval. Precomputation moves division out of the accounting path. Both calculations use integer arithmetic. See `c/kernel/sched/sched.c`; the clock implementation is in `c/kernel/core/ktime.c`.

| Parameter | Value |
|---|---|
| `SCHED_NICE_MIN` | −20 |
| `SCHED_NICE_MAX` | 19 |
| Default nice value | 0 |
| `SCHED_W_REF` | 1024 |
| `SCHED_VSHIFT` | 10 |
| Weight at nice −20 | 4096 |
| Weight at nice −10 | 2048 |
| Weight at nice 0 | 1024 |
| Weight at nice 10 | 512 |
| Weight at nice 19 | 274 |

The table approximates `round(1024 × 2^(−nice/10))`. Nice values −10, 0, and 10 give exact factors of two in both weights and reciprocals. The complete weight range is approximately 14.9:1. The design discussion in `c/kernel/sched/sched.h` relates this bounded range to delays suffered by sleeping lock holders.

`pick_min_locked()` scans the ring for the smallest `vtime` among live, ready threads available to the calling CPU. Its candidates exclude idle threads, threads owned by another CPU, and the outgoing thread. `schedule()` considers the outgoing thread itself after charging its elapsed execution time:

1. Find the eligible alternative with the smallest virtual runtime.
2. Keep the current thread when it remains eligible and its virtual runtime is no greater than the alternative’s.
3. Use the CPU’s idle thread when runnable work is exhausted.

Traversal order resolves ties between alternatives. A tie involving the current thread favors continuing execution. Comparisons use signed differences between virtual runtime values. `SYS_YIELD` calls `schedule()`, so a yield requests selection and can return to the same thread. See `c/kernel/sched/sched.c` and `c/kernel/exec/syscall.c`.

`g_vnow` advances monotonically when the final selection has a greater virtual runtime and is a thread other than idle. Creation initializes a thread’s `vtime` from this floor. Reinsertion through `ring_link()` raises a value below the floor to `g_vnow`. This prevents a returning sleeper from accumulating a large scheduling credit during its wait.

The introductory comment in `c/kernel/sched/sched.h` still calls the scheduler round robin. That label describes the earlier policy; the current implementation selects by weighted virtual runtime.

## Priorities and nice values

`SYS_SCHED` exposes three operations through `sched_prio_syscall()`:

| Operation | Result |
|---|---|
| `SCHEDCTL_GET_NICE` | Read the target’s nice value. |
| `SCHEDCTL_SET_NICE` | Apply a value clamped to the supported range and return that value. |
| `SCHEDCTL_GET_WEIGHT` | Read the stored scheduling weight. |

The operations are defined in `include/abi/logit_abi.h` and implemented in `c/kernel/sched/sched.c`.

A PID of zero selects the caller’s process, or the calling kernel thread when it has no process. For a user process, `sched_nice_set()` updates every matching thread in `g_all`, including blocked threads. New threads inherit the creator’s nice value through `prio_inherit()`. Reweighting preserves accumulated `vtime`.

The setter obtains credentials before taking `g_sched_lock`, keeping credential lookup outside the scheduler’s lock domain. UID zero may change another process’s priority. Other callers must match the target’s UID and may keep or increase its nice value. Increasing nice reduces scheduling priority.

The libc interfaces `getpriority()`, `setpriority()`, and `nice()` are in `c/apps/libc/src/resource.c`. They support `PRIO_PROCESS`. `nice()` adds an increment to the current value; `setpriority()` translates a successful kernel result into the libc return value zero. Collisions between those return values and priority results are described under Limitations.

## SMP distribution and context switching

Every CPU selects work from `g_ring` while holding `g_sched_lock`. Selection sets the incoming thread’s `running` flag before another CPU can inspect the ring. This common pool distributes work among CPUs as they seek available threads. A thread can resume on a different CPU after being switched out.

`struct cpu` holds the current thread, idle thread, TSS, and kernel entry depth. `PERCPU_MAXCPU` is 32 in the standard configuration. Each CPU has an idle thread outside the shared ring; an application processor adopts its startup stack as its idle context. See `c/kernel/cpu/smp/percpu.h` and `c/kernel/sched/sched.c`.

The scheduler holds `g_sched_lock` across `context_switch()`. This protects the outgoing thread until assembly has saved its stack pointer. The incoming thread releases the lock through its resumed scheduling frame or its bootstrap on first dispatch.

`c/boot/switch.asm` saves callee-saved registers and flags, exchanges stack pointers, and returns with interrupts disabled. Dispatch updates the user trap stack, changes address space when required, synchronizes the TLB generation, and installs the incoming TLS base. Interrupt entry and return preserve general registers and FP/SSE state through `c/boot/isr.asm`.

Resumed scheduler frames reacquire the current CPU pointer before restoring kernel entry depth. The interrupt return path follows the same rule because a blocking syscall may resume on another CPU. Entry depth suppresses timer preemption during kernel execution. Historical BKL handoff comments remain beside these paths; the current code uses object locks and entry depth tracked per CPU. See `c/kernel/sched/sched.c`, `c/kernel/cpu/irq/interrupts.c`, and `c/kernel/cpu/spinlock.c`.

## Blocking and wakeup

Wait queues build on `sched_block_self_unlock()` and `sched_wake()`. Parking follows this protocol:

1. Acquire the predicate’s spinlock with interrupts saved, test the predicate, and register the waiter.
2. Acquire `g_sched_lock`, mark the thread blocked, and remove it from the run ring, subject to the exception above.
3. Release the predicate lock.
4. Switch context while retaining `g_sched_lock`.

A waker acquiring the predicate lock after its release encounters an already blocked thread. Its call to `sched_wake()` waits for `g_sched_lock` until the outgoing execution context has been saved. This ordering closes the gap between checking a condition and parking. The contract is documented in `c/kernel/sched/sched.h` and implemented in `c/kernel/sched/sched.c`.

`struct waitq` stores FIFO `struct waiter` records on waiting threads’ kernel stacks. `waitq_wake_one()` removes the oldest registration; `waitq_wake_all()` drains the queue. Scheduler policy determines dispatch order after wakeup. Waiters remove any remaining registration before returning, and predicate loops check the condition again after waking. See `c/kernel/sync/wait.h` and `c/kernel/sync/wait.c`.

Parking requires thread context, with the predicate lock as the caller’s sole held spinlock. Wake operations support interrupt context and calls from other CPUs.

`sched_wake_id()` performs lookup and wakeup under the scheduler lock. When its target is already ready, it records `wake_pending`. The next parking attempt consumes this latch and returns for another predicate check. This preserves an asynchronous wake arriving before parking; the signal path uses it in `c/kernel/exec/signal/ksignal.c`.

## Timers, idle policy, and accounting

The bootstrap CPU advances `timer_ticks()` and expires scheduler deadlines on timer vector 32. `TIMER_HZ` is 100, giving a nominal 10 ms tick. Deadline expiry occurs before the timer handler checks kernel entry depth for scheduling. See `c/drivers/timer/pit.h` and `c/kernel/cpu/irq/interrupts.c`.

`sched_block_self_unlock_until()` registers a tick deadline in `g_timer_wait`. `sched_timer_expire()` scans the list, makes expired threads ready, and records timeout as the wake cause. `sched_wake()` removes an armed timer registration when it unparks the thread.

`wait_deadline_ms()` rounds milliseconds upward to ticks and adds one tick to account for a tick arriving immediately after the request. `sched_sleep_ms()` uses this mechanism and checks the deadline again after waking. Both functions are in `c/kernel/sync/wait.c`.

Application processors use periodic LAPIC timers. Before starting them, `smp_init()` calls `lapic_timer_calibrate()` on the bootstrap CPU. Calibration measures the divided LAPIC timer against a calibrated TSC, rejects samples with delayed register accesses, and takes the median of accepted samples. AP index `idx` requests a period of 1.6 ms plus `idx × 0.2 ms` through `lapic_timer_count_ns()`. Staggering periods reduces synchronized contention on the scheduler lock. See `c/kernel/cpu/smp/smp.c` and `c/kernel/cpu/irq/lapic.c`.

Earlier documentation described AP reloads as uncalibrated. The current path uses calibration, with the former fixed counts retained as fallback: `LAPIC_AP_TIMER_COUNT` is 100000, plus one eighth of that base per CPU index. The clock code first attempts TSC calibration against PIT channel 2, then offers an HPET calibration path when PIT calibration fails. See `c/kernel/core/ktime.c`.

`sched_become_idle()` enters `cpuidle_enter()` and calls `schedule()` after waking. Earlier versions used a fixed halt sequence. The current idle policy selects a firmware MWAIT hint on Intel CPUs after checking MONITOR/MWAIT, extension and interrupt-break support, ARAT, and the reported substates. Accepted hints have exit latency at most 200 µs. ARAT is required to keep the LAPIC timer running through idle states. The fallback executes `sti; hlt; cli`. `sched_poll_wait()` also uses this policy. See `c/kernel/cpu/power/cpuidle.c` and `c/kernel/cpu/power/hardware.c`.

CPU accounting measures elapsed dispatched time. On timer entries eligible for scheduling, `sched_cpu_tick_check()` folds the current user thread’s accounting and checks the CPU budget for that thread. It latches the first budget crossing and returns the process ID. The interrupt handler posts `LOGIT_SIGXCPU` after the scheduler lock has been released, avoiding recursive lock acquisition through signal wakeup. See `c/kernel/sched/sched.c` and `c/kernel/cpu/irq/interrupts.c`.

## Scheduler observation

The observer records wake attempts, timeout wakes, context switches, interrupts, and the interval from becoming runnable to starting execution. `runnable_since_ns` supplies the start of that interval when its timestamp belongs to the current run. Switch records pair an outgoing record with an incoming record written on the destination thread’s stack, including first dispatch through the bootstraps. See `c/kernel/sched/sched.c` and `c/kernel/sched/observe/flight_recorder.c`.

The flight recorder allocates 65536 slots per observed CPU. Writers disable interrupts while committing records. Atomic payload words and an odd/even version protocol let readers accept a copy whose completed version remains stable across copying. Reads report overwritten ranges and retries. Lock hooks measure intervals opened through `spin_lock_irqsave()`, including time waiting for the lock, and subtract recorded observer cost. See `c/kernel/sched/observe/flight_recorder.c` and `c/kernel/cpu/spinlock.c`.

`SYS_SCHED_OBSERVE`, numbered 224, dispatches to `observe_syscall()`. Request layouts and record streams are defined in `include/abi/logit_sched_observe.h`.

| Operation | Behavior |
|---|---|
| `SCHED_OBSERVE_BEGIN` | Root starts a run after validation and allocation of recorder, input, and group pools. |
| `SCHED_OBSERVE_STOP` | Closes input admission and advances draining and CPU acknowledgement; returns 1 while shutdown is pending. |
| `SCHED_OBSERVE_STATUS` | Copies lifecycle state, outcome counts, loss counters, and timing and output counters. |
| `SCHED_OBSERVE_READ` | Exports trace, input, group, or metadata records using a cursor supplied by the caller, with at most 64 records per request. |

The input ledger API fixes response and outcome deadlines at arrival plus two and three frame periods. The group archive preserves ordered input membership through immutable leaf, join, and routing nodes. These implementations are in `c/kernel/sched/observe/input_ledger.c` and `c/kernel/sched/observe/coalesced_inputs.c`.

Scheduler entry, interrupt exit, and periodic housekeeping advance shutdown. Each producer CPU seals its own ring. Reclamation waits for CPU acknowledgements, confirmation of every stream’s export, and the departure of active readers. An empty read with `SCHED_READ_CONFIRM_EXPORT` and a cursor one past the final record confirms an export. This keeps records and group evidence available through copying. See `c/kernel/sched/observe/observe_control.c`.

## Testing

Run these targets from the repository root:

```sh
make test-mk-wired
make test-sched
make test-wait-smp
make test-thread
make test-thread-negctl
```

The table describes harness checks.

| Target | Coverage |
|---|---|
| `test-mk-wired` | Checks reachability of test fragments through Makefile includes, with declared exceptions. |
| `test-sched` | Boots QEMU with two CPUs; checks weighted execution ratios, priority APIs, and nice preservation across image replacement. |
| `test-wait-smp` | Requires the kernel wait self-test to pass with one, two, and four CPUs. |
| `test-thread` | Checks concurrency across user threads, TLS, synchronization, join, detach, and descriptor reuse with four CPUs. |
| `test-thread-negctl` | Runs six controls on user threads and the guard page test it depends on. |

`test-sched` depends on `test-sched-negctl`. The control kernel defines `SCHED_IGNORE_WEIGHT`, preserving reported nice values and weights while assigning every thread the same runtime multiplier. Its harness requires the case with equal weights to pass and the cases with unequal weights to fail with CPU ratios between 0.85 and 1.15. It expects `SCHED-RESULT 4/6`: one passing ratio case and three passing API checks. See `tests/sched.mk`, `tests/schedneg.mk`, and `tests/boot/run-sched-negctl.sh`.

The positive guest test uses nice pairs 0/0, 0/10, and −10/10, corresponding to weight ratios 1:1, 2:1, and 4:1. It checks CPU-time ratios within 5% and completed-work ratios within 25%. The work counter is maintained in user code, providing a second measurement alongside scheduler accounting. The remaining checks cover an unsupported priority selector, a missing PID, and a relative nice change. The harness requires `SCHED-RESULT 6/6` and checks nice preservation through image replacement. See `tests/guest/sched/schedtest.c` and `tests/boot/run-sched-test.sh`.

The wait self-test covers waking one waiter in FIFO order, waking all waiters, semaphores, mutex exclusion, deferred work, and dispatch counts during a wait. It requires zero dispatches for the parked sleeper during the measurement interval and positive predicate rechecks for the polling baseline. Its success marker is `WAITQ_SELFTEST_OK`. See `c/kernel/diag/wait_selftest.c` and `tests/boot/run-wait-smp.sh`.

The positive guest targets depend on both the kernel ISO and application disk. `make build/disk.img` rebuilds user programs on that disk; the default `make` target builds the ISO. Definitions are in `Makefile`, `tests/sched.mk`, `tests/smpstorm.mk`, and `tests/thread.mk`.

The [heterogeneous scheduler design](../superpowers/specs/2026-11-01-hetero-interactive-sched-design.md) proposes per-CPU EEVDF queues, capacity accounting, interaction chains carrying frame deadlines, and typed CPU leases. Its merged S0a portion supplies the flight recorder, input ledger, group archive, and observation entry point described above.

## Limitations

- Kernel execution can delay timer scheduling and checks of the CPU budget until a return or voluntary scheduling point. See `c/kernel/cpu/irq/interrupts.c`.
- Selection and deadline expiry use linear scans under one shared scheduler lock. Scheduling weights and CPU limits belong to threads; scheduling budgets for whole processes and priority inheritance for sleeping locks are absent. See `c/kernel/sched/sched.c` and `c/kernel/sync/wait.c`.
- Scheduler wakeups send no general reschedule IPI. Idle CPUs can wait for another interrupt; compositor work has its own kick mechanism. See `c/kernel/cpu/irq/interrupts.c` and `c/kernel/cpu/smp/smp.c`.
- Failed LAPIC calibration leaves APs using fixed reload counts whose periods depend on the timer rate. See `c/kernel/cpu/smp/smp.c`.
- `sched_cpu_ns_proc()` sums live threads, using other threads’ stored totals. Exited siblings’ final totals are omitted. See `c/kernel/sched/sched.c`.
- `sched_poll_wait()` enters the idle policy while retaining runnable status, so its caller can repeatedly recheck a condition. See `c/kernel/sched/sched.c`.
- Priority results overlap error values in `SYS_SCHED`. Libc interprets nice −1 and −3 as errors when reading priority, and successful updates returning −1, −2, or −3 as failures. See `include/abi/logit_abi.h` and `c/apps/libc/src/resource.c`.
- GUI input producers and frame receipts await observer integration. `SYS_GUI_RECEIPT` is reserved at 225; probe and model commands return `SCHED_OBSERVE_ENOSYS` without providers. Baseline identifiers label runs without selecting a GUI waiting implementation. See `c/kernel/exec/syscall.c` and `c/kernel/sched/observe/observe_control.c`.
- Observer timing covers instrumented lock paths and C interrupt entry after register and FP-state saving. Bare interrupt masking and assembly intervals fall outside that coverage; NMI and machine check entries are counted. Metadata reports unknown TSC offset and uncertainty, and names pending hardware verification. See `c/kernel/sched/observe/flight_recorder.c` and `c/kernel/sched/observe/observe_control.c`.
- The optional observer test fragment referenced by `Makefile` and the guest fixture included conditionally by `c/kernel/sched/observe/observe_control.c` are absent from this checkout.

## See also

- [Synchronisation](synchronization.md)
- [Process management](processes.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Multiprocessor support](../architecture/smp.md)
- [Writing tests](../contributing/writing-tests.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/sched/sched.c` | Thread state, run ring, weighted selection, accounting, parking, wakeup, and switching. |
| `c/kernel/sched/sched.h` | Scheduler interfaces, priority bounds, design rationale, and blocking contract. |
| `c/kernel/sync/wait.h`, `c/kernel/sync/wait.c` | Wait queues, predicate loops, sleeping locks, and tick deadlines. |
| `c/kernel/cpu/smp/percpu.h` | CPU scheduling state and CPU-count ceiling. |
| `c/kernel/cpu/smp/smp.c` | AP scheduler entry, timer periods, and compositor CPU kicks. |
| `c/kernel/cpu/irq/lapic.c` | LAPIC timer calibration and reload conversion. |
| `c/kernel/cpu/power/cpuidle.c`, `c/kernel/cpu/power/hardware.c` | Idle policy, firmware hint validation, and halt/MWAIT sequences. |
| `c/kernel/cpu/spinlock.c` | Object ticket locks and observer timing hooks. |
| `c/kernel/cpu/irq/interrupts.c` | Timer scheduling, deadline expiry, CPU budget signals, and entry depth restoration. |
| `c/kernel/core/ktime.c` | Accounting clock and TSC calibration paths. |
| `c/drivers/timer/pit.h` | Tick rate and timer interfaces. |
| `c/kernel/gui/wm.c` | Scheduler initialization and AP readiness publication. |
| `c/kernel/sched/observe/flight_recorder.c` | Event rings, stable reads, switch records, and timing hooks. |
| `c/kernel/sched/observe/input_ledger.c` | Input records, deadlines, outcomes, and expiry. |
| `c/kernel/sched/observe/coalesced_inputs.c` | Immutable input groups and retained membership evidence. |
| `c/kernel/sched/observe/observe_control.c` | Observation syscall, lifecycle, providers, exports, and reclamation. |
| `include/abi/logit_sched_observe.h` | Observation requests, records, commands, and export protocol. |
| `c/boot/switch.asm`, `c/boot/isr.asm` | Context switching and interrupt register preservation. |
| `c/kernel/exec/syscall.c` | Yield, priority, and observation syscall dispatch. |
| `c/kernel/exec/signal/ksignal.c` | Signal wakeup through thread IDs. |
| `include/abi/logit_abi.h` | Priority operations, result codes, and syscall numbers. |
| `c/apps/libc/src/resource.c` | Priority and resource-accounting wrappers. |
| `c/kernel/diag/wait_selftest.c` | Wait, synchronization, and parked-thread dispatch checks. |
| `tests/guest/sched/schedtest.c` | Workloads with weighted shares, ratio bounds, and priority API checks. |
| `tests/boot/run-sched-test.sh`, `tests/boot/run-sched-negctl.sh` | Positive scheduler checks and required control failure pattern. |
| `tests/boot/run-wait-smp.sh` | Wait self-test across CPU counts. |
| `tests/guest/sched/thrtest.c`, `tests/boot/run-thread-test.sh` | Workloads on user threads and a harness that uses four CPUs. |
| `tests/sched.mk`, `tests/schedneg.mk` | Scheduler targets and control kernel build. |
| `tests/thread.mk`, `tests/smpstorm.mk` | Thread targets, the guard test dependency, and the SMP wait target. |
| `tools/mk_wired.py` | Checks that test fragments are reachable. |
| `Makefile` | Build outputs, fragment inclusion, and wiring-check target. |
