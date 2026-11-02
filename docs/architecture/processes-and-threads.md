# Processes and threads

LogitOS represents a process as shared resources and identity, with scheduler threads providing its execution contexts. Threads in one process share an address space, file descriptors, working directory and credentials. Each thread has its own kernel stack, saved execution state and thread local storage (TLS) pointer. Process creation and teardown span `c/kernel/exec/proc.c`, `c/kernel/exec/load/exec.c` and `c/kernel/sched/uthread.c`.

## Ownership and execution

Three records have different responsibilities and lifetimes:

| Record | Responsibility | Lifetime |
|---|---|---|
| `struct proc` | Process identity, address space, file descriptors, shared memory handles, working directory, capabilities and optional GUI association | Process reservation through zombie reaping |
| `struct thread` | Kernel stack, saved stack pointer, address space used for dispatch, scheduling state, CPU accounting and TLS base | Scheduler allocation through deferred cleanup |
| `struct uthread` | Owning PID, scheduler TID, return value, join and detach state, and user stack cleanup information | Registration through join, detached completion or process teardown |

`struct proc` is defined in `c/kernel/exec/proc.h`. The scheduler and user thread records are private to `c/kernel/sched/sched.c` and `c/kernel/sched/uthread.c`.

A user scheduler thread carries its process pointer in its `data` field. `proc_current()` retrieves that pointer through the scheduler. Additional user threads use the same process object, so descriptor table and working directory changes are visible to siblings.

The user thread record retains a completed thread's return value for a later join. Its lifetime can extend beyond the scheduler record and kernel stack. The initial thread acquires its record lazily through `uthread_self()` and is registered as detached.

Kernel threads use the same scheduler. Their construction supplies a kernel entry function and the kernel address space; their bootstrap calls that function and exits the thread if it returns. These paths are in `c/kernel/sched/sched.c`.

## IDs and terminal membership

Process IDs come from a counter starting at 1. Scheduler IDs come from another counter starting at 0 and cover kernel threads, user threads and idle threads.

| Field or interface | Meaning |
|---|---|
| `pid` | Process identity, returned by `SYS_GETPID` |
| `ppid` | Parent process identity; kernel launches can use zero |
| `tid` in `struct proc` | Scheduler ID recorded when the process is first published |
| `SYS_THREAD_SELF` | Calling user thread's scheduler ID |
| `sid` | Terminal session membership |
| `pgid` | Process group membership |
| `ctty_id` | Controlling PTY identity; zero means unattached |

These fields are maintained by `c/kernel/exec/proc.c` and `c/kernel/sched/sched.c`. Additional threads share the process PID and receive their own scheduler IDs. The process record retains the TID assigned at first publication.

Allocation initializes the session and process group IDs to the new PID. Fork copies the parent's session, group and controlling terminal identity. `proc_setsid()` creates a session and group for a caller whose group ID differs from its PID, and clears its controlling terminal association. `proc_setpgid()` updates membership for the caller or an eligible child within the same session.

Terminal membership belongs to the process table. The desktop login credential described below belongs to the VFS credential subsystem.

## Process creation and publication

Process allocation reserves a table slot, assigns a PID, and initializes resource tables, identity fields and signal state.

| State | Meaning |
|---|---|
| `PROC_FREE` | Slot available for allocation |
| `PROC_BUILDING` | Reserved while creation prepares resources |
| `PROC_RUNNING` | Published process whose threads may be runnable or blocked |
| `PROC_ZOMBIE` | Exited process retaining its exit status and address space for reaping |

`proc_create()` returns a building process. The caller prepares its address space, descriptors and launch identity before creating the first scheduler thread. The scheduler calls `proc_publish()` before enqueueing that thread. This ordering prevents process table readers from observing a launch record whose resources are still being initialized.

On construction failure, `proc_abort_build()` closes acquired descriptors and shared memory handles, releases signal state and returns the slot to the free table. The caller releases the unpublished address space. These ownership rules are implemented in `c/kernel/exec/proc.c`.

