# System calls

LogitOS applications enter the kernel through `int 0x80`, passing a syscall number and up to three argument words in registers. Shared ABI headers define numbers, records and return contracts. The kernel checks policy and routes each request to its subsystem; application helpers and libc provide caller interfaces. Generated bindings, packing macros and layout assertions keep selected parts of the ABI consistent.

## Register convention and entry

`include/abi/logit_abi.h` defines the register convention. `_sys()` in `c/apps/logit.h` and the syscall helper in `c/apps/libc/src/io.c` implement it.

| Register | Meaning |
|---|---|
| `rax` on entry | Syscall number |
| `rdi` | First argument |
| `rsi` | Second argument |
| `rdx` | Third argument |
| `rax` on return | Result word |

Arguments occupy 64-bit registers. Each call interprets those words as integers, pointers or packed fields. Records in user memory carry larger requests. For example, `SYS_FUTEX` takes an address, a word containing the timeout in its upper 32 bits and a value in its lower 32 bits, and an operation selector. `uthread_syscall()` in `c/kernel/sched/uthread.c` decodes this contract from `include/abi/logit_abi.h`.

`idt_init()` in `c/kernel/cpu/irq/idt.c` installs vector 128 as a present interrupt gate with DPL3 and kernel code selector `0x08`, making it reachable from ring 3. The `isr128` stub in `c/boot/isr.asm` pushes a dummy error code and the vector number, then enters the common interrupt path.

The assembly saves the general registers and a 512-byte FXSAVE area before calling `interrupt_handler()`. The register frame follows `struct registers` in `c/kernel/cpu/irq/interrupts.h`. A syscall handler places its result in the frame’s `rax` field. The epilogue restores the floating-point state and general registers, then returns through `iretq`.

For an outer syscall from ring 3, `interrupt_handler()` enables interrupts when the caller’s saved interrupt flag is set. Builds defining `BKL_VERIFY_IRQ_OFF` retain interrupt masking for comparison. Entry-depth tracking suppresses timer preemption during the syscall, while blocking operations can sleep and resume on another CPU. Shared objects use their own synchronization. These rules appear in `c/kernel/cpu/irq/interrupts.c` and `c/kernel/exec/syscall.c`.

`SYS_SIGRETURN` takes a dedicated branch in `interrupt_handler()`. After entry and worker-policy checks, it restores the signal context through the register frame and the saved FXSAVE area. Keeping restoration at this level lets the assembly epilogue install both parts of the context.

The same interrupt path supplies scheduler observation hooks. During recording, `flight_interrupt_arrival_tsc()` timestamps entry at the start of the C handler, after the assembly saves registers and floating-point state. `flight_interrupt_enter()` records vector 128 alongside other interrupt vectors. The syscall exit path calls `flight_interrupt_leave()` and `observe_advance_shutdown()`. The recorder implementation is in `c/kernel/sched/observe/flight_recorder.c`.

## Numbering and dispatch

The main number declarations live in `include/abi/logit_abi.h`. Additional declarations appear in `include/abi/aex_agent.h`, `include/abi/pty.h` and `include/abi/fs_ref.h`.

| Symbol | Number | Purpose |
|---|---:|---|
| `SYS_WRITE` | 1 | Write through a descriptor |
| `SYS_FORK` | 50 | Create a child process |
| `SYS_SIGRETURN` | 132 | Restore a signal context |
| `SYS_FTRUNCATE` | 189 | Change a buffered file description’s length |
| `SYS_SCHED_OBSERVE` | 224 | Control and export a scheduler observation run |

Developers maintain the number declarations and C dispatch switches by hand. In `c/kernel/exec/syscall.c`, `syscall_dispatch()` first calls `syscall_entry_checks()`, which checks pending process termination, thread termination and the current TLB generation. It then applies capability classification and calls `syscall_do()`, with optional syscall accounting.

`syscall_do()` applies the agent-worker allowlist through `aex_agent_syscall_allowed()` in `include/abi/agent_policy.h`. Its switch handles some operations locally and forwards others to their owning subsystem. Thread calls reach `uthread_syscall()` in `c/kernel/sched/uthread.c`; file metadata calls reach `meta_syscall()` in `c/kernel/exec/load/meta.c`. This keeps argument interpretation beside the subsystem that owns the operation.

