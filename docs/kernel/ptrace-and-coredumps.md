# Debugging support: ptrace and core dumps

LogitOS provides live process inspection through ptrace and records fatal user CPU exceptions as ELF64 core dumps. Both interfaces use a register array with 27 entries, defined in `c/kernel/exec/signal/coredump.h`. The implementations in `c/kernel/exec/ptrace.c` and `c/kernel/exec/signal/coredump.c` capture registers from the saved user interrupt frame.

## Ptrace interface

`SYS_PTRACE` is syscall 187 in `include/abi/logit_abi.h`. The dispatcher in `c/kernel/exec/syscall.c` forwards the request, target PID and argument to `ptrace_syscall()`.

`c/kernel/exec/ptrace.h` defines seven requests:

| Request | Value | Argument | Operation |
|---|---:|---|---|
| `PTRACE_ATTACH` | 1 | Unused | Establish tracing ownership, post a stop signal and wait for a register snapshot. |
| `PTRACE_DETACH` | 2 | Unused | Remove ownership and post `LOGIT_SIGCONT`. |
| `PTRACE_GETREGS` | 3 | Pointer to `uint64_t[27]` | Read the saved registers. |
| `PTRACE_SETREGS` | 4 | Pointer to `uint64_t[27]` | Stage register edits for resume. |
| `PTRACE_PEEKDATA` | 5 | Pointer to `struct logit_ptrace_word` | Read an aligned word of eight bytes. |
| `PTRACE_POKEDATA` | 6 | Pointer to `struct logit_ptrace_word` | Write an aligned word of eight bytes. |
| `PTRACE_CONT` | 7 | Unused | Resume execution while retaining ownership. |

`struct logit_ptrace_word` contains the target virtual address in `addr` and the transferred word in `data`. Keeping the data in the structure lets the syscall return status without reserving any memory value as an error sentinel.

Success returns `PT_OK`, valued zero. The negative `PT_E_*` statuses distinguish invalid arguments, missing processes, permission failures, existing ownership, a target awaiting a stop, inaccessible memory, alignment errors, attach timeout and tracing table exhaustion.

## Ownership and stopping

Attach requires a current process and a target whose process state is `PROC_RUNNING`. The target PID must exceed one and differ from the caller's PID. `may_attach()` reads credentials through `vfs_cred_get()` and permits UID zero or a caller with the target's UID. Each target has one tracing link; subsequent requests check its recorded tracer PID. These checks live in `c/kernel/exec/ptrace.c`.

Attach posts the unmaskable `LOGIT_SIGSTOP`. At the return to user mode, `ksig_deliver()` enters the signal stop loop and calls `ptrace_note_stop()` with the saved user frame. Reusing this stop loop places register capture at the point where the thread waits to resume. The hooks are in `c/kernel/exec/signal/ksigframe.c`.

The attach loop checks for a published snapshot and calls `sched_poll_wait()` between checks. `ATTACH_TICKS` bounds it to 200 iterations. On timeout, attach posts `LOGIT_SIGCONT`, removes tracing links involving the target and returns `PT_E_TIMEOUT`.

Register requests, memory requests and `PTRACE_CONT` require the link's stopped flag. On leaving the stop loop, `ptrace_note_resume()` applies pending register edits and clears that flag. Use `PTRACE_CONT` to resume with staged edits.

During process exit, `ptrace_proc_free()` removes links involving the exiting PID. If the exiting process was a tracer, cleanup posts `LOGIT_SIGCONT` to its targets. The hook in `c/kernel/exec/proc.c` runs before the process becomes a zombie, clearing ownership before its PID can be reused.

## Registers and remote memory

The register array follows `CORE_R15` through `CORE_GS` in `c/kernel/exec/signal/coredump.h`, matching the x86-64 general register order used by the core file. Both implementations map fields from `struct registers` by name because the interrupt frame has a different order.

`PTRACE_SETREGS` stages general registers, the instruction pointer, the stack pointer and flags. On resume, the kernel preserves the interrupted frame's code and stack selectors. The writable flags mask is `0xCD5`, covering CF, PF, AF, ZF, SF, DF and OF. Other flag bits retain their interrupted values.

Remote memory access validates the target mapping with `vmm_user_range_ok()` and transfers the word through `vmm_copy_in_space()`. The latter reaches the physical page through a kernel alias and holds a page reference and pin during the copy. These helpers are in `c/kernel/mm/virt/vmm.c`.

A sleeping mutex in `c/kernel/exec/ptrace.c` coordinates memory inspection with the resume hook and tracing cleanup. A spinlock protects short operations on the tracing table.

## Core dump triggers

The exception path in `c/kernel/cpu/irq/interrupts.c` attempts recovery before taking a dump:

1. A page fault is offered to `mm_fault()`.
2. A user exception is mapped to a signal and offered to `ksig_fault()`.
3. The fatal user path calls `coredump_take()`, prints the fault details and calls `proc_exit(139)`.