Launch paths in `c/kernel/exec/load/exec.c` load an image into a fresh address space, build its initial stack and create a user thread. `proc_spawn()` connects descriptors 0, 1 and 2 to the serial console when the terminal description is allocated successfully. `proc_spawn_service()` marks a service launched by the kernel as a credential root before publication.

## Fork and image replacement

`proc_fork()` creates a child with a new PID and address space. It copies the working directory, terminal membership and capabilities, clones shared memory handles, and duplicates references to the parent's open file descriptions. Parent and child share description state, including file offsets. Descriptor cloning is in `c/kernel/exec/proc.c`, shared memory handle cloning is in `c/kernel/exec/shmfd.c`, and the open file record is defined in `c/kernel/exec/fd/file.h`.

Address space cloning uses `vmm_clone_user_counted()` in `c/kernel/mm/virt/vmm.c`. With copy on write enabled, eligible frames are shared and writable private pages become copy on write pages. Shared memory mappings retain their shared backing.

`thread_fork()` constructs one child execution context from the calling thread's syscall frame. It copies the saved user FP/SSE state and TLS base, and sets the child's return register to zero. The parent receives the child PID. `ksig_proc_fork()` copies signal dispositions and the blocked mask; the child's pending set and alarm start clear. These paths are in `c/kernel/sched/sched.c` and `c/kernel/exec/signal/ksignal.c`.

`proc_execve()` replaces the program image while retaining the process PID, parent identity, descriptor table, working directory and capabilities. It copies the path, arguments and environment into kernel memory, checks executable access, and builds the replacement image and stack in a new address space. Once preparation succeeds, it commits the replacement, frees the old space, installs the new TLS pointer and rewrites the syscall return frame. Keeping the old space through preparation allows loading failures to return to the existing image.

`uthread_exec_begin()` coordinates exec admission with thread creation, and `uthread_exec_end()` clears the admission flag. After successful image replacement, the syscall dispatcher resets caught signal handlers to their default dispositions while preserving ignored dispositions and the blocked mask. See `c/kernel/exec/load/exec.c`, `c/kernel/exec/syscall.c` and `c/kernel/sched/uthread.c`.

## Creating and completing user threads

`SYS_THREAD_CREATE` accepts a `struct logit_thread_spec` containing the entry address, stack bounds, argument and TLS pointer. The ABI is defined in `include/abi/logit_abi.h`.

The kernel checks the specification and writes the argument and TLS pointer onto the initial user stack. It reserves a user thread record, then allocates a scheduler thread using the process's address space and process pointer. A publication callback installs the TID before the thread is enqueued. The reservation counts as a live owner during allocation, coordinating creation with exec admission and final process teardown.

The C library's `pthread_create()` prepares one mapping containing the requested guard region, stack, TLS storage and thread control block. A nonzero guard is protected against access. The assembly trampoline installs the TLS pointer with `SYS_SET_TLS` before calling the C thread body. See `c/apps/libc/src/pthread.c` and `c/apps/libc/src/pthread_entry.asm`.

`SYS_THREAD_EXIT` ends the calling thread. `uthread_release_self()` completes it in stages:

1. Record the return value and mark the thread as exiting.
2. Unmap the user stack region whose cleanup ownership was passed to the kernel.
3. Publish completion, retaining a joinable record or releasing a detached record.
4. Elect the owner of final process teardown when the live owner count reaches zero.

User stack cleanup runs on the departing thread's kernel stack. Joined and detached threads follow the same cleanup path. The initial process stack is released with the address space.

`SYS_THREAD_JOIN` claims a joinable target in the same process and waits for completion. A successful join consumes the record and copies out its saved return value. `SYS_THREAD_DETACH` arranges record release at completion, or releases an already completed record. When the last user thread exits through `SYS_THREAD_EXIT`, it proceeds to `proc_exit(0)`. These paths are in `c/kernel/sched/uthread.c`.

## User and kernel transitions

