# Synchronisation

LogitOS uses ticket spinlocks for short critical sections, wait queues to block threads, and deferred work to run callbacks in the context they need. The central contract joins queue registration to scheduler state: a thread becomes blocked before the scheduler releases the lock handed to it. This page explains that handoff, the sleeping primitives built on it, and the lock orders their callers rely on.

## Execution context

Kernel entry tracks nesting through `in_kernel` in `c/kernel/cpu/irq/interrupts.c`. The timer handler calls the scheduler when the saved entry depth is zero. Blocking operations can switch threads, and a resumed thread can continue on another CPU. Shared objects use subsystem and object locks to coordinate access across CPUs.

Older comments in `c/kernel/sync/wait.h`, `c/kernel/sched/sched.h`, and `c/kernel/sync/work.h` describe a big kernel lock, or BKL, held during kernel execution. The correction dated 2026-09-10 in `CLAUDE.md` records its removal from kernel entry and interrupt dispatch. The current entry path and scheduler implement the model based on object locks.

Choose a primitive according to the operation:

| Requirement | Primitive | Interface |
|---|---|---|
| Protect a short critical section | `spinlock_t` | `c/kernel/cpu/spinlock.h` |
| Coordinate device access across CPUs and local interrupts | `io_lock_t` | `c/drivers/core/io_lock.h` |
| Wait for a condition | `struct waitq`, `wait_event()` | `c/kernel/sync/wait.h` |
| Protect an operation that may sleep | `struct mutex` | `c/kernel/sync/wait.h` |
| Count resources or completions | `struct semaphore` | `c/kernel/sync/wait.h` |
| Wait for a predicate protected by a mutex | `struct condvar` | `c/kernel/sync/wait.h` |
| Allow concurrent readers and exclusive writers | `struct rwlock` | `c/kernel/sync/wait.h` |
| Run a callback in a kernel thread | `struct work` | `c/kernel/sync/work.h` |

## Ticket spinlocks

`spinlock_t` contains ticket and serving counters. `spin_lock()` reserves a ticket with a relaxed atomic increment, then waits until an acquire load of the serving counter matches it. `spin_unlock()` advances the serving counter with release ordering. Tickets order acquisition, while the acquire/release operations order access to protected data (`c/kernel/cpu/spinlock.c`).

The contention loop executes the processor’s pause instruction and calls `tlb_service()`. That function polls for a pending TLB shootdown, performs the flush, and acknowledges it. Polling lets a CPU service shootdowns while waiting with interrupts disabled (`c/kernel/cpu/tlb.c`).

| API | Behaviour |
|---|---|
| `spin_lock()` / `spin_unlock()` | Acquire and release while the caller manages interrupt discipline. |
| `spin_lock_irqsave()` / `spin_unlock_irqrestore()` | Save flags and disable local interrupts before acquisition; release and restore the saved state of the interrupt enable flag. |
| `spin_trylock()` | Return `1` after acquiring the lock or `0` on contention. A failed attempt consumes no ticket. |

Use the pair that saves interrupt flags for a lock shared with an interrupt handler. Disabling local interrupts prevents a handler on the same CPU from entering code that acquires the interrupted thread’s lock. The ticket protocol coordinates contenders on other CPUs. Pair each release with the flags saved at acquisition and release nested locks in reverse order (`c/kernel/cpu/spinlock.h`, `c/kernel/cpu/spinlock.c`).

Acquisition records `owner_cpu` and `owner_ra`. Release checks the owning CPU and detects an advance of the serving counter from a state where it already equals the ticket counter. Serial diagnostics identify these invalid releases near their cause.

The device lock in `c/drivers/core/io_lock.h` uses an atomic held flag. `io_lock_enter()` saves and disables interrupts before acquiring it, and `io_lock_leave()` releases it and restores the saved state. Its contention loop also services TLB requests. `io_lock_try()` performs the atomic acquisition attempt; its caller supplies interrupt protection, as `serial_tx_tick()` does in `c/drivers/char/serial.c`.

