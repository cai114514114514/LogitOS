# Process management

LogitOS represents a process as an address space, shared resources and a set of scheduler threads. `c/kernel/exec/proc.c` manages process identity, construction, fork, exit and collection. `c/kernel/exec/load/exec.c` implements program launch and image replacement. Construction publishes a completed process before its first thread becomes runnable; exit makes the process available for collection after its final thread finishes resource cleanup and detaches from the address space.

## The process record

`struct proc` in `c/kernel/exec/proc.h` holds the identity and resources shared by a process’s threads.

| Fields | Purpose |
|---|---|
| `pid`, `ppid` | Process ID and parent process ID. |
| `sid`, `pgid`, `ctty_id` | Session, process group and controlling PTY identity. |
| `state`, `exit_code` | Lifecycle state and exit status. |
| `tid`, `cr3` | Initial scheduler thread ID and physical address of the page table root. |
| `gui` | Association with a desktop application. |
| `fd`, `fd_lock` | File descriptor table and lock protecting descriptor references. |
| `shm`, `shm_mask` | Shared memory handles, access rights and occupied slots. |
| `cwd`, `name` | Working directory and diagnostic name. |
| `caps`, `fs_prefix` | Capability bitmap and filesystem scope string. |
| `execing`, `teardown` | Coordination of image replacement and final cleanup. |
| `system_service`, `agent` | Service credential marker and AEX agent identity. |

The header defines `NPROC` as 32 process slots, `NFD` as 32 file descriptors per process and `NSHM` as 16 shared memory handles per process.

Scheduler records point to their owning process. `proc_current()` retrieves that pointer through `sched_current_data()`. Each scheduler thread has its own kernel stack, saved execution context and TLS pointer. `c/kernel/sched/uthread.c` maintains additional records for user thread creation, joining, registered user stacks and exit coordination.

VFS credentials live in a table keyed by PID in `c/fs/vfs/vfs_cred.c`. Lookup checks the process’s credential entry, walks its parent chain and falls back to the desktop login credential. A `system_service` marker encountered during that walk supplies the kernel service root credential. Credential setters sweep entries for process records that have disappeared.

## Construction and publication

The process table distinguishes reservation from publication.

| State | Meaning |
|---|---|
| `PROC_FREE` | Slot available for allocation. |
| `PROC_BUILDING` | Slot reserved during construction. |
| `PROC_RUNNING` | Published process; its threads may be running, runnable or blocked. |
| `PROC_ZOMBIE` | Exited process retaining status and an address space for collection. |

`alloc_proc()` reserves a slot under `g_proc_lock`, assigns a PID and resets its metadata and resources. The initial session and process group IDs equal the new PID, the working directory is `/`, and the capability bitmap starts at zero (`c/kernel/exec/proc.c`).

`proc_create()` installs the address space, application association, name and parent ID. The creator prepares resources and authority, then asks the scheduler to construct the initial thread. The scheduler calls `proc_publish()` before adding that thread to the runnable ring. Process inspection and termination can then find the completed record (`c/kernel/sched/sched.c`).

After a failure involving a reserved process, the creator frees the candidate address space and calls `proc_abort_build()`. That function closes descriptors and shared memory handles, releases signal state and returns the slot to `PROC_FREE`. Holding the reservation through cleanup prevents another allocation from receiving resources that the failed creator is still releasing.

The process table lock protects slot ownership and state transitions. Address space destruction, backend file cleanup and scheduling occur outside it because those operations acquire other locks or can wait.

## Fork

`proc_fork()` creates a child that resumes from the calling thread’s saved syscall frame. The parent receives the child PID; the child receives zero (`c/kernel/exec/proc.c`, `c/kernel/sched/sched.c`).

`vmm_clone_user_counted()` clones virtual memory area metadata and walks allocated user page tables. With copy on write enabled, eligible writable private pages become shared mappings protected against writes in both processes. The clone copies private frames when sharing is unavailable. Shared memory mappings retain references to the same frames, and swapped mappings acquire references to their swap slots (`c/kernel/mm/virt/vmm.c`).

The child copies the parent’s session, process group, controlling PTY identity, working directory, name, capability bitmap and filesystem scope string. Its desktop application association starts null. `ksig_proc_fork()` copies signal dispositions and the blocked mask into freshly initialised signal state, so the child begins with an empty pending signal set (`c/kernel/exec/signal/ksignal.c`).

`proc_fd_clone()` acquires references to the same `struct file` objects. This shares open file state, including the file offset described in `c/kernel/exec/fd/file.h`. `proc_shm_clone()` acquires segment references and copies each handle’s access rights (`c/kernel/exec/shmfd.c`).

`thread_fork()` constructs one child scheduler thread. It copies the saved register frame, sets the child return value to zero, inherits the TLS pointer and copies the user floating point state captured at kernel entry. Using that entry snapshot preserves the user’s state across the kernel C code that constructs the child. The bootstrap restores it before returning to user mode (`c/kernel/sched/sched.c`, `c/boot/enter_user.asm`).

Failure paths release the candidate address space and reserved process resources before returning an error.

## Spawn and image replacement

Launch paths prepare the new process’s parent, resources and authority before creating its initial thread.

| Entry point | Process setup |
|---|---|
| `proc_spawn()` | Loads a fresh image with parent PID zero and grants `CAP_ALL`. |
| `proc_spawn_service()` | Uses the kernel spawn path and sets `system_service` before publication; requires a kernel caller. |
| `proc_cap_spawn()` | Creates a child with a checked capability request, the caller’s working directory and cloned file references. |
| Desktop launch in `c/kernel/gui/wm.c` | Associates the process with its application, uses parent PID zero and grants `CAP_ALL`. |
| `proc_agent_spawn()` | Creates an AEX worker with the caller as parent, zero capabilities and a transferred channel descriptor. |

The kernel and desktop launch paths initialise descriptors 0, 1 and 2 with references to one serial console file when allocation succeeds. The window manager launches `/bin/login` through `proc_spawn()` and uses `proc_spawn_service()` to launch `/bin/agentd` (`c/kernel/exec/load/exec.c`, `c/kernel/gui/wm.c`).

For `SYS_CAP_SPAWN`, `proc_cap_spawn()` copies the argument vector and capability request from user memory. `proc_cap_subset()` checks the requested bitmap and prefix string against the caller’s grant. The loader checks executable access, loads the image and constructs its stack. Fresh process allocation supplies the child’s session and process group IDs.

`proc_agent_spawn()` loads a worker from a snapshot of the executable bytes and compares the AEX application ID and image hash with the request. It transfers the channel endpoint before scheduling the child. The image hash is calculated by the parser in `c/kernel/exec/load/aex.c`. See [Agent runtime](../userland/agent-runtime.md) for the worker launch contract.

Image replacement uses the existing process record. `uthread_exec_begin()` coordinates the operation with thread creation. `proc_execve()` builds a candidate address space and argument stack, then commits the new root through `proc_exec_space()`. Preparation failures release the candidate and return an error. After commitment, the loader switches to the new space, frees the old space, installs the new TLS pointer and rewrites the syscall return frame to enter the program (`c/kernel/exec/load/exec.c`).

The successful syscall path calls `ksig_proc_exec()` to reset caught signal handlers to their default dispositions (`c/kernel/exec/syscall.c`, `c/kernel/exec/signal/ksignal.c`). See [Program loading](program-loading.md) for image formats and stack construction.

## Sessions and controlling terminals

Session and process group membership are process table state protected by `g_proc_lock`.

`proc_setsid()` accepts a running caller whose process group ID differs from its PID. It sets both session and process group IDs to the caller’s PID and clears its controlling terminal ID. `proc_setpgid()` changes the caller or its child within the same session. The target must be running and must differ from the session leader. Creating a group uses the target’s PID; joining another group requires a running member of that group in the session (`c/kernel/exec/proc.c`).

PTY objects receive IDs that increase throughout the boot. This gives a newly allocated terminal an identity distinct from earlier allocations, even when memory addresses are reused (`c/kernel/exec/fd/pty.c`).

Attaching a controlling terminal requires a slave endpoint and a session leader. The PTY checks its owning session, and `proc_attach_ctty()` checks that the caller’s session has a compatible terminal assignment. It then assigns the PTY identity to running and zombie process records in that session. The PTY records the session and foreground process group.

Foreground group changes check the caller’s session and controlling terminal identity. The membership check accepts a matching running or zombie process through `proc_group_in_session()`. When the final references to both PTY endpoints close, `pty_release()` clears process records carrying that terminal identity and frees the object.

Both serial console files and PTY endpoints use `F_TTY`. A null backing pointer selects the serial console; a PTY backing object selects the PTY implementation (`c/kernel/exec/fd/file.c`).

Serial console writes acquire the message lock shared with kernel logging for each line or bounded portion of a long line. `CONSOLE_LINE_MAX` limits each group to 160 output bytes after newline expansion to carriage return and newline. A trailing partial line is queued immediately (`c/kernel/exec/fd/file.c`, `c/kernel/diag/kprintf.c`). `serial_putc()` queues bytes and attempts transmission; timer interrupts call `serial_tx_tick()` to continue draining queued output (`c/drivers/char/serial.c`, `c/kernel/cpu/irq/interrupts.c`).

## Exit and resource cleanup

`SYS_EXIT` enters `proc_exit()`. The first process exit request records its code through `uthread_proc_kill()` and wakes registered live sibling threads. Later requests use that recorded code (`c/kernel/exec/syscall.c`, `c/kernel/sched/uthread.c`).

Each departing thread calls `uthread_release_self()`. A registered user stack is unmapped while its thread record still counts as an address space owner. The function then elects the final teardown owner under the user thread lock. Earlier departing threads detach their scheduler records from the process before releasing that lock. This ordering gives the final owner exclusive responsibility for publishing the zombie.

`SYS_THREAD_EXIT` releases the calling thread and invokes `proc_exit(0)` when it wins final teardown. An existing process exit mark supplies the previously recorded exit code.

The elected owner performs these steps in `c/kernel/exec/proc.c`:

1. Releases user thread records and the process exit mark.
2. Closes file descriptors and shared memory handles.
3. Sweeps sockets owned by the process through `sock_close_owner()`.
4. Marks the associated desktop application dead and releases signal and tracing state.
5. Detaches its scheduler record from the process and switches to the kernel address space.
6. Stores the exit code and publishes `PROC_ZOMBIE`.
7. Posts `LOGIT_SIGCHLD` to a parent with a positive PID and wakes child waiters.

Zombie publication follows cleanup and address space detachment because a parent may collect the process immediately after that transition.

Descriptor closure removes the table pointer under `fd_lock`, then calls `file_close()` after unlocking. The final file reference triggers backend cleanup: buffered VFS writeback, socket release, event cleanup, PTY release or pipe endpoint release. References inherited by another process extend the file object’s lifetime (`c/kernel/exec/fd/file.c`).

Shared memory cleanup detaches handles under `fd_lock` and releases segment references afterward. Address space destruction later releases mappings and their backing references (`c/kernel/exec/shmfd.c`, `c/kernel/mm/virt/vmm.c`).

`thread_exit()` retires the scheduler thread and switches to a successor. A later scheduler pass frees the retired thread’s kernel stack and scheduler record (`c/kernel/sched/sched.c`).

## Waiting and reaping

`proc_waitpid()` searches the caller’s children. A positive PID selects one child; `-1` selects any child.

| Options | Behaviour |
|---|---|
| Zero | Waits for a matching child to become a zombie. |
| `WNOHANG` | Returns zero while matching children exist but none is a zombie; writes zero to a supplied status location. |

Collection claims a zombie under `g_proc_lock`, copies its PID, exit code and address space pointer into local variables, and returns the slot to `PROC_FREE`. `vmm_free_space()` runs after unlocking. A search with no matching child returns `-1` (`c/kernel/exec/proc.c`).

Blocking waits use one shared child wait queue. Each waiter checks its own child predicate after waking. Exit wakes the queue after publishing the zombie; a 200 ms timeout supplies another opportunity to check the table. An interrupted wait returns `SIG_E_INTR`, which libc `waitpid()` converts to `-1` with `EINTR` (`c/apps/libc/src/io.c`).

The window manager loop calls `proc_reap()` to collect zombies with parent PID zero or a parent record that has disappeared. It claims each slot under the process lock and frees the address space afterward (`c/kernel/gui/wm.c`, `c/kernel/exec/proc.c`).

Memory pressure can release zombie user mappings earlier through `oom_task_reap_dead()`. It tries to acquire an address space operation guard, then revalidates the PID, slot state and address space pointer before reclaiming mappings. That validation protects against a collector taking the process between the initial snapshot and guard acquisition. Later collection frees the remaining address space structures (`c/kernel/exec/proc.c`, `c/kernel/mm/virt/vmm.c`).

## Termination requests and inspection

Unflagged `SYS_KILL` records a termination mark. `proc_kill_check()` consumes the mark in the victim’s context and calls `proc_exit(137)`. Checks run at syscall entry and inside the signal stop loop. The check protects processes whose parent PID is zero and whose desktop association is null (`c/kernel/exec/proc.c`, `c/kernel/exec/syscall.c`, `c/kernel/exec/signal/ksigframe.c`).

Requests carrying `LOGIT_KILL_SIGNAL` use `ksig_kill()`. Signal delivery runs on the return path to user mode, including returns from timer interrupts. A terminating signal action enters `proc_exit()` (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/exec/signal/ksigframe.c`).

`SYS_PROCS` snapshots running and zombie records one slot at a time under the table lock, then copies each result to user memory after unlocking. Procfs accessors and `proc_snapshot()` also return copies. Descriptor operations use `proc_fd_acquire()` to take a file reference while holding `fd_lock`, preserving the object’s lifetime during concurrent descriptor closure (`c/kernel/exec/proc.c`).

`SYS_SCHED_OBSERVE` provides measurement run control and paged record export through `observe_syscall()`. Scheduler hooks record thread creation, wakeups and switches caused by scheduling, blocking or exit. Incoming threads record their start on their own stacks, allowing the recorder to calculate the interval from becoming runnable to running (`c/kernel/sched/sched.c`, `c/kernel/sched/observe/flight_recorder.c`). Starting and stopping require UID zero; status and record reads accept root or a caller matching the run’s recorded UID and PID (`c/kernel/sched/observe/observe_control.c`).

## Limitations

- Exec with multiple live threads returns `THR_E_INVAL`. Fork creates one child thread, so a lock held by another parent thread stays locked in the child (`c/kernel/exec/syscall.c`, `c/kernel/exec/proc.c`).
- Wait supports a positive child PID or `-1`, and zero options or `WNOHANG`. Other option bits return `SIG_E_NOSYS`. The older description in `c/apps/libc/include/sys/wait.h` documents packed status in the Linux style; `proc_waitpid()` and libc `waitpid()` pass the raw exit code through. `WIFEXITED` and `WEXITSTATUS` expect the packed representation (`c/kernel/exec/proc.c`, `c/apps/libc/src/io.c`).
- Job control covers part of the model. Saved terminal state per login, hangup and continue signals for orphaned groups, the restriction on group changes made by a parent after exec, and enforcement of background terminal I/O policy are absent (`c/kernel/exec/proc.c`).
- Unflagged kill marks wait for a victim check and rely on the victim reaching one. Process exit requests wake registered siblings, but a sibling computing in user space can delay final cleanup until it reaches a teardown check (`c/kernel/exec/proc.c`, `c/kernel/sched/uthread.c`, `c/kernel/exec/syscall.c`).
- Serial console output can lose bytes when the transmit ring fills. The driver counts dropped bytes and queues a loss notice during a later write when space permits (`c/drivers/char/serial.c`).

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/proc.h` | Process record, states, resource limits and lifecycle interfaces. |
| `c/kernel/exec/proc.c` | Construction, publication, fork, sessions, exit, wait, reaping and inspection. |
| `c/kernel/exec/load/exec.c` | Kernel spawn, capability spawn, worker spawn and image replacement. |
| `c/kernel/exec/load/aex.c` | AEX parsing, application identity and image hashing. |
| `c/kernel/exec/syscall.c` | Lifecycle syscall dispatch and teardown checks. |
| `c/kernel/sched/sched.c` | Scheduler thread construction, fork context, detachment and deferred cleanup. |
| `c/boot/enter_user.asm` | Entry to user mode and restoration of forked register state. |
| `c/kernel/sched/uthread.c` | User thread records, exit coordination and final teardown election. |
| `c/kernel/sched/observe/flight_recorder.c` | Scheduler event recording and incoming thread start records. |
| `c/kernel/sched/observe/observe_control.c` | Observation run control, access checks and paged export. |
| `include/abi/logit_sched_observe.h` | Observation commands, requests and exported record formats. |
| `c/kernel/mm/virt/vmm.c` | Address space cloning and destruction. |
| `c/kernel/exec/fd/file.h` | File descriptions and shared open file state. |
| `c/kernel/exec/fd/file.c` | File references, serial console writes and backend cleanup. |
| `c/kernel/exec/fd/pty.c` | PTY lifetime, controlling terminals and foreground groups. |
| `c/kernel/exec/shmfd.c` | Shared memory handle inheritance and cleanup. |
| `c/kernel/exec/signal/ksignal.c` | Signal state lifecycle and inheritance. |
| `c/kernel/exec/signal/ksigframe.c` | Signal delivery, stop loops and termination. |
| `c/kernel/exec/ptrace.c` | Tracing link cleanup during process exit. |
| `c/kernel/cpu/irq/interrupts.c` | Syscall entry, timer handling and signal delivery before returning to user mode. |
| `c/kernel/diag/kprintf.c` | Message lock shared by kernel logging and serial console writes. |
| `c/drivers/char/serial.c` | Serial transmit queue, periodic drain and loss accounting. |
| `c/fs/vfs/vfs_cred.c` | Credentials keyed by PID and parent chain lookup. |
| `c/kernel/gui/wm.c` | Desktop creation, login and service launch, application exit and reaping calls. |
| `c/net/core/sock.c` | Cleanup of sockets owned by an exiting process. |
| `c/apps/libc/src/io.c` | Wait wrappers and raw status handling. |
| `c/apps/libc/include/sys/wait.h` | Wait options and macros expecting packed status. |

## See also

- [Processes and threads](../architecture/processes-and-threads.md)
- [Scheduling](scheduling.md)
- [Program loading](program-loading.md)
- [Signals](signals.md)
- [File descriptors, polling and PTYs](files-and-polling.md)
- [Shared memory](shared-memory.md)