New user threads start through `ring3_bootstrap` in `c/boot/enter_user.asm`. It releases the scheduler lock and calls `enter_user`, which constructs a return frame and enters ring 3 with `iretq`. Forked children use `fork_ret` to restore their copied FP/SSE and register state.

Before dispatching a user thread, the scheduler installs the top of its kernel stack in the current core's TSS, switches address spaces as needed and restores its `%fs` base. TLS restoration has its own comparison because siblings sharing an address space can have different TLS pointers. See `c/kernel/sched/sched.c`.

System calls enter through `int 0x80`. `c/kernel/cpu/irq/idt.c` installs an interrupt gate for that vector with permission for user mode to invoke it. `c/boot/isr.asm` saves general registers and FP/SSE state on the thread's kernel stack before calling the C handler, then restores them before `iretq`.

`c/kernel/cpu/irq/interrupts.c` dispatches syscalls and invokes signal delivery on outer returns to user mode, including returns from timer interrupts. Signal dispositions, the asynchronous pending set and the blocked mask belong to the process in `c/kernel/exec/signal/ksignal.c`. Synchronous fault records identify the faulting thread, as defined in `c/kernel/exec/signal/ksig_int.h`.

The helpers `user_copy_from()` and `user_copy_to()` copy through pinned physical pages while holding the address space guard for each page. This coordinates copies with mapping changes made by siblings and with copy on write. Their implementation is in `c/kernel/exec/usercopy.c`.

## Scheduling and waiting

The scheduler selects runnable threads from a shared ring by the smallest accumulated virtual time. CPU time advances virtual time at a rate determined by the thread's scheduling weight. Each core tracks its current thread, and selection skips threads running on another core. Siblings can execute concurrently across cores. The selection and accounting code is in `c/kernel/sched/sched.c`.

Blocking removes a thread from the runnable ring when other members remain. A sole member stays linked as a blocked enqueue anchor, and the picker skips it. Child waiting and thread joining each use a shared completion queue whose waiters recheck their predicates after waking. This allows unrelated completions and spurious wakeups to share a queue. See `c/kernel/exec/proc.c`, `c/kernel/sched/uthread.c` and `c/kernel/sync/wait.h`.

Futex operations compare a user word and park the caller. `c/kernel/sched/uthread.c` derives the waiter key from the caller's address space and user virtual address. It pins the comparison word and checks it while holding the bucket lock used by the wake side. Enrollment and parking use the same locking sequence, closing the interval in which a wake could otherwise be lost.

Process table state, descriptor tables, user thread records and scheduler state have their own locks. Process teardown closes resources and frees address spaces outside the process table lock. Kernel entry tracks nesting depth, which suppresses timer preemption during an active kernel entry; blocking operations can still switch threads.

Each core's idle thread lives outside the shared runnable ring. The idle loop and `sched_poll_wait()` call `cpuidle_enter()`. The policy in `c/kernel/cpu/power/cpuidle.c` selects MWAIT on Intel CPUs when the required CPUID features and a supported firmware hint are available. It requires an always running local APIC timer to preserve the timer wake source, and falls back to HLT when its checks fail.

## Scheduler observation

During an observation run, hooks in `c/kernel/sched/sched.c` record wake attempts, timer expiry and context switches through `c/kernel/sched/observe/flight_recorder.c`. The recorder stores events in fixed rings for participating CPUs.

The `runnable_since_ns` field records when a thread becomes runnable during recording. The incoming thread records its run start on its own stack after the context switch, so the measured wait includes the switch to execution. The recorder represents waits whose starting timestamp predates the run as unknown.

`SYS_SCHED_OBSERVE` (224) exposes begin, stop, status and paged read operations through `include/abi/logit_sched_observe.h`. Run control in `c/kernel/sched/observe/observe_control.c` allocates recording storage before publishing a run. Shutdown drains pending work and collects acknowledgements as CPUs seal their rings. Scheduler entry, timer processing and interrupt exit advance that shutdown.

Caller identity comes from `c/kernel/sched/observe/observe_platform.c`. Starting or stopping a run requires UID zero. Reading records and status requires UID zero or a match of both UID and PID with the owner recorded when the run began.