The default branch forwards to `wm_gui_syscall()` in `c/kernel/gui/wm.c`, which handles GUI and miscellaneous calls. An unmatched number returns `-1`.

Capability classification is per syscall number. `syscall_cap_class()` assigns `CAP_FS` to listed filesystem path and enumeration operations, and `CAP_NET` to the listed network family, including socket I/O. Shared descriptor operations such as `SYS_READ`, `SYS_WRITE` and `SYS_FTRUNCATE` receive classification zero. The comments beside the classifier explain the acquisition rule: filesystem access is checked when acquired through a path, while descriptor operations also serve pipes and terminals.

`SYS_SCHED_OBSERVE` forwards its three arguments, command, request address and request size, to `observe_syscall()` in `c/kernel/sched/observe/observe_control.c`. Starting and stopping a run require user ID zero. Status and export accept root or a caller whose process ID and user ID match the recorded owner. The worker allowlist rejects this syscall number.

## Results and error translation

Each syscall defines the meaning of its result word. Counts, identifiers, addresses, status values and errors all travel through `rax`. Raw callers interpret the result using the selected call’s contract; libc translates failures according to the operation.

The following mappings appear in `c/apps/libc/src/io.c`, using constants from `include/abi/logit_abi.h` and `c/apps/libc/include/errno.h`.

| Operation | Kernel result | Libc result |
|---|---|---|
| `read()` or `write()` | `EAGAIN_RC`, value `-2` | `-1`, with `errno = EAGAIN` |
| `read()` or `write()` | `SIG_E_INTR`, value `-4` | `-1`, with `errno = EINTR` |
| `close()` | `-1` | `-1`, with `errno = EBADF` |
| `close()` | `-2` | `-1`, with `errno = EIO` |

The interpretation of `-2` depends on the call. Read and write use it for temporary unavailability; close uses it for failed write-back. `proc_fd_close()` in `c/kernel/exec/proc.c` distinguishes an invalid descriptor from a failure returned by releasing its file reference. These libc wrappers preserve the previous value of `errno` on success.

Scheduler observation uses negative errno values defined in `include/abi/logit_sched_observe.h`, including `SCHED_OBSERVE_EFAULT` (`-14`) for a failed user-memory transfer and `SCHED_OBSERVE_EINVAL` (`-22`) for an invalid request. Its stop command returns `1` while shutdown is pending and `0` when shutdown has finished, as implemented in `c/kernel/sched/observe/observe_control.c`.

## Generated ABI artifacts

`tools/gen_abi.py` reads two inputs:

| Input | Contents |
|---|---|
| `include/abi/logit_abi.h` | Supported structure definitions, field types and array bounds |
| `include/abi/logit_calls.abi` | Call descriptions, argument order, memory directions, byte extents and packed fields |

For example, `include/abi/logit_calls.abi` contains:

```text
call sock_open      SYS_SOCK_OPEN      str(host) pack(port:16:16, flags:0:16)
```

This describes a string argument followed by a word containing two 16-bit fields. The generator emits caller packing and the extraction macros `LOGIT_SOCK_OPEN_B_PORT` and `LOGIT_SOCK_OPEN_B_FLAGS`. The `SYS_SOCK_OPEN` handler in `c/kernel/exec/syscall.c` uses those macros, tying its field positions to the generated caller.

The generated outputs are:

| Output | Contents |
|---|---|
| `fsroot/as/lib/abi.as` | AetherScript bindings and record layouts |
| `include/abi/logit_pack.h` | C macros for extracting packed argument fields |
| `c/apps/as/sema/abi_layout.inc` | C assertions for structure sizes, field offsets and field widths |

The generator computes layouts using LP64 rules. `c/apps/as/sema/constants.c` includes the generated assertions, so the compiler checks the calculated layouts against the C declarations. `tools/abi_native.py` renders the language bindings, and `c/apps/as/common/system_constants.def` binds their syscall names to shared constants.

