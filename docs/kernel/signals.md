# Signals

LogitOS keeps signal dispositions, pending signals and blocked masks in the kernel. Delivery runs at the common interrupt exit to user mode, where the kernel has the interrupted register state available. A caught signal enters a handler through a frame on the user stack; returning through libc restores the saved registers, x87/SSE state and mask. `c/kernel/exec/signal/ksignal.c` owns bookkeeping, and `c/kernel/exec/signal/ksigframe.c` constructs and restores frames.

## State and lifecycle

`g_sig` contains `NPROC` entries of `struct sigst`, indexed by PID through `ksig_find_locked()`. Each entry holds dispositions, handler masks, flags, restorer addresses, pending and blocked masks, an alarm deadline and stopped state. `g_sig_lock` protects this state. The layout is defined in `c/kernel/exec/signal/ksig_int.h`.

Signal numbers run from 1 through 31. The signal number selects its bit position in a 64-bit mask. Posting an asynchronous signal sets that bit, so repeated posts coalesce while it remains pending. Kernel definitions live in `include/abi/logit_abi.h`; libc names live in `c/apps/libc/include/signal.h`.

Lifecycle hooks maintain signal state alongside the process:

| Operation | Signal state |
|---|---|
| Creation | Default dispositions, empty pending and blocked masks, cleared alarm and stopped state |
| Fork | Child receives dispositions, handler masks, flags, restorers and the blocked mask; pending signals and alarm start clear |
| Successful exec | Caught dispositions reset to default; ignored dispositions and the blocked mask carry into the replacement image; handler masks, flags and restorers are cleared |
| Teardown | Signal state and fault records for the PID are released |

These hooks are implemented in `c/kernel/exec/signal/ksignal.c`, with callers in `c/kernel/exec/proc.c` and `c/kernel/exec/syscall.c`. Resetting caught dispositions during exec prevents handler addresses from referring into the previous program image.

## Posting and delivery

`ksig_post()` sets a pending bit under `g_sig_lock`. After releasing the lock, it looks up and wakes the main scheduler thread, then calls `uthread_wake_process()` to wake adopted live threads. A wake lets a blocking operation reconsider its wait condition. Signal zero checks whether the PID has a signal-table entry. See `c/kernel/exec/signal/ksignal.c` and `c/kernel/sched/uthread.c`.

`interrupt_handler()` calls `ksig_deliver()` at the common exit when the entry is outermost, the saved code selector identifies user mode and `ksig_armed()` reports pending work. Syscall returns, resolved page faults and timer interrupts reach this hook. Timer interrupts also provide delivery points for programs computing in user mode. The hook is in `c/kernel/cpu/irq/interrupts.c`.

The global `ksig_armed()` gate accounts for asynchronous pending sets and synchronous fault records. During signal selection, delivery first takes a fault record belonging to the current scheduler thread. Otherwise, it chooses the lowest numbered eligible asynchronous signal. Eligibility combines the pending and blocked masks, with `SIGKILL` and `SIGSTOP` treated as unmaskable.

For a caught signal, delivery snapshots the disposition and updates the blocked mask while holding `g_sig_lock`. It releases the lock before constructing the user frame because user-memory access can resolve mappings. The copy helpers in `c/kernel/exec/usercopy.c` access pinned physical pages under address-space protection.

One call to `ksig_deliver()` enters at most one caught handler. Ignored signals and default continue actions are consumed as its loop searches for further work.

## Default actions and signal sources

`ksig_default_action()` in `c/kernel/exec/signal/ksignal.c` defines the defaults:

| Signals | Default action |
|---|---|
| `SIGCHLD`, `SIGURG`, `SIGWINCH` | Ignore |
| `SIGCONT` | Continue |
| `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` | Stop |
| Remaining signal numbers | Terminate |

Default termination calls `proc_exit()` with code `128 + signo`. A stop sets the process signal entry’s stopped flag and makes the delivering thread wait through the scheduler. The wait is bracketed by ptrace’s register snapshot and resume hooks in `c/kernel/exec/signal/ksigframe.c`.

Posting `SIGCONT` clears stopped state and pending stop signals. Posting a stop signal clears pending `SIGCONT`. Cancelling opposing signals at posting time preserves the order of a stop followed by a continue before delivery.

Kernel signal sources include:

| Source | Signal | Implementation |
|---|---|---|
| Child exit, after publishing zombie state | `SIGCHLD` | `c/kernel/exec/proc.c` |
| Pipe write with no readers | `SIGPIPE` | `c/kernel/exec/fd/file.c` |
| Expired alarm | `SIGALRM` | `c/kernel/exec/signal/ksignal.c` |
| Serial console byte value 3 | `SIGINT` to the console foreground PID | `c/kernel/exec/signal/ksignal.c` |
| Selected user CPU exceptions | Signal selected from the exception vector | `c/kernel/cpu/irq/interrupts.c` |

Reading the serial console claims its foreground PID. The shell’s signal handler sets a flag, and its foreground-job loop forwards `SIGINT` to pipeline stages. This forwarding lives in `c/apps/coreutils/sh.c`.

## Synchronous faults

The exception path first offers page faults to the memory subsystem. Remaining user exceptions pass through `fault_signal()`, which maps selected vectors to `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL` or `SIGTRAP`. See `c/kernel/cpu/irq/interrupts.c`.

For a caught, unblocked fault signal, `ksig_fault()` attempts to allocate a `struct ksig_fault_record`. The record holds the scheduler thread ID, PID, signal number, sampled CR2 value, error code and exception vector. Delivery consumes it by scheduler thread ID, tying the fault metadata to the thread whose registers will form the handler context. The record table and selection logic live in `c/kernel/exec/signal/ksignal.c`.

CR2 is sampled before frame construction accesses user memory. The context exposes that sample through `cr2`, alongside `err` and `trapno`. Asynchronous delivery fills these three fields with zero.

When an exception proceeds to termination, `c/kernel/cpu/irq/interrupts.c` calls the core-dump hook and exits with code 139. Asynchronous default termination uses `128 + signo` in `c/kernel/exec/signal/ksigframe.c`. Child waits expose these raw exit codes through `c/kernel/exec/proc.c` and `c/apps/libc/src/io.c`.

## Handler frames

`plan_frame()` and `push_frame()` in `c/kernel/exec/signal/ksigframe.c` arrange the frame below the interrupted stack pointer:

| From higher to lower addresses | Purpose |
|---|---|
| Preserved 128-byte red zone and alignment adjustment | Protect interrupted leaf-function locals |
| 512-byte FXSAVE image, aligned to 16 bytes | Save x87/SSE state |
| `struct logit_sigctx`, aligned to 16 bytes | Save general registers, return state, previous mask and fault metadata |
| Eight-byte restorer address | Supply the handler’s return address |

The handler enters with `rsp % 16 == 8`, matching the x86-64 System V function-entry convention. The kernel sets `rip` to the handler address and supplies:

| Register | Value |
|---|---|
| `rdi` | Signal number |
| `rsi` | Zero |
| `rdx` | Address of `struct logit_sigctx` |

Handler entry also clears the direction and trap flags. `include/abi/logit_abi.h` defines the context layout, including its `fpstate` pointer to the saved FXSAVE image.

The floating-point image comes from the interrupt entry snapshot in `c/boot/isr.asm`. Kernel C can use SSE registers before delivery runs, so that earlier snapshot supplies the interrupted program’s state. During signal return, the kernel copies the saved image into the current interrupt entry’s FXSAVE area. The assembly epilogue installs it with FXRSTOR.

## Returning from a handler

Libc’s `sigaction()` supplies `__logit_sigrestore` as the restorer address. The kernel requires a nonzero restorer for a caught disposition. Placing this code in executable libc text lets the stack frame hold data. See `c/apps/libc/src/signal.c` and `c/kernel/exec/signal/ksignal.c`.

The handler’s return instruction pops the restorer address, leaving `rsp` pointing at `struct logit_sigctx`. The restorer loads `SYS_SIGRETURN` into `rax` and executes `int $0x80` without moving the stack pointer.

`interrupt_handler()` intercepts this syscall before dispatch because restoration needs both the interrupt register frame and its FXSAVE area. `ksig_sigreturn()` reads the context and checks its signal-number range. It then restores general registers, `rip` and `rsp`, filters flags through `RFLAGS_USER_MASK`, forces interrupts enabled and uses the user segment selectors from the current interrupt entry. A successful floating-point copy filters MXCSR through `MXCSR_MASK`.

Finally, restoration installs `oldmask` after removing `SIGKILL` and `SIGSTOP`. The restoration code lives in `c/kernel/exec/signal/ksigframe.c`.

## Masks and handler flags