`ksig_fault()` accepts a fault when an installed, unblocked handler exists and a fault record can be stored. Signal delivery then prepares the handler's user frame. The dump call remains in the fatal exception path so it receives the exception registers, saved floating point state and faulting address space before process teardown.

`fault_signal()` defines this mapping:

| CPU vectors | Signal |
|---|---|
| 0, 7, 16, 19 | `LOGIT_SIGFPE` |
| 1, 3 | `LOGIT_SIGTRAP` |
| 4, 5, 13, 14 | `LOGIT_SIGSEGV` |
| 6 | `LOGIT_SIGILL` |
| 11, 12, 17 | `LOGIT_SIGBUS` |

Other exception vectors map to signal number zero and reach the fatal user path when the saved frame indicates user mode.

## Core file format

`coredump_build()` emits a little endian ELF64 file with type `ET_CORE` and machine type x86-64. A `PT_NOTE` segment carries metadata, and `PT_LOAD` segments describe memory. Standard note layouts let ELF tools interpret the file; the private `LOGIT` note adds exception details and memory coverage.

| Note | Contents |
|---|---|
| `NT_PRSTATUS` | Signal, process identifiers and the general register array with 27 entries. |
| `NT_PRPSINFO` | Process metadata and a name shortened to at most 15 characters. |
| `NT_SIGINFO` | A record of 128 bytes containing the signal, signal code and captured CR2 value. |
| `NT_FPREGSET` | The FXSAVE image of 512 bytes, when supplied to the builder. |
| `LOGIT` | Format identification, signal, exception vector, error code, CR2, CR3, coverage counts, flags and region records. |

The standard notes use owner name `CORE`. The private note uses owner name `LOGIT` and type `CORE_NT_LOGIT`, valued `0x4C4F4749`. Its descriptor is `struct core_logit_note` in `c/kernel/exec/signal/coredump.h`.

`c/boot/isr.asm` executes FXSAVE before calling the C interrupt handler. Passing that saved image to the dumper preserves the interrupted program's floating point and SSE state despite subsequent kernel register use.

For page faults, `NT_SIGINFO` records code 1 when error bit zero is clear and code 2 when it is set, distinguishing an absent page from a protection violation. Other exceptions receive code `0x80`.

## Memory selection and coverage

`vma_snapshot()` in `c/kernel/mm/virt/vma.c` supplies regions ordered by virtual address. The kernel adapter classifies each region, and the builder checks anonymous pages for resident user mappings.

| Region kind | Representation |
|---|---|
| Anonymous memory | One `PT_LOAD` per contiguous run of resident user pages. |
| File mapping | A `PT_LOAD` spanning the region, with `p_filesz` zero. |
| Shared memory segment | A `PT_LOAD` spanning the region, with `p_filesz` zero. |

The file mapping policy saves dump space by retaining range metadata while the mapped file supplies its original contents. The `LOGIT` region records contain each range, protection, kind and dumped byte count.

Coverage fields describe different stages of selection:

| Field | Meaning |
|---|---|
| `want_regions` | Number of regions reported by the source callback. |
| `nregion` | Number of region records retained in the private note. |
| `got_regions` | Number of retained regions with bytes stored in the dump. |
| `want_bytes` | Resident anonymous bytes found within the retained regions, including runs beyond the program header limit. |
| `got_bytes` | Memory payload bytes stored in the dump. |

Among the runs represented by program headers, the builder copies the run containing the saved stack pointer first. This gives stack data priority under the byte cap. Program headers retain address order, while their file offsets reflect the copying order.

When the buffer fills, the builder copies whole pages that fit and reduces `p_filesz` while preserving `p_memsz`. `CORE_F_TRUNCATED` records a byte shortfall. `CORE_F_PHDRFULL` and `CORE_F_RGNFULL` identify exhaustion of the program header and region tables.

## Storage and resource bounds

`c/kernel/exec/signal/coredump.h` defines these bounds:

| Constant | Value | Purpose |
|---|---:|---|
| `CORE_BUF_MAX` | 256 KiB | Capacity of each complete dump buffer. |
| `CORE_PHDR_MAX` | 64 | One note header and up to 63 memory headers. |
| `CORE_RGN_MAX` | 32 | Region records in the private note. |
| `CORE_SLOTS` | 4 | Static buffers and corresponding output paths. |
| `CORE_MAX_PER_BOOT` | 8 | Dump attempt budget per boot. |

Each crash atomically reserves a static buffer and owns it through construction, VFS writing and register readback. Static storage avoids allocating the dump buffer in the fault path.

Slots correspond to `/core.1` through `/core.4`. Allocation rotates its starting slot and searches for a free buffer. Reusing these paths bounds the number of output files.

Calls with a current process consume the boot budget before slot acquisition, so busy slots and later failures also consume attempts. Slot exhaustion, build failure, write failure and budget exhaustion produce serial diagnostics. After a successful VFS write, the kernel obtains the summary's instruction and stack pointers by parsing the generated buffer through `coredump_read_gregs()`.