After changing a generator input, regenerate and compare the artifacts:

```sh
python3 tools/gen_abi.py --write
make check-abi
```

The `check-abi` target in `Makefile` invokes the generator’s comparison mode. It compares all three outputs with freshly rendered content and checks that syscall names referenced by generated bindings appear in the constants manifest.

## Passing user memory

Pointer arguments need a copying strategy and a lifetime contract. `c/kernel/exec/usercopy.c` provides `user_copy_from()`, `user_copy_to()` and `user_copy_string()`.

The transfer implementation copies one page at a time under the address-space guard, using a referenced and pinned physical page. This coordinates the copy with mapping changes and page lifetime. The correction comment in that file explains why checking a mapping and accessing it later creates a race when another thread changes the address space.

The `SYS_READ` and `SYS_WRITE` cases in `c/kernel/exec/syscall.c` use private kernel buffers around file operations. A write copies each input chunk into kernel storage before passing it to the file layer. A read receives data into kernel storage and then copies the returned bytes to the application. Blocking operations thereby use storage whose lifetime the kernel controls.

Records can carry a size contract. `SYS_STAT` accepts the caller’s buffer length. The `emit()` helper in `c/kernel/exec/load/meta.c` requires room for the length and version fields, copies the smaller of the supplied length and the kernel record size, and writes the copied length into `struct logit_stat`.

Scheduler observation demonstrates a versioned request protocol. Its records begin with a header containing size, version and run ID. `observe_syscall()` checks the supplied byte count, copies the request, and validates its size and version before routing the command. Command handlers validate the run ID as required. `c/kernel/sched/observe/observe_platform.c` connects production callers to the checked user-copy routines.

Observation exports use a cursor supplied with each request and return at most 64 records through a private kernel buffer. After a run stops, the reader confirms a completed stream with an empty read carrying `SCHED_READ_CONFIRM_EXPORT` and a cursor one past its final sequence. The controller retains the run’s pools until producing CPUs have sealed their rings, exports have been confirmed and active readers have left. The protocol is defined in `include/abi/logit_sched_observe.h` and implemented in `c/kernel/sched/observe/observe_control.c`.

## Adding a syscall end to end

`SYS_FTRUNCATE` provides an example across the ABI, dispatcher, file layer and libc.

1. Define the contract and choose a number. Search the ABI headers and their reservation comments. Document argument interpretation, pointer directions, lengths, results and resource ownership. `SYS_FTRUNCATE` is number 189 in `include/abi/logit_abi.h` and takes a descriptor and a length.

2. Implement the operation and add routing. Its case in `c/kernel/exec/syscall.c` acquires a file reference and calls `file_truncate()` in `c/kernel/exec/fd/file.c`. That function holds the file I/O guard, zero-fills growth, updates the buffered length and marks changed content dirty. Persistence follows the existing flush path.

3. Review policy and memory access. Update `syscall_cap_class()` when the operation needs a capability category, and review `include/abi/agent_policy.h` for worker access. For pointer arguments, use checked copies and define buffer ownership across sleeps. Truncate follows the descriptor classification documented beside the capability table.

4. Expose the caller interface. Add the declaration and wrapper to the relevant application or libc files. `ftruncate()` is declared in `c/apps/libc/include/unistd.h` and implemented in `c/apps/libc/src/io.c`. Its wrapper passes the descriptor and length and translates failure into `errno`.

5. Update generated consumers where applicable. Add a call description to `include/abi/logit_calls.abi` and a binding to `c/apps/as/common/system_constants.def`. The existing truncate description has two integer arguments. Regenerate the artifacts and run `make check-abi`. Use generated extraction macros when the handler consumes packed fields.

6. Exercise the backend and guest boundary. `tests/unit/storage_test.c` checks the file implementation. `tests/boot/run-storage-test.sh` exercises syscall behavior across two boots using the same disk copy. `tests/storage.mk` defines `test-storage`, `test-storage-negctl` and `test-storage-os`; both positive targets require the negative control.

For changes to the kernel and callers, build both artifacts: `make` builds the kernel ISO, and `make build/disk.img` builds the application disk. These targets are defined in `Makefile`.

Extend guest libc coverage when changing a C wrapper. `make test-libc` runs the program built from `tests/unit/libctest_main.c` through `tests/boot/run-libc-test.sh`. When adding a test fragment, run `make test-mk-wired` and demonstrate that the negative control fails for the intended reason. See [Writing tests](../contributing/writing-tests.md).

## Limitations

- `SYS_SPAWN`, number 63, is declared without an implementation and returns `-1` through the fallback. `include/abi/logit_abi.h` preserves this history.
- `SYS_GUI_RECEIPT`, number 225, is reserved in `include/abi/logit_abi.h`; it has no dispatch case and returns `-1` through the window manager fallback.
- Error encoding varies by call. Several interfaces collapse failures into `-1`, leaving libc to choose an approximate error. A universal conversion from negative results to `errno` would misinterpret existing contracts.
- `errno` uses process-global storage in `c/apps/libc/src/io.c`, with an external `int` declaration in `c/apps/libc/include/errno.h`.
- `check-abi` verifies generated artifacts and name bindings. Dispatch behavior, policy enforcement and subsystem results require execution tests.

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_abi.h` | Main numbers, register convention, records and return contracts |
| `include/abi/aex_agent.h`, `include/abi/pty.h`, `include/abi/fs_ref.h` | Additional syscall numbers and records |
| `c/apps/logit.h` | Raw application syscall helper |
| `c/kernel/cpu/irq/idt.c` | User-callable interrupt gate |
| `c/boot/isr.asm`, `c/kernel/cpu/irq/interrupts.h` | Assembly entry, saved register frame and return |
| `c/kernel/cpu/irq/interrupts.c` | Interrupt dispatch, entry depth, signal restoration and observation hooks |
| `c/kernel/exec/syscall.c` | Entry checks, capability classification, accounting and routing |
| `include/abi/agent_policy.h` | Worker syscall allowlist |
| `c/kernel/sched/uthread.c` | Thread calls and futex argument decoding |
| `include/abi/logit_sched_observe.h` | Observation commands, request records, export protocol and errors |
| `c/kernel/sched/observe/flight_recorder.c` | Interrupt recording and timestamps |
| `c/kernel/sched/observe/observe_control.c` | Observation access checks, run control and exports |
| `c/kernel/sched/observe/observe_platform.c` | Observation memory transfers and caller identity |
| `c/kernel/exec/usercopy.c`, `c/kernel/exec/usercopy.h` | Checked user-memory transfers |
| `c/kernel/exec/load/meta.c` | Metadata calls and record-length handling |
| `c/kernel/exec/proc.c`, `c/kernel/exec/fd/file.c` | Descriptor references, close results and file operations |
| `c/kernel/gui/wm.c` | GUI dispatch and unmatched-number fallback |
| `include/abi/logit_calls.abi` | Argument and packing descriptions |
| `tools/gen_abi.py`, `tools/abi_native.py` | ABI generation, comparison and binding rendering |
| `include/abi/logit_pack.h`, `fsroot/as/lib/abi.as` | Generated extraction macros and language bindings |
| `c/apps/as/sema/abi_layout.inc`, `c/apps/as/sema/constants.c`, `c/apps/as/common/system_constants.def` | Layout assertions and constant bindings |
| `c/apps/libc/src/io.c`, `c/apps/libc/include/unistd.h`, `c/apps/libc/include/errno.h` | C interfaces and error translation |
| `tests/storage.mk`, `tests/unit/storage_test.c`, `tests/boot/run-storage-test.sh` | Storage checks, negative control and guest runner |
| `tests/unit/libctest_main.c`, `tests/boot/run-libc-test.sh` | Guest libc checks and runner |
| `Makefile` | Build, ABI comparison, libc test and test-wiring targets |

## See also

- [System call table](../reference/syscall-table.md)
- [The C library](../userland/libc.md)
- [Security model](security-model.md)
- [Interrupts and time](interrupts-and-time.md)
- [Building LogitOS](../getting-started/building.md)
- [Writing tests](../contributing/writing-tests.md)