## Wait queues and the scheduler handoff

A `struct waitq` contains a spinlock and a FIFO list of `struct waiter` registrations. The wait macros and sleeping primitives store their registrations on the waiting thread’s kernel stack. Queue operations and registration removal use the queue lock, tying registration lifetime to the waiting frame (`c/kernel/sync/wait.h`, `c/kernel/sync/wait.c`).

`wait_event()` follows this loop:

1. Acquire the queue lock with interrupts saved.
2. Evaluate the condition under that lock.
3. If the condition is false, enqueue a waiter and call `sched_block_self_unlock()` with the lock and saved flags.
4. On return, acquire the queue lock again, remove any remaining registration, and recheck the condition.

`block_self()` in `c/kernel/sched/sched.c` completes the transition. While the supplied lock remains held, it acquires `g_sched_lock`, marks the thread `THREAD_BLOCKED`, and removes it from the run ring. It releases the supplied lock, then switches contexts while retaining `g_sched_lock`. The incoming thread releases the scheduler lock after the outgoing stack pointer has been saved. The blocking call returns with the supplied lock released and the interrupt state restored.

This ordering closes the race between checking a condition and parking. A producer publishes the state that satisfies the condition and then wakes the queue. A wake reaching the queue after registration waits for its lock; subsequent acquisition of `g_sched_lock` waits for the context switch to finish. A producer that completes before the condition check supplies state for that check to observe. The caller’s synchronization protocol must cover both state publication and condition evaluation.

`waitq_wake_one()` removes the oldest registration, while `waitq_wake_all()` removes all registrations. Both acquire the queue lock themselves and support calls from interrupt context or another CPU. Their return values count threads made runnable. The queue’s `wakes` counter counts registrations removed by these operations (`c/kernel/sync/wait.c`).

Every wake leads to another condition check. Another thread may acquire the resource first, and several predicates may share a queue. Callers include resource availability and any required interruption state in their condition loops.

Use `WAITQ_INIT` for static initialization or `waitq_init()` for fresh storage. Keep the queue and its registrations alive through registration removal.

## Deadlines and asynchronous wakes

`wait_event_timeout()` adds a deadline to the queue protocol. `wait_deadline_ms()` rounds the requested duration up to ticks and adds one tick to account for the partial interval at the start. The macro sets its result to `1` when the condition succeeds and to `0` when the deadline check ends the loop (`c/kernel/sync/wait.h`, `c/kernel/sync/wait.c`).