`sigprocmask()` supports `SIG_BLOCK`, `SIG_UNBLOCK` and `SIG_SETMASK`. The kernel removes `SIGKILL` and `SIGSTOP` from supplied masks and rejects `sigaction()` requests for either signal. `sigpending()` reports asynchronous signals that are both pending and blocked. Installing `SIG_IGN` clears the signal’s pending bit. These operations are implemented in `c/kernel/exec/signal/ksignal.c`.

Handler entry adds `sa_mask` to the current blocked mask, then adds the delivered signal unless `SA_NODEFER` is set. The frame carries the preceding mask for restoration.

| Flag | Effect |
|---|---|
| `SA_NODEFER` | Suppresses automatic blocking of the delivered signal; `sa_mask` still applies |
| `SA_RESETHAND` | Resets the disposition to default before entering the handler |
| `SA_RESTART` | Prepares an interrupted syscall for re-execution after handler return |

Libc’s `signal()` installs `SA_RESETHAND | SA_NODEFER`. `sigaction()` provides persistent handlers when `SA_RESETHAND` is clear. Both wrappers live in `c/apps/libc/src/signal.c`.

## Interaction with syscalls

Blocking operations use `ksig_interrupted()` to detect eligible pending signals after excluding ignored dispositions. The predicate also recognizes pending user-thread teardown. Its header comment describes unlocked reads; the implementation samples pending, blocked and ignored state under `g_sig_lock`. See `c/kernel/exec/signal/ksignal.h` and `c/kernel/exec/signal/ksignal.c`.

Console reads, pipe operations and event-counter reads consult this predicate in `c/kernel/exec/fd/file.c`. Child waits consult it in `c/kernel/exec/proc.c`. Polling checks readiness before interruption in `c/kernel/exec/fd/kpoll.c`, giving ready events priority.

An operation interrupted through these checks returns `SIG_E_INTR`, defined as `-4` in `include/abi/logit_abi.h`. Libc’s `read()`, `write()` and `waitpid()` translate that value to `-1` with `errno` set to `EINTR` in `c/apps/libc/src/io.c`. A pipe write interrupted after transferring bytes returns its partial count.

For `SA_RESTART`, `push_frame()` requires syscall vector 128, a nonzero saved syscall number and a result of `SIG_E_INTR`. It replaces saved `rax` with the syscall number and rewinds saved `rip` by two bytes over `int $0x80`. Handler return then reissues the syscall using the saved argument registers. This logic lives in `c/kernel/exec/signal/ksigframe.c`.

The signal syscall block is defined in `include/abi/logit_abi.h`:

| Syscall | Number | Purpose |
|---|---:|---|
| `SYS_SIGACTION` | 130 | Read or install a disposition |
| `SYS_SIGPROCMASK` | 131 | Read or change the blocked mask |
| `SYS_SIGRETURN` | 132 | Restore a handler context |
| `SYS_SIGPENDING` | 133 | Read blocked pending signals |
| `SYS_ALARM` | 134 | Replace or cancel an alarm; return previous remaining seconds, rounded up |
| `SYS_SIGSUSPEND` | 135 | Install a waiting mask and wait for interruption |
| `SYS_SIGQUERY` | 136 | Read global counters or the caller’s pending or blocked mask |

Libc’s `kill()` invokes `SYS_KILL` with `LOGIT_KILL_SIGNAL` in the third argument. `proc_syscall()` routes that form to `ksig_kill()`; clearing the flag requests deferred process destruction. For a target with no parent and no GUI window, `ksig_kill()` refuses signals whose default action is termination unless a caught handler is installed. See `c/apps/libc/src/signal.c`, `c/kernel/exec/proc.c` and `c/kernel/exec/signal/ksignal.c`.

## Limitations