## Readers and checks

`c/apps/coreutils/readcore.c` prints registers, exception details, segment sizes and the `LOGIT` region table. It accepts a path and defaults to `/core.1`. Its parser, `c/apps/coreutils/corefmt.h`, is also used by the host checks.

An older comment in `tests/coredump.mk` describes the reader as missing from the disk image. Current packaging in `tests/agent.mk`, included by `Makefile`, builds and installs it as `/bin/readcore`.

`tests/coredump.mk` defines these checks:

| Target | Coverage |
|---|---|
| `test-coredump` | Note layouts, register order, memory contents, truncation and stack priority using the production builder with a model address space. |
| `test-coredump-os` | A guest exception, extraction from the written disk image, comparison with the fault frame and GDB register checks. |
| `test-ptrace-os` | Guest attach, register reads, remote memory transfers, detach and reattach, and refusal of an unrelated process. |

`test-coredump-negctl` substitutes the dumper's register context and checks that register assertions fail while named format and content assertions pass. `test-ptrace-negctl` removes ownership checks and checks that an unrelated process can read a stopped target. They are prerequisites of `test-coredump` and `test-ptrace-os`, respectively.

## Limitations

- Ptrace lacks single stepping, syscall tracing, tracing initiated by the child and interception of fatal signals. `c/kernel/exec/ptrace.h` describes the additional stop reporting these features need.
- Tracing stores one register snapshot per PID, without thread selection or a barrier confirming that every thread has stopped. Core dumps contain one general register record.
- Detach removes the link before the resume hook can apply pending `PTRACE_SETREGS` edits, discarding those edits.
- Ptrace rejects absent or swapped pages. Its validation also rejects writes to mappings marked read only, including pages awaiting resolution of copy on write.
- Core generation covers the fatal user CPU exception path. Default signal termination, failed signal frame delivery and kernel exceptions follow paths that skip core generation.
- FS base, GS base and the DS, ES, FS and GS selectors are recorded as zero. Core metadata uses UID/GID zero and the PID for process group and session identifiers. CR2 for other exceptions can retain an address from an earlier page fault.
- File mappings and shared segments contribute metadata without memory contents; absent anonymous pages are skipped. GDB can display omitted segment bytes as zeros. Consult the private region records, coverage counts and flags.
- Stack priority applies after header selection. Header exhaustion or a stack run larger than the remaining buffer can still omit stack bytes.
- If a page copy fails after the mapping check, `k_read_page()` fills that page with zeros. The dump counts those bytes as stored and has no marker identifying the failed copy.
- Attach authorization depends on credentials. The session starts with UID zero in `c/fs/vfs/vfs_cred.c`, permitting broad attachment until credentials change.

## See also

- [Signals](signals.md)
- [Process management](processes.md)
- [Virtual memory](virtual-memory.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/ptrace.h` | Requests, return codes and supported tracing scope. |
| `c/kernel/exec/ptrace.c` | Ownership, snapshots, register edits and remote memory access. |
| `include/abi/logit_abi.h` | Syscall number and memory transfer structure. |
| `c/kernel/exec/syscall.c` | Ptrace syscall dispatch. |
| `c/kernel/exec/signal/ksignal.c` | Signal posting and synchronous fault recording. |
| `c/kernel/exec/signal/ksigframe.c` | Stop hooks, resume and signal termination. |
| `c/kernel/exec/proc.c` | Tracing cleanup during exit. |
| `c/kernel/cpu/irq/interrupts.c` | Exception mapping and dump trigger. |
| `c/kernel/cpu/irq/interrupts.h` | Interrupt register frame layout. |
| `c/boot/isr.asm` | Register and FXSAVE capture. |
| `c/kernel/exec/signal/coredump.h` | Note layouts, register indices and resource bounds. |
| `c/kernel/exec/signal/coredump.c` | ELF construction, memory selection, buffer slots and output. |
| `c/kernel/mm/virt/vma.c` | Ordered region snapshots. |
| `c/kernel/mm/virt/vmm.c` | Mapping validation, page pinning and remote copies. |
| `c/fs/vfs/vfs_cred.c` | Process credentials and session identity. |
| `c/apps/coreutils/readcore.c` | Guest dump reader. |
| `c/apps/coreutils/corefmt.h` | Parser shared with host checks. |
| `tests/coredump.mk` | Core and ptrace targets and negative controls. |
| `tests/unit/coredump_test.c` | Format, register, memory content and truncation checks. |
| `tests/unit/corecheck.c` | Extracted dump checks and GDB comparison. |
| `tests/boot/run-core-test.sh` | Guest crash and persistent dump extraction. |
| `tests/boot/run-ptrace-test.sh` | Guest tracing and ownership checks. |
| `tests/agent.mk` | Reader build and disk packaging. |
| `Makefile` | CLI build rules and packaging integration. |