The boot CPU’s timer path calls `sched_timer_expire()`. Under `g_sched_lock`, that function marks expired sleepers ready and returns them to the run ring. `sched_sleep_ms()` uses the same deadline mechanism with a queue and a loop that checks elapsed ticks (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/sched/sched.c`, `c/kernel/sync/wait.c`).

Signals and process teardown also wake threads by ID. `sched_wake_id()` performs lookup and wake within one hold of `g_sched_lock`. If it finds a live target that is already runnable, it records `wake_pending`. The next blocking decision consumes that flag and returns so the caller can check its state again. This preserves an asynchronous event arriving between a condition check and parking (`c/kernel/sched/sched.c`). Callers include `c/kernel/exec/signal/ksignal.c` and `c/kernel/sched/uthread.c`.

## Sleeping locks

The primitives in `c/kernel/sync/wait.c` use short holds of an internal queue spinlock for bookkeeping. Mutex ownership spans the protected operation while that internal spinlock is free.

| Primitive | State and wake policy |
|---|---|
| Mutex | `mutex_lock()` waits for `locked` to clear and records the current thread as owner. `mutex_unlock()` clears ownership and wakes one waiter. The awakened thread competes for acquisition. |
| Semaphore | `sem_wait()` consumes a positive count. `sem_post()` increments the count and wakes one waiter, providing a completion operation usable by interrupt handlers. |
| Condition variable | `cv_wait()` registers on the condition queue, releases the caller’s mutex, and parks. It acquires the mutex again before returning. |
| Reader/writer lock | Readers enter while `writer` and `waiting_w` are clear. Writers increment `waiting_w` before waiting, causing new readers to defer. Both groups check their counters under one queue lock. |

For a condition variable, the caller checks its predicate in a loop while holding the mutex. `cv_wait()` registers before releasing that mutex, and the condition queue lock covers the transition into the scheduler. A producer can update the predicate under the mutex and signal the condition queue after the waiter has registered.

The reader/writer lock removes queued registrations when the last reader leaves or a writer releases ownership. It then wakes those threads after releasing the queue lock. Each awakened thread checks its acquisition predicate again.

## Deferred work

`c/kernel/sync/work.c` provides three execution tiers:

| Tier | Execution and coordination |
|---|---|
| Softirq | `softirq_raise()` atomically sets a pending bit for the current CPU. `softirq_run_pending()` invokes registered handlers on that CPU. Repeated raises of the same pending bit coalesce. |
| Tasklet | `tasklet_schedule()` adds an item supplied by the caller to a global list and raises `SOFTIRQ_TASKLET`. A `running` flag serialises invocations of the same tasklet across CPUs. |
| Work item | `work_queue()` appends an item supplied by the caller to the FIFO consumed by the `kworker` kernel thread. Its callback can use sleeping primitives and blocking I/O. |

`c/kernel/cpu/irq/interrupts.c` calls `softirq_run_pending()` on the exit path of an entry whose saved nesting depth is zero. Softirq processing has a budget of eight rounds per invocation. Each tasklet action has a budget of 64 callbacks and raises the tasklet softirq again if queued work remains after exhausting that budget.

`work_init()` registers the tasklet handler and requests creation of `kworker`. The worker uses `work_q.lock` for both its FIFO and the condition checked while waiting for work. Submission publishes an item under that lock, releases it, and wakes the worker. The worker removes an item under the lock, then invokes its callback after releasing it.

An item’s `queued` flag clears when the worker removes it. Another invocation can be queued while its callback executes; the single worker processes that invocation after the current callback returns. `work_queue()` returns `1` for a new queue entry and `0` when an entry is already pending. Keep tasklets, work items, and callback arguments alive through pending and executing invocations (`c/kernel/sync/work.h`, `c/kernel/sync/work.c`).

The ACPI adapter in `c/drivers/power/acpi/native.c` illustrates this pattern. Submission adds a job to a preallocated ring under a spinlock and queues a work item. The callback drains jobs and invokes their handlers after releasing the ring’s lock.

## Lock ordering

At a blocking handoff, run in a schedulable thread and retain the spinlock handed to the scheduler. Release other spinlocks before that point. Evaluate the waiting condition under the supplied lock. For a predicate protected by a mutex, use the condition-variable protocol (`c/kernel/sync/wait.h`).

A waker may hold a subsystem lock while taking a queue lock and then the scheduler lock. The sleeper releases its other spinlocks before handing over the queue lock. These paths establish the following orders:

| Path | Lock order | Reason |
|---|---|---|
| Queue wake or blocking handoff | Queue lock → `g_sched_lock` | Registration and scheduler state participate in one blocking protocol. |
| Condition-variable wait | `c->wq.lock` → `m->wq.lock` | `cv_wait()` registers before releasing mutex ownership. |
| Poll wake hook | Object queue lock → poll table lock → `g_sched_lock` | The waker publishes the poll flag under the lock used for parking. |
| Child wait predicate | `g_child_wq.lock` → `g_proc_lock` | `have_zombie()` reads the process table during condition evaluation. |
| Thread join predicate | `g_join_wq.lock` → `g_ut_lock` | `join_ready()` reads the thread table during condition evaluation. |
| Network wake | `net_lock()` → queue lock → `g_sched_lock` | Receive processing publishes state and wakes readers while holding the network lock. |
| Heap growth | `kheap_lock` → `pmm_lock` | Heap growth obtains physical frames while holding the heap lock. |
| Heap magazine draining | `kheap_lock` → magazine lock | Draining returns cached blocks to the heap under its global lock. |

Process exit releases `g_proc_lock` before waking child waiters. Thread exit releases `g_ut_lock` before waking joiners. These choices preserve the orders imposed by their predicates (`c/kernel/exec/proc.c`, `c/kernel/sched/uthread.c`).

Heap magazine allocation and free paths release their magazine lock before returning or continuing to the global heap path. Heap allocation also releases `kheap_lock` before calling the handler that runs when memory is short, which can inspect process and memory state. Observer allocation uses `kmalloc_quiet()` to suppress logging of allocation shortages and the callback for those shortages (`c/kernel/mm/phys/kheap.c`, `c/kernel/mm/phys/pmm.c`, `c/kernel/sched/observe/observe_platform.c`).

Predicates and callbacks belong in a review of lock order because they can acquire further locks. Two callers show why.

In `c/net/transport/tcp.c`, `tcp_wait_readable()` evaluates scalar connection state under the receive queue lock and calls `tcp_available()` after leaving the wait. The source preserves the earlier predicate beside its correction: calling `tcp_available()` inside the predicate acquired `net_lock()` beneath the queue lock, reversing the receive path’s order. The network locking contract appears in `c/net/core/netlock.c`.

For polling, a backend calls `poll_wait()` before reading readiness. A waker holding the object queue lock acquires the poll table lock, sets `triggered`, and wakes the thread. Registration and removal acquire object queue locks with the poll table lock free. Pollable backends must use `waitq_wake_all()`: a poll registration can consume a wake even though the resource is still available to a reader (`c/kernel/exec/fd/kpoll.h`, `c/kernel/exec/fd/kpoll.c`, `c/kernel/sync/wait.c`).

## Device access and visibility

Serial diagnostics acquire the transmit ring’s `io_lock_t` beneath their callers’ locks. `serial_putc()` queues a byte and writes a burst when the UART has room. The lock protects queue state together with the UART status check and write transaction. Timer interrupts on every CPU call `serial_tx_tick()`, which tries the ring lock and drains at most one burst. Diagnostic callers can enqueue bytes and continue while the UART transmits (`c/drivers/char/serial.c`, `c/kernel/cpu/irq/interrupts.c`).

The Polaris self-test uses an ownership lease to govern device access. Acquisition checks binding to the AMD framebuffer driver used at boot. Guard callbacks check the held lease before forwarding register access, mapping resolution, and synchronization. Setup of the memory controller checks the lease before the native platform is bound. Quiesce checks a configured SDMA ring, attempts restoration of the saved values in the memory controller when ring idleness permits it, and retires the lease. Device removal also retires a held lease (`c/drivers/gpu/amd/bootfb.c`, `c/drivers/gpu/amd/gpu_lease.c`, `c/drivers/gpu/amd/polaris/selftest/selftest.c`).

For visibility between the CPU and GPU, the native adapter surrounds MMIO access with sequentially consistent hardware fences. Its synchronization callback flushes posted writes through an HDP register write and readback. Synchronization toward the CPU also invalidates the HDP read cache (`c/drivers/gpu/amd/polaris/native.c`).

## Observing synchronization

The scheduler observer times intervals opened through `spin_lock_irqsave()`. `flight_irq_disabled_begin()` starts before ticket acquisition, so the interval includes contention. `flight_irq_disabled_end()` closes it after release. Nested acquisitions contribute one interval, with time spent recording subtracted from the result (`c/kernel/cpu/spinlock.c`, `c/kernel/sched/observe/flight_recorder.c`).

Before switching stacks, `flight_record_switch()` closes the outgoing CPU’s observed interrupt nesting and the interval it spent with interrupts disabled. The incoming thread records its run start on its own stack. Wake and deadline paths record whether they made a blocked thread runnable; a timestamp taken at that transition supports measurement of its wait for a CPU (`c/kernel/sched/sched.c`).

Each CPU writes its event ring with interrupts disabled. Readers use atomic payload loads and check the expected completed slot version before and after copying. Observer control operations use `struct observe_spinlock`, whose interrupt masking and atomic acquisition bypass the measured spinlock functions (`c/kernel/sched/observe/flight_recorder.c`, `c/kernel/sched/observe/observe_platform.h`).

`sched_timer_expire()` calls observer housekeeping before acquiring `g_sched_lock`. Scheduler entry and interrupt exit also advance the shutdown of an observation run (`c/kernel/sched/sched.c`, `c/kernel/cpu/irq/interrupts.c`, `c/kernel/sched/observe/observe_control.c`).

## Limitations

- Blocking from interrupt context or while retaining another spinlock violates the blocking contract. An idle thread, an unadopted CPU context, or a CPU without an idle successor falls back to releasing the supplied lock, so condition loops can poll (`c/kernel/sync/wait.h`, `c/kernel/sched/sched.c`).
- Mutexes are nonrecursive, and `mutex_unlock()` clears ownership without checking the caller. Sleeping locks have no priority inheritance (`c/kernel/sync/wait.c`, `c/kernel/sched/sched.h`).
- Spinlock release diagnostics continue through release after reporting an invalid operation. Reinitializing a live wait queue resets its ticket lock beneath potential contenders (`c/kernel/cpu/spinlock.c`, `c/kernel/sync/wait.c`).
- Deadlines use `TIMER_HZ`, currently 100 Hz. Interrupt delays, lost ticks, and scheduling can extend elapsed waiting time. `cv_wait_timeout()` returns the scheduler’s result, which reports whether the wake or the timeout ended the wait; callers must recheck their predicate under the mutex (`c/drivers/timer/pit.h`, `c/kernel/sync/wait.c`).
- Softirq and tasklet callbacks must run without sleeping. The work queue has one worker, so waiting for another callback on that queue can stall progress. Its interface provides no cancellation or flush operation (`c/kernel/sync/work.h`, `c/kernel/sync/work.c`).
- Observer timing covers the instrumented spinlock intervals. Bare interrupt masking and assembly paths fall outside those hooks. NMI and machine-check entries contribute counters and bypass event recording and observer shutdown locks (`c/kernel/sched/observe/flight_recorder.c`, `c/kernel/cpu/irq/interrupts.c`).
- The serial ring drops and counts bytes when full. Panic takeover uses a synchronous drain with bounded UART polling under the contract that peer CPUs have stopped (`c/drivers/char/serial.c`).
- The GPU lease uses plain state fields under a contract that boot code uses it from one thread only. Concurrent use requires synchronization. The Polaris self-test is guarded by `LOGIT_HWHUD` in production builds, and lease retirement lasts for the rest of the boot (`c/drivers/gpu/amd/gpu_lease.h`, `c/drivers/gpu/amd/polaris/selftest/selftest.h`).

## Tests in the tree

`tests/unit/wait_test.c` exercises the queue and sleeping primitives with host threads, replacement spinlock functions, and a model scheduler. One control releases the supplied lock before marking the thread parked. Another releases a mutex before condition-queue registration and compares the resulting lost signals with `cv_wait()`.

`tests/unit/poll_test.c` injects an event after readiness sampling and before parking. It drives the queue and poll implementations with a model object and scheduler to check registration order.

`c/kernel/diag/wait_selftest.c` contains guest scheduler checks for queue wake order, semaphore traffic, mutex contention, and a tasklet that queues a sleeping work callback. Its waiting comparison records both polling retries and dispatch counts.

`tests/gpu/amd/polaris/native/test.c` exercises the native adapter with memory representing device BARs and callbacks supplying PCI configuration. `test-polaris-native-host` depends on `test-polaris-native-negctl`, whose mutation removes HDP invalidation and requires the corresponding synchronization assertion to fail (`tests/gpu/amd/polaris/native/test.mk`).

## See also

- [Scheduling](scheduling.md)
- [File descriptors, polling and PTYs](files-and-polling.md)
- [Multiprocessor support](../architecture/smp.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [GPU drivers](../drivers/gpu.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/cpu/spinlock.h` | Ticket lock state, initializer, and interfaces. |
| `c/kernel/cpu/spinlock.c` | Atomic ordering, interrupt masking, release diagnostics, and timing hooks. |
| `c/kernel/cpu/tlb.c` | Shootdown servicing through polling and interrupts. |
| `c/kernel/sync/wait.h` | Queue structures, condition macros, sleeping primitives, and locking contracts. |
| `c/kernel/sync/wait.c` | Queue operations, deadlines, mutexes, semaphores, condition variables, and reader/writer locks. |
| `c/kernel/sync/work.h` | Deferred-work interfaces and callback context requirements. |
| `c/kernel/sync/work.c` | Softirq pending bits, tasklet serialization, callback budgets, and worker FIFO. |
| `c/kernel/sched/sched.h` | Blocking and waking interfaces and a discussion of priority inheritance. |
| `c/kernel/sched/sched.c` | Blocking handoff, run-ring updates, asynchronous wake latch, deadlines, and observation hooks. |
| `c/kernel/cpu/irq/interrupts.c` | Entry nesting, timer scheduling, deadline processing, softirq dispatch, and serial draining. |
| `c/kernel/sched/observe/flight_recorder.c` | Event rings, atomic reader protocol, interrupt timing, and switch observations. |
| `c/kernel/sched/observe/observe_platform.h` | Observer locks and interrupt discipline. |
| `c/kernel/sched/observe/observe_platform.c` | Observer allocation and kernel service adapters. |
| `c/kernel/sched/observe/observe_control.c` | Observation lifecycle and periodic housekeeping. |
| `c/drivers/core/io_lock.h` | Device locks, saved interrupt flags, and TLB servicing during contention. |
| `c/drivers/char/serial.c` | Transmit locking, burst draining, drop accounting, and panic takeover. |
| `c/drivers/timer/pit.h` | Timer frequency, granularity, and an explanation of tick loss. |
| `c/kernel/exec/fd/kpoll.h` | Backend registration and wake contracts. |
| `c/kernel/exec/fd/kpoll.c` | Poll registration, removal, flag locking, and blocking. |
| `c/kernel/exec/proc.c` | Predicate for waiting on a child and wake ordering at process exit. |
| `c/kernel/exec/signal/ksignal.c` | Signal publication and thread wakes. |
| `c/kernel/sched/uthread.c` | Join predicates, wake ordering at exit, and process-wide thread wakes. |
| `c/net/core/netlock.c` | Network locking contract and wake order. |
| `c/net/transport/tcp.c` | Connection waits and the predicate lock-order correction. |
| `c/kernel/mm/phys/kheap.c` | Heap and magazine locking, growth, and handling of allocation shortages. |
| `c/kernel/mm/phys/pmm.c` | Physical allocator locking. |
| `c/drivers/power/acpi/native.c` | ACPI jobs submitted to the shared worker. |
| `c/drivers/gpu/amd/gpu_lease.h` | Lease states and the contract for use at boot. |
| `c/drivers/gpu/amd/gpu_lease.c` | Lease acquisition, holder checks, and retirement. |
| `c/drivers/gpu/amd/bootfb.c` | Binding checks and self-test lease adapters. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.h` | Self-test interfaces and build guard. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.c` | Lease guards, access to the memory controller, and quiesce ordering. |
| `c/drivers/gpu/amd/polaris/native.c` | MMIO fences and HDP synchronization. |
| `tests/unit/wait_test.c` | Host concurrency checks and controls for lost wakeups. |
| `tests/unit/poll_test.c` | Poll registration and checks for races with readiness. |
| `c/kernel/diag/wait_selftest.c` | Guest synchronization checks and a comparison of waiting costs. |
| `tests/gpu/amd/polaris/native/test.c` | Native adapter checks using modeled device resources. |
| `tests/gpu/amd/polaris/native/test.mk` | Host target and a negative control that removes HDP invalidation. |
| `CLAUDE.md` | Historical concurrency model and BKL-removal correction. |