## Process exit and reaping

`SYS_EXIT` requests termination of the whole process through `proc_exit()`. It records an exit request, preserves the first recorded exit code and wakes registered sibling threads. Each departing thread releases its own user thread state. The final departing owner performs shared resource teardown.

That owner closes file descriptors and shared memory handles, releases owned sockets, marks the associated GUI application dead, and retires signal and tracing state. It then calls `sched_detach_current_proc()` to clear its process pointer and switch to the kernel address space before publishing `PROC_ZOMBIE`.

A parent on another core can reap a zombie immediately. Detaching before publication protects the outgoing thread while it finishes on its kernel stack. Scheduler cleanup frees that stack after the thread switches away. These ownership transitions are in `c/kernel/exec/proc.c`, `c/kernel/sched/uthread.c` and `c/kernel/sched/sched.c`.

`proc_waitpid()` collects a matching zombie child. It claims the process table slot under the process lock and frees the address space after releasing the lock. It accepts a particular child PID or `-1` for any child. With `WNOHANG`, it returns zero while matching children exist and none is a zombie. Blocking waits can return when interrupted by a signal.

Exit publishes the zombie before posting `LOGIT_SIGCHLD` and waking child waiters, allowing a notified parent to collect the exit status immediately. `proc_reap()`, called from the window manager loop, collects zombies whose parent value is zero or whose parent entry has disappeared. See `c/kernel/exec/proc.c` and `c/kernel/gui/wm.c`.

## Credentials and capabilities

Credentials are resolved through a table keyed by PID in `c/fs/vfs/vfs_cred.c`. Lookup checks for the process's own credential entry, then walks its parent chain. At each step, a credential entry takes precedence over a system service marker; that marker supplies root credentials when an entry is absent. Lookup falls back to the desktop login session UID and GID. Kernel callers without a current process receive root credentials.

Inheritance is evaluated during lookup. Changes to a governing ancestor or the session default affect descendants using that source. Threads share the result because lookup starts with their common PID. The desktop session credential starts with UID and GID zero.

Identity syscalls expose UID, GID and supplementary groups. Effective UID and GID queries return the same values as UID and GID queries. Changing identity or supplementary groups requires UID zero. Session establishment updates the session default and the caller's credential under one lock, allowing login to establish both while dropping privilege.

Capabilities are stored in `struct proc` alongside `fs_prefix`. Allocation initializes an empty capability set, and launch paths supply grants. Fork copies the grant; exec preserves it. `proc_cap_spawn()` checks the requested child grant with `proc_cap_subset()` before creating the child.

The syscall dispatcher checks classified filesystem and networking operations against `CAP_FS` and `CAP_NET`. VFS permission checks use the resolved UID, GID and supplementary groups. See `c/kernel/exec/syscall.c` and `c/fs/vfs/vfs_meta.c`.

## Limitations

- Exec admission refuses more than one live user thread with `THR_E_INVAL`. See `c/kernel/exec/syscall.c` and `c/kernel/sched/uthread.c`.
- Fork creates the calling execution context in the child. Locks held by other threads can remain locked there; `c/kernel/exec/proc.c` documents this hazard.
- Legacy kill marks and process exit requests are checked at syscall entry and inside the signal stop loop. A sibling running a compute loop in user space can delay final teardown. Signal delivery on timer return follows its own path. See `c/kernel/exec/syscall.c`, `c/kernel/exec/signal/ksigframe.c` and `c/kernel/cpu/irq/interrupts.c`.
- Futex keys use the address space and virtual address, so shared memory mappings do not provide futex wakeups across processes. See `c/kernel/sched/uthread.c`.
- `SYS_WAITPID` and libc's `waitpid()` expose raw exit codes, incompatible with POSIX wait status decoding macros. See `c/kernel/exec/proc.c` and `c/apps/libc/src/io.c`.
- The process table has `NPROC` entries, currently 32. User thread records are bounded by `LOGIT_THREADS_MAX`, currently 64 per process, and `UTHREAD_TABLE_MAX`, currently 128 across the machine. Completed joinable records occupy slots until collected. Mapping capacity and allocation failures can impose lower limits. See `c/kernel/exec/proc.h`, `include/abi/logit_abi.h`, `c/kernel/sched/uthread.h`, `c/kernel/sched/uthread.c` and `c/apps/libc/src/pthread.c`.
- `proc_cap_subset()` compares capability bits and prefix strings at path component boundaries. This comparison does not establish containment after symlink resolution. See `c/kernel/exec/proc.c`.