- Dispositions and masks are shared by the process. `pthread_sigmask()` changes that shared mask, and `pthread_kill()` returns `ENOSYS`. Asynchronous delivery targets processes. See `c/apps/libc/src/pthread.c`.
- Alternate signal stacks are absent. Frame construction failure terminates the process. Escaping through `longjmp()` bypasses mask restoration; `tests/unit/sigtest_main.c` unblocks the fault signal afterward.
- `SA_SIGINFO` provides a null second argument and the custom context as the third. Queued signal payloads and the behaviors represented by `SA_NOCLDSTOP` and `SA_NOCLDWAIT` are absent. See `include/abi/logit_abi.h` and `c/apps/libc/include/signal.h`.
- The `sigsuspend()` comments describe carrying the original mask through handler delivery. `sys_sigsuspend()` restores that mask and clears `in_suspend` before delivery, so a signal temporarily unblocked for the wait can become blocked again before selection. See `c/kernel/exec/signal/ksignal.c`.
- The stopped-loop comment says pending unmaskable signals reach selection on the next pass. Pending `SIGKILL` breaks the inner wait while stopped state remains set, causing the outer loop to enter the stopped branch again. Posting `SIGCONT` clears that state. See `c/kernel/exec/signal/ksigframe.c`.
- The `SYS_POLL` ABI comment says the wait is never restarted. The frame code applies its generic `SA_RESTART` rule to an interrupted `SYS_POLL` too. See `include/abi/logit_abi.h`, `c/kernel/exec/fd/kpollsys.c` and `c/kernel/exec/signal/ksigframe.c`.
- Signal return checks context readability and signal range, then assigns saved `rip` and `rsp` directly. It requires no outstanding-frame record, and failure to read the floating-point image does not reject restoration. See `c/kernel/exec/signal/ksigframe.c`.
- Group-directed `kill()` and complete terminal job control are absent. The serial console tracks one foreground PID; controlling PTYs track sessions and foreground groups with policy gaps documented in `include/abi/pty.h`.

## Test coverage

`tests/unit/sigtest_main.c` defines guest checks for handler entry and return, disposition reset, masks, faults, child and pipe signals, interrupted reads, syscall restart and termination of a child computing in user mode. Its floating-point check loads known values into XMM registers, raises a signal within the same assembly block and compares the values after handler return. The restart check makes the alarm handler write the byte consumed by the restarted pipe read.

`tests/signal.mk` defines `test-signal`, `test-signal-negctl` and the aggregate `test-signal-all`. The negative control builds with `SIGNAL_NO_FPU`, which omits floating-point preservation from signal frames. `tests/boot/run-signal-test.sh` checks the guest’s result markers.

The negative-control recipe accepts any nonzero driver exit as the expected failure, including a boot or timeout failure. Its output needs inspection to establish that the XMM check detected the injected defect.

## See also

- [Process management](processes.md)
- [File descriptors, polling and PTYs](files-and-polling.md)
- [Debugging support: ptrace and core dumps](ptrace-and-coredumps.md)
- [System calls](../architecture/system-calls.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/signal/ksignal.c` | Lifecycle, posting, masks, defaults, alarms, console input and signal syscalls |
| `c/kernel/exec/signal/ksigframe.c` | Delivery, frame layout, syscall restart and context restoration |
| `c/kernel/exec/signal/ksig_int.h` | Signal state and synchronous fault records |
| `c/kernel/exec/signal/ksignal.h` | Kernel signal interface and delivery contract |
| `c/kernel/cpu/irq/interrupts.c` | Fault mapping, delivery hook and signal-return interception |
| `c/boot/isr.asm` | Interrupt register frame and FXSAVE/FXRSTOR |
| `include/abi/logit_abi.h` | Syscall numbers, flags, errors and signal context layouts |
| `c/kernel/exec/proc.c` | Lifecycle callers, child signals, waits and kill routing |
| `c/kernel/exec/syscall.c` | Syscall dispatch and disposition reset after exec |
| `c/kernel/exec/usercopy.c` | Copies through pinned pages under address-space protection |
| `c/kernel/exec/fd/file.c` | Console, pipe and event-counter interruption |
| `c/kernel/exec/fd/kpoll.c` | Poll readiness and interruption |
| `c/kernel/exec/fd/kpollsys.c` | Poll syscall translation |
| `c/kernel/sched/uthread.c` | Wakeups for adopted process threads |
| `c/apps/libc/include/signal.h` | Public signal API and supported scope |
| `c/apps/libc/src/signal.c` | Signal wrappers and executable restorer |
| `c/apps/libc/src/io.c` | I/O interruption mappings and child waits |
| `c/apps/libc/src/pthread.c` | Thread signal API behavior |
| `c/apps/coreutils/sh.c` | Shell signal handling and foreground-job forwarding |
| `include/abi/pty.h` | PTY session and foreground-group contract |
| `tests/unit/sigtest_main.c` | Guest signal checks |
| `tests/signal.mk` | Signal test builds and floating-point negative control |
| `tests/boot/run-signal-test.sh` | Guest test driver and result checks |