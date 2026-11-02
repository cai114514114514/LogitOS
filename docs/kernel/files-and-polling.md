# File descriptors, polling and PTYs

LogitOS uses reference counts to share open file descriptions between process descriptors. Files, pipes, terminals, sockets and event counters use this interface, so duplication and inheritance retain access to the same object. Polling combines backend readiness checks with scheduler wait queues. Pseudo-terminals provide terminal input processing, attributes and session controls. The implementation lives in `c/kernel/exec/fd/`, with descriptor ownership managed by `c/kernel/exec/proc.c`.

## Descriptor tables and ownership

Each `struct proc` contains an `fd` array of pointers to `struct file`, protected by `fd_lock`. `proc_fd_alloc()` installs a description in the lowest free slot. `proc_fd_pair()` installs two descriptions while holding that lock, giving pipe and PTY creation an atomic table update. See `c/kernel/exec/proc.h` and `c/kernel/exec/proc.c`.

The descriptor number identifies a table slot. The open file description holds the access mode, status flags, offset and backend state. `dup()`, `dup2()` and inheritance through `proc_fd_clone()` add references to that description. Duplicated file descriptors share their offset and any buffered contents. The fields are defined in `c/kernel/exec/fd/file.h`.

I/O lookup acquires a temporary reference. `proc_fd_acquire()` increments the reference count while holding the descriptor-table lock; the caller releases it with `file_close()`. The `FILE_REF` cleanup annotation releases that reference when its variable leaves scope, including through early returns. This keeps an operation’s description alive while another thread closes or reuses its descriptor slot.

Closing removes the table entry before releasing its reference outside the table lock. The final reference triggers backend cleanup. `file_close()` copies the cleanup state and detaches the description under the file-pool lock. Cleanup then uses those local copies, allowing the pool slot to be reused safely.

## File backends

`c/kernel/exec/fd/file.c` dispatches I/O and readiness checks by description type.

| Type | Backend and state |
|---|---|
| `F_VFS` | Filesystem path, offset and either streamed reads or buffered contents. |
| `F_PIPE` | An endpoint of a shared byte ring, with endpoint counts and a wait queue. |
| `F_TTY` | Serial console when `backing` is null; PTY endpoint when it points to a terminal object. |
| `F_SOCK` | Socket state managed by `c/net/core/lsock.c`. |
| `F_EVENT` | Counter and wait queue used by eventfd and timerfd. |

For an existing file opened with `O_RDONLY` and without `O_TRUNC`, `file_open_vfs()` probes a one-byte read at offset one. A successful probe selects streaming through `vfs_pread()` at the description’s current offset. Synthetic files that require a complete rendering use a buffer. Paths recognised as generated process files take the live streaming path before this probe.

Writable descriptions keep a kernel buffer because the VFS write operation replaces the file contents. Writes update the buffer and mark it dirty. `file_fsync()` and final close write dirty contents through `vfs_write()`. The filesystem interfaces are declared in `c/fs/vfs/vfs.h`.

When final close encounters a failed flush, `proc_fd_close()` returns `-2`. Libc’s `close()` translates that result to `EIO` in `c/apps/libc/src/io.c`.

VFS reads and writes take the description’s sleeping `io_lock`. Pipes, event counters and PTYs protect queue state with their wait-queue locks. Blocking operations hand the relevant lock to the scheduler, allowing producers and consumers to make progress.

## Serial console

For serial-console output, `tty_write()` expands newlines to carriage return plus newline and takes the console message lock shared with `kprintf()`. Each chunk ends at a newline or before its expanded output would exceed `CONSOLE_LINE_MAX`, which is 160 bytes. An expanded carriage-return/newline pair stays within one chunk. A final partial line is queued immediately, allowing prompts to appear before a terminating newline. See `c/kernel/exec/fd/file.c` and `c/kernel/diag/kprintf.c`.

The driver in `c/drivers/char/serial.c` configures COM1 for 115200 baud, eight data bits, no parity and one stop bit. Console output enters a 64 KiB transmit ring through `serial_putc()`. The driver attempts a burst when the UART reports an empty transmitter, using up to 16 bytes when its FIFO capability check succeeds.

Timer interrupts call `serial_tx_tick()` on every core to drain pending output, as wired in `c/kernel/cpu/irq/interrupts.c`. This lets output continue after its producer finishes. `serial_tx_stats()`, declared in `c/drivers/char/serial.h`, exposes accepted byte counts, bytes written to the UART transmit register, dropped bytes and ring occupancy.

Console reads claim the foreground process, obtain a byte through the signal layer, translate carriage return to newline and echo the result. The timer path in `c/kernel/exec/signal/ksignal.c` collects UART input and wakes console pollers after queueing bytes.

## Pipes

A pipe has two open file descriptions sharing an 8 KiB ring. The `is_write` field identifies the endpoint direction. Duplication adds references to an endpoint description; its final close updates the shared reader or writer count. These operations are implemented in `c/kernel/exec/fd/file.c`.

An empty reader sleeps while a writer remains. After the final writer closes, the reader drains queued bytes and then receives EOF. A writer waits for capacity while readers remain. Losing the final reader posts `LOGIT_SIGPIPE` and makes a write fail if it has transferred no bytes. `O_NONBLOCK` turns a wait for data or capacity into a retry result, which libc translates to `EAGAIN`.

Within `pipe_write()`, a request of at most `PIPE_SZ` waits until the entire request fits, then copies it while holding the queue lock. Larger requests progress as space becomes available. Reads, writes and endpoint closure wake the queue used by blocking I/O and polling.

## The polling interface

`SYS_POLL` accepts an array of `struct logit_pollfd`, an entry count and a timeout in milliseconds. Each entry contains a descriptor, requested `events` and returned `revents`. The ABI is defined in `include/abi/logit_abi.h`.

| Timeout | Behavior |
|---|---|
| Negative | Wait for readiness or interruption. |
| Zero | Scan and return immediately. |
| Positive | Wait for readiness, interruption or the deadline. |

The return value counts entries with nonzero `revents`. `LPOLLERR`, `LPOLLHUP` and `LPOLLNVAL` pass through the event filter regardless of the requested mask. A call with zero entries provides a timed or interruptible wait.

`c/kernel/exec/fd/kpollsys.c` validates and copies the user array, resolves descriptors and constructs sources for `poll_core()`. Each resolved description receives a reference that lasts through the wait. The syscall releases those references after the core removes its wait-queue registrations, then copies the results to the caller.

An older comment in that file justified borrowing description pointers. The adjacent correction and current acquire/release loop establish the current rule: references retain descriptions and their backend queues across concurrent descriptor closes.

## Readiness and wakeups

`poll_core()` operates on `struct pollsrc`: an object, a readiness callback and event masks. `file_poll()` dispatches into the file backends. This structure lets `tests/unit/poll_test.c` exercise the polling core with a model object that injects an event between computing readiness and sleeping.

| Backend | Readiness reported by the implementation |
|---|---|
| VFS file | `LPOLLIN | LPOLLOUT`. |
| Pipe reader | Queued bytes produce `LPOLLIN`; final writer closure produces `LPOLLHUP`. |
| Pipe writer | Available space produces `LPOLLOUT`; loss of all readers produces `LPOLLERR`. |
| Serial console | Queued input produces `LPOLLIN`; output reports `LPOLLOUT`. |
| PTY | Queue contents, pending EOF, available capacity and peer endpoint state determine readiness. |
| Event counter | A nonzero counter produces `LPOLLIN`; eventfd also reports `LPOLLOUT`. |
| Socket | UNIX socket and TCP readiness through `lsock_file_poll()`. |

The older socket-polling comment in `include/abi/logit_abi.h` describes support as pending. The current dispatch in `c/net/core/lsock.c` implements UNIX socket and TCP polling.

The backend contract in `c/kernel/exec/fd/kpoll.h` requires registration through `poll_wait()` before inspecting readiness state. Producers publish state changes before calling `waitq_wake_all()`.

Registration attaches a waiter carrying the poll table’s lock and `triggered` flag. A waker sets that flag under the table lock before waking the thread. The poller checks the flag under the same lock and hands that lock to the scheduler when blocking. An event arriving during the scan either changes the state being inspected or leaves a wake indication that causes another scan.

Broadcast wakeups matter because a poll registration observes readiness without consuming data. Waking one registration could leave a reader asleep while data remains available. The waiter hooks and wake ordering are implemented in `c/kernel/sync/wait.h` and `c/kernel/sync/wait.c`.

The core removes registrations before returning or beginning another pass. A scan that finds ready entries returns before checking pending signals. When no entry is ready and registration capacity was exceeded, the core returns `POLL_E_NOMEM` before sleeping.

## Poll and select in libc

Libc’s `poll()` forwards its array to `SYS_POLL`. `struct pollfd` and `struct logit_pollfd` have matching field layouts. The wrapper translates kernel errors to `EINVAL`, `EFAULT`, `ENOMEM` or `EINTR`. See `c/apps/libc/src/poll.c` and `c/apps/libc/include/poll.h`.

`select()` translates requested read and write sets into a poll array, waits through `poll()`, and rebuilds the result sets after success. Error and hangup events mark the requested sides ready so a following I/O operation can report its outcome. Its return value counts the bits placed in the result sets, including both bits when one descriptor appears in both sets.

Keeping this translation in libc lets the library define the `fd_set` representation. `FD_SETSIZE` is 256 in `c/apps/libc/include/sys/select.h`. A null timeout becomes an indefinite wait. A positive timeout shorter than one millisecond becomes one millisecond.

## Event and timer descriptors

Eventfd provides a counter that another thread can increment to wake a poll loop. A successful read transfers an eight-byte counter value and drains the counter. With `EFD_SEMAPHORE`, a read returns one and decrements the counter by one. Writes consume an eight-byte value and add it to the counter; nonzero additions wake waiters. The backend is implemented in `c/kernel/exec/fd/file.c`.

Timerfd uses the same `F_EVENT` backend, with timer expiry supplying the increments. Reads return accumulated expiration counts. For periodic timers, the update computes how many intervals have elapsed on the tick clock since the deadline. `file_timerfd_tick()` runs through the timer path in `c/kernel/exec/signal/ksignal.c`.

The native `SYS_TIMERFD` interface creates a descriptor when its descriptor argument is negative and rearms an existing descriptor otherwise. Libc exposes these operations through `timerfd_create()` and `timerfd_settime()` in `c/apps/libc/src/poll.c`. Arming or disarming clears the pending expiration count.

Poll deadlines and timerfd deadlines use `TIMER_HZ`, defined as 100 in `c/drivers/timer/pit.h`. Duration conversion rounds upward to ticks and adds one tick to the initial deadline. Periodic timer intervals use the rounded tick duration. The conversion code is in `c/kernel/sync/wait.c` and `c/kernel/exec/fd/file.c`.

## PTY data flow

`SYS_PTY_OPEN` creates an anonymous master and slave descriptor pair. Libc exposes it through `openpty()`, which can apply initial terminal attributes and window dimensions. When supplied, its name output receives an empty string. The ABI comments explain the descriptor-pair design in terms of the synthetic device namespace. See `include/abi/pty.h` and `c/apps/libc/src/pty.c`.

Both endpoints use `F_TTY` and share a `struct pty` in `c/kernel/exec/fd/pty.c`. Terminal metadata follows the character-device path in `c/kernel/exec/load/meta.c`, which libc’s `isatty()` queries through `fstat()`.

The terminal contains two 32 KiB queues:

| Operation | Data flow |
|---|---|
| Master write | Processes input through the line discipline into the slave input queue. |
| Slave read | Consumes processed input. |
| Slave write | Processes output into the master output queue. |
| Master read | Consumes slave output and echoed input. |

The default terminal has 24 rows and 80 columns, canonical input, echo, erase echo and signal-character processing. Input carriage returns become newlines. Output processing expands slave-written newlines to carriage return plus newline.

Canonical input accumulates in a 4 KiB editing buffer. Newline, the configured EOF character or a full editing buffer publishes the edited bytes. EOF at the start of an empty edit records a pending zero-length read. Erase removes a pending byte; line kill clears the edit. Canonical slave reads stop after a newline. Switching out of canonical mode publishes any pending edit and sends subsequent input bytes directly to the slave queue.

Before accepting input, the write path reserves capacity for pending edited bytes and echo expansion. Reads and writes sleep on the terminal’s shared wait queue when they need data or capacity. `pty_poll()` registers on that queue before checking readiness.

Final closure of an endpoint updates peer state and wakes waiters. The surviving endpoint reports `LPOLLHUP | LPOLLIN`; reads drain queued bytes and then return EOF. Final closure of both endpoint descriptions releases the shared terminal object.

## Terminal controls and sessions

`SYS_PTY_CTL` handles attributes, window dimensions, queue flushing, output draining, signal retrieval and controlling-terminal operations. Libc routes `tcgetattr()`, `tcsetattr()`, `tcflush()`, `tcdrain()` and foreground process-group operations through it. Output draining waits until the master has consumed the output queue. The wrappers are in `c/apps/libc/src/termios.c` and `c/apps/libc/src/uio.c`.

The process table owns session and process-group membership. A PTY stores its controlling session, foreground process group and a terminal identity allocated monotonically during the boot. Attaching the slave requires a session leader. Setting the foreground group checks membership in the caller’s session and the caller’s controlling-terminal identity. Final terminal destruction clears matching identities from processes. See `c/kernel/exec/proc.c` and `c/kernel/exec/fd/pty.c`.

With signal processing enabled, interrupt and quit characters record pending signal numbers and clear queued and edited input. A window-size update records `LOGIT_SIGWINCH`. The master retrieves events through `LPTY_SIGNAL`; repeated occurrences of the same pending signal coalesce in a bitset. The SSH channel pump in `c/apps/services/ssh/sshd_channels.h` retrieves these events and forwards them to its child process.

`tests/boot/pty_probe.c` demonstrates child setup: allocate a pair, fork, create a session, attach the slave with `TIOCSCTTY`, duplicate the slave onto standard descriptors and execute the child image.

## Limitations

- Resource bounds are 32 descriptors per process, 512 open file descriptions across the system, 32 poll entries per call and 16 timerfd objects. These are defined in `c/kernel/exec/proc.h`, `include/abi/logit_abi.h` and `c/kernel/exec/fd/file.c`.
- Negative poll entries receive zero `revents` during result processing, but their missing callbacks count as invalid sources inside `poll_core()`. They can cause an early zero return while other entries remain unready. The guest test covers a negative entry beside an already-ready pipe. See `c/kernel/exec/fd/kpollsys.c`, `c/kernel/exec/fd/kpoll.c` and `tests/guest/exec/polltest.c`.
- IP datagram and raw socket polling returns `LPOLLNVAL`. `select()` clears its exception set on success and maps invalid descriptors into requested readiness sets. Although the ABI comments describe `LPOLLRDNORM` and `LPOLLWRNORM` as aliases, the current polling path performs no conversion to `LPOLLIN` and `LPOLLOUT`. See `c/net/core/lsock.c`, `c/apps/libc/src/poll.c` and `c/kernel/exec/fd/kpoll.c`.
- Readiness is a snapshot that another consumer can change before I/O. Pipe write readiness reports some available space; an atomic write can require more. Eventfd additions saturate at `UINT64_MAX - 1`. See `c/kernel/exec/fd/file.c`.
- Serial-console writes report the requested length even when the transmit ring drops bytes, and `LPOLLOUT` remains set when the ring is full. A later `serial_putc()` queues a loss notice when space permits. Diagnostics may appear between output chunks or write calls, and console read echo bypasses the message lock. See `c/kernel/exec/fd/file.c` and `c/drivers/char/serial.c`.
- Libc’s timerfd wrappers support the monotonic clock and relative timers. Absolute timers and requests for the previous timer value are rejected. See `c/apps/libc/src/poll.c`.
- PTYs accept `VMIN = 1` and `VTIME = 0`. Software flow control, background job-control enforcement and automatic terminal-generated group signals are absent. Signal delivery remains the master owner’s responsibility, and pending signal events alone do not set `LPOLLIN`. See `c/kernel/exec/fd/pty.c`, `c/kernel/exec/proc.c` and `include/abi/pty.h`.
- Libc’s `fcntl()` stores `FD_CLOEXEC` locally without enforcing closure across execution. `F_SETFL` can set `O_NONBLOCK` but cannot clear it; `F_GETFL` returns `O_RDWR`; `F_DUPFD` ignores the requested minimum descriptor. See `c/apps/libc/src/io.c`.

## Tests in the tree

The tree defines the following checks and controls.

| Target | Coverage in the test sources |
|---|---|
| `test-poll` | Core readiness, wakeups, registration cleanup and overflow using kernel poll and wait code with a host scheduler model. Its prerequisite control moves readiness checks before registration. |
| `test-poll-os` | Guest pipe, console, `select()`, eventfd and timerfd behavior. Its prerequisite control substitutes a sleep and an empty readiness result. |
| `test-pty` | Guest allocation, terminal detection after execution, controlling-terminal setup, echo changes and parent/child byte exchange. Prerequisite controls preserve echo or replace child input with a pipe. |

The target definitions and control dependencies are in `tests/poll.mk` and `tests/pty.mk`. The assertions are in `tests/unit/poll_test.c`, `tests/guest/exec/polltest.c` and `tests/boot/pty_probe.c`.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/proc.h` | Process descriptor table, descriptor limit and terminal identities. |
| `c/kernel/exec/proc.c` | Descriptor ownership, duplication, inheritance and session operations. |
| `c/kernel/exec/syscall.c` | Descriptor I/O entry points and subsystem dispatch. |
| `c/kernel/exec/fd/file.h` | Open file descriptions, reference cleanup and backend interfaces. |
| `c/kernel/exec/fd/file.c` | Description pool, VFS I/O, pipes, console, counters and readiness dispatch. |
| `c/kernel/exec/fd/kpoll.h` | Backend registration contract and poll structures. |
| `c/kernel/exec/fd/kpoll.c` | Readiness scans, registrations and scheduler waits. |
| `c/kernel/exec/fd/kpollsys.c` | Poll user copies, retained references and counter descriptor installation. |
| `c/kernel/exec/fd/pty.c` | PTY queues, line discipline, controls and lifetime. |
| `c/kernel/exec/load/meta.c` | Descriptor file metadata. |
| `c/kernel/exec/signal/ksignal.c` | Console input collection and timerfd tick integration. |
| `c/kernel/sync/wait.h` | Waiter hooks and locking contract. |
| `c/kernel/sync/wait.c` | Queue wakeups and deadline conversion. |
| `c/kernel/diag/kprintf.c` | Console message lock used by diagnostics and console writes. |
| `c/kernel/cpu/irq/interrupts.c` | Timer integration for serial draining and signal processing. |
| `c/drivers/char/serial.c` | COM1 setup, transmit ring, FIFO draining and loss reporting. |
| `c/drivers/char/serial.h` | Serial interfaces and transmit statistics. |
| `c/drivers/timer/pit.h` | Timer frequency and tick interfaces. |
| `c/fs/vfs/vfs.h` | Filesystem operation interfaces. |
| `c/net/core/lsock.c` | Socket file backend and polling dispatch. |
| `include/abi/logit_abi.h` | Poll masks, structures, limits and counter syscall interfaces. |
| `include/abi/pty.h` | PTY operations, attributes and signal-event contract. |
| `c/apps/libc/include/poll.h` | Public poll declarations and masks. |
| `c/apps/libc/include/sys/select.h` | Descriptor sets and select declarations. |
| `c/apps/libc/src/io.c` | Descriptor wrappers, terminal detection and descriptor flags. |
| `c/apps/libc/src/poll.c` | Poll, select, eventfd and timerfd wrappers. |
| `c/apps/libc/src/pty.c` | PTY allocation wrapper. |
| `c/apps/libc/src/termios.c` | Terminal attribute and process-group wrappers. |
| `c/apps/libc/src/uio.c` | Terminal ioctl translation. |
| `c/apps/services/ssh/sshd_channels.h` | SSH PTY handling and signal forwarding. |
| `tests/unit/poll_test.c` | Polling model and lost-wakeup checks. |
| `tests/guest/exec/polltest.c` | Guest polling API checks. |
| `tests/boot/pty_probe.c` | Guest PTY setup and byte-exchange checks. |
| `tests/poll.mk` | Poll targets and negative controls. |
| `tests/pty.mk` | PTY targets and negative controls. |

## See also

- [System calls](../architecture/system-calls.md)
- [Process management](processes.md)
- [Synchronisation](synchronization.md)
- [Signals](signals.md)
- [Virtual file system](../filesystems/vfs.md)
- [Sockets](../networking/sockets.md)
- [The C library](../userland/libc.md)