## See also

- [System calls](system-calls.md)
- [Memory model](memory-model.md)
- [Multiprocessor support](smp.md)
- [Process management](../kernel/processes.md)
- [Scheduling](../kernel/scheduling.md)
- [Program loading](../kernel/program-loading.md)
- [Signals](../kernel/signals.md)
- [Security model](security-model.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/proc.h` | Process record, states, capacities and lifecycle interfaces |
| `c/kernel/exec/proc.c` | Allocation, publication, fork, terminal identities, exit, waiting, reaping and capability attenuation |
| `c/kernel/exec/shmfd.c` | Ownership and cloning of shared memory handles |
| `c/kernel/exec/load/exec.c` | Initial launches, capability grants and transactional image replacement |
| `c/kernel/exec/syscall.c` | Lifecycle dispatch, termination checks, exec admission and capability classification |
| `c/kernel/exec/fd/file.h` | Open file descriptions, reference counts and offsets |
| `c/kernel/sched/sched.c` | Scheduler records, IDs, selection, context switching, idle loops and deferred cleanup |
| `c/kernel/sched/uthread.h` | User thread interfaces and machine record capacity |
| `c/kernel/sched/uthread.c` | Thread registration, creation, completion, join, detach, teardown election and futexes |
| `c/kernel/sync/wait.h` | Wait queues and predicates evaluated under queue locks |
| `c/boot/enter_user.asm` | Initial user entry and fork return trampolines |
| `c/boot/isr.asm` | Register and FP/SSE preservation across kernel entry |
| `c/kernel/cpu/irq/idt.c` | Syscall interrupt gate accessible from user mode |
| `c/kernel/cpu/irq/interrupts.c` | Interrupt nesting, syscall dispatch, timer scheduling and signal delivery |
| `c/kernel/exec/usercopy.c` | Guarded copies through pinned user pages |
| `c/kernel/mm/virt/vmm.c` | Address space cloning, copy on write and address space release |
| `c/kernel/exec/signal/ksignal.c` | Process signal state, fork inheritance and exec reset |
| `c/kernel/exec/signal/ksig_int.h` | Signal state definitions and fault records associated with threads |
| `c/kernel/exec/signal/ksigframe.c` | Signal delivery and the signal stop loop |
| `c/fs/vfs/vfs_cred.c` | Credential lookup, supplementary groups and desktop login identity |
| `c/fs/vfs/vfs_meta.c` | Permission checks using credentials |
| `c/apps/libc/src/pthread.c` | User thread mappings, guards, TLS and lifecycle wrappers |
| `c/apps/libc/src/pthread_entry.asm` | TLS installation and entry into the C thread body |
| `c/apps/libc/src/io.c` | Waiting wrappers and raw exit status behavior |
| `c/kernel/gui/wm.c` | GUI exit handling and invocation of orphan reaping |
| `c/kernel/cpu/power/cpuidle.c` | CPU idle policy and timer wake requirements |
| `c/kernel/sched/observe/flight_recorder.c` | Event rings, context switch records and runnable wait measurements |
| `c/kernel/sched/observe/observe_control.c` | Observation run creation, shutdown, export and access checks |
| `c/kernel/sched/observe/observe_platform.c` | Observer access to caller identity, memory, allocation and clocks |
| `include/abi/logit_abi.h` | Process, thread, futex, identity and capability syscall contracts |
| `include/abi/logit_sched_observe.h` | Scheduler observation requests, record streams and run states |