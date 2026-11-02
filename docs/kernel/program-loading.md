# Program loading

LogitOS loads ELF programs into a private user address space, builds their startup stack, and enters user mode. It supports fixed executables, static position independent executables, and programs that name an ELF interpreter. AEX packages supply an embedded ELF image to the same loader, while the exec path also dispatches shebang scripts. The main implementations are `c/kernel/exec/load/elf.c` and `c/kernel/exec/load/exec.c`.

## Loader interfaces

`aex_load_path()` in `c/kernel/exec/load/aex.c` recognises bare ELF files and AEX containers. A bare ELF starts at file offset zero; an AEX container supplies the embedded image’s offset and length.

Three structures in `c/kernel/exec/load/elf.h` connect file access, ELF loading, and process startup:

| Type | Purpose |
|---|---|
| `struct elf_reader` | Describes an image in memory or at a pathname, with offsets relative to the ELF image. |
| `struct elf_src` | Supplies the page cache handle and containing-file offsets for mappings backed by the executable. |
| `struct elf_image` | Returns entry addresses, mapped extent, startup metadata, TLS state, and mapping counts. |

`elf_load_reader()` handles both memory and file sources. For file sources, each `elf_read()` invocation owns scratch storage bounded at 512 KiB across I/O waits. This bounds temporary storage while copying large segments.

For AEX v3, `aex_load_path()` copies the package into a kernel buffer, then parses and loads that snapshot. Its ELF pages are copied from memory, binding the package identity to the bytes loaded into the process. See [Applications and AEX packages](../architecture/applications-aex.md).

## Replacing a process image

The `SYS_EXECVE` handler in `c/kernel/exec/syscall.c` enters the thread exclusion protocol, calls `proc_execve()`, and updates signal dispositions after success.

`proc_execve()` in `c/kernel/exec/load/exec.c` performs these stages:

1. Copy the pathname, arguments, and environment into kernel storage, resolve the pathname, and check execution permission.
2. Dispatch a shebang script when present, then inspect the binary header.
3. Create a replacement address space, load the executable and any ELF interpreter, and build the initial stack.
4. Publish the replacement through `proc_exec_space()`, switch to it, free the old address space, install TLS, and rewrite the saved syscall frame.

Preparation retains the old image. A loading, stack construction, or publication failure frees the replacement space and returns an error. Holding both spaces costs memory, but preserves the caller’s executable mappings when preparation fails. `proc_exec_space()` in `c/kernel/exec/proc.c` publishes the replacement under the process lock after checking the expected old space and teardown state.

Exec preserves the process ID, file descriptors, working directory, and capability grant. After success, `ksig_proc_exec()` in `c/kernel/exec/signal/ksignal.c` resets caught signal handlers to their default disposition. Ignored dispositions and the blocked signal mask survive.

Loading can sleep during allocation or I/O. Address space guards cover mapping and filling as one operation, protecting the loader’s stores against concurrent reclaim. `sched_use_address_space()` records the temporary space in the current thread and switches hardware state, allowing the scheduler to restore that target after a wait. These mechanisms are implemented in `c/kernel/mm/virt/mmguard.h` and `c/kernel/sched/sched.c`.

## ELF validation and mapping

`elf_check_header64()` accepts ELF64 images for x86-64 with little endian encoding and SysV or GNU OSABI identification. `elf_load_object()` validates header sizes, table bounds, segment arithmetic, alignment, load order, and entry coverage. Program headers drive loading; section table size, bounds, and index metadata receive consistency checks.

The user address windows are defined in `c/kernel/mm/mm.h`:

| Window | Address range |
|---|---|
| Legacy | `[0x40000000, 0x80000000)` |
| Wide | `[0x0000010000000000, 0x0000800000000000)` |

Each mapped range must fit within one window. The entry must lie in an executable `PT_LOAD`, with 64 MiB of address space available from the entry. The loader also checks for room above the segments for startup metadata and TLS.

A load segment whose end, rounded up to a page boundary, is at or below `0x40000000` is skipped. This accommodates link layouts that place ELF headers in low memory while preserving the shared kernel mappings there.

For eagerly copied segments, the loader allocates and clears frames, copies file bytes, and then applies final permissions. Clearing supplies zeroes for segment memory beyond its file contents. Adjacent segments can occupy different parts of one page; the loader allocates that page once and combines the segments’ permission flags.

`elf_file_runs()` identifies pages suitable for the page cache. Each eligible page belongs to one segment, has no write permission, lies entirely within the segment’s file contents, and has an aligned offset in the containing file. Partial pages follow the copy path, preserving the boundary between file contents and zeroed data.

Successful file reservations populate on first access through `c/kernel/mm/virt/fault.c`. Processes mapping the same executable can share cached frames. A failed file reservation sends the affected pages through eager copying.

Final page permissions derive from the covering segments’ flags. Non-executable pages receive NX when `cpu_prot_nx_usable()` permits it. That predicate in `c/kernel/cpu/prot.c` reads the active processor protection state.

## Static PIE

An `ET_DYN` image receives a load bias before segment validation and mapping. The placement algorithm in `c/kernel/exec/load/elf.c` selects aligned slots near 1 TiB for main programs and 1.5 TiB for interpreters. It adds the bias to the entry and relevant addresses in its working copy of the program headers.

For static PIE, `pie_dynamic()` translates the RELA table’s virtual address through the loadable segments’ file ranges. `pie_relocate()` validates the records before mapping, then repeats the checks while applying relocations after copying.

| Relocation | Behaviour |
|---|---|
| `R_X86_64_NONE` with symbol index zero | Skip the record. |
| `R_X86_64_RELATIVE` with symbol index zero | Store the load bias plus the signed addend. |

Each applied relocation destination must fit eight bytes within writable, non-executable segment data. The resulting pointer must lie in a user address window. Records are read in batches of 32, bounding temporary storage for large tables.

Relocations precede final segment permissions and `PT_GNU_RELRO` protection. The RELRO pass considers pages from the range’s start rounded down through its end rounded down. It removes write permission where all writable segment bytes on the page belong to the RELRO range. This preserves write access for neighbouring data that still needs it.

## ELF interpreters

`PT_INTERP` supplies an absolute pathname terminated by NUL. `elf_load_reader()` loads the main object, opens the interpreter, and loads it into the same address space. It checks the interpreter’s address span, including room for metadata, against the main image.

`elf_interpreter_open()` in `c/kernel/exec/load/interp.c` checks read and execution permission using the current credentials and opens a page cache handle. The interpreter passes through ELF validation as an `ET_DYN` object.

The loader result distinguishes the main entry from the first instruction to execute:

| Field in `struct elf_image` | Meaning |
|---|---|
| `entry` | Main program entry, supplied as `AT_ENTRY`. |
| `start_entry` | Initial execution address, belonging to the interpreter when present. |
| `interp_base` | Interpreter load bias, supplied as `AT_BASE`. |

With an interpreter, the kernel delegates relocation, TLS construction, and RELRO protection for both objects to the user runtime. It maps the objects and supplies their startup information. It also registers eagerly copied segment ranges and metadata pages as VMAs, allowing memory syscalls to find them and keeping later allocations clear of the metadata.

The program headers exposed to userland retain their original address fields. The runtime applies the load bias when interpreting those addresses.

## Thread-local storage

For static execution, `PT_TLS` describes the initial thread’s TLS template. The loader constructs the x86-64 variant II layout: TLS data lies below the thread pointer, and the thread control block starts at that pointer. It writes the self-pointer at `%fs:0` and zero at `%fs:8`.

For static PIE, the TLS initialiser comes from the relocated memory image. Copying those bytes preserves pointers adjusted during relocation. The implementation is in `c/kernel/exec/load/elf.c`.

`proc_execve()` installs the resulting thread pointer through `sched_set_fsbase()`. That function updates scheduler state and hardware together, so context switches preserve the installed value. New process launchers pass the pointer to `thread_create_user_tls()`. Both interfaces are in `c/kernel/sched/sched.c`.

Interpreted execution starts with a zero thread pointer, giving the user runtime control over its TLS layout.

## Shebang scripts

Before binary loading, `proc_execve()` passes the requested file’s first bytes to `shebang_parse()` in `c/kernel/exec/load/shebang.h`. The parser skips spaces and tabs after `#!`, reads the interpreter name, and treats the trimmed remainder of the line as one optional argument. It trims trailing spaces, tabs, and carriage returns, including the carriage return in a CRLF ending.

For a recognised script, the rebuilt argument vector contains:

1. The interpreter name.
2. The optional argument, when present.
3. The script pathname as supplied by the caller.
4. The caller’s arguments starting at `argv[1]`.

The exec path resolves the interpreter pathname and checks execution permission before loading it. The environment accompanies the rebuilt arguments. Relative interpreter paths resolve through the process pathname resolver.

The resolved binary pathname goes to `setup_cli_stack()`. For shebang execution, `AT_EXECFN` names the resolved interpreter, while the argument vector carries the script pathname. Both operations are in `c/kernel/exec/load/exec.c`.

## Initial user stack

`setup_user_stack()` in `c/kernel/exec/load/exec.c` builds the startup stack for CLI execution and GUI launches that use the same layout.

CLI stacks reserve 256 pages, or 1 MiB. Their initial top is 64 MiB above the main entry rounded down to a 1 MiB boundary. The builder raises the top as needed to place the entire stack above the loaded objects, metadata, and TLS.

The builder maps enough pages for the startup data, with a minimum of two pages for CLI stacks. The remaining reservation populates on demand. If the VMA reservation fails, the builder maps the full stack eagerly.

`do_anon()` in `c/kernel/mm/virt/fault.c` supplies zeroed pages as the stack grows. When allocation fails or reclaim stalls, `mm_fault_in()` can call `oom_fault_decide()` once after the fault attempt releases its address space guard. The policy can reap exited processes or mark a victim and wait within a bounded loop. An allowed retry walks the current page tables again because mappings can change during the wait. See [Page cache, reclaim and swap](page-cache-and-reclaim.md).

At entry, the stack pointer is aligned to 16 bytes and points at:

| Order toward higher addresses | Contents |
|---|---|
| First word | `argc` |
| Argument vector | `argv` pointers followed by NULL |
| Environment vector | `envp` pointers followed by NULL |
| Auxiliary vector | Tag/value pairs ending with `AT_NULL`, zero |
| Remaining startup storage | Alignment padding and copied strings |

`include/abi/logit_exec.h` defines the argument budget: 256 entries each for `argv` and `envp`, plus 16,384 bytes for their combined strings, including terminators. `copy_uvec()` checks the terminating pointer after a full vector. Shebang expansion appends its interpreter, optional argument, and script strings to the same kernel storage budget.

The auxiliary vector contains:

| Entries | Values |
|---|---|
| `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` | Main program’s header table address, entry size, and count |
| `AT_PAGESZ` | 4096 |
| `AT_BASE` | Interpreter load bias, or zero for static execution |
| `AT_ENTRY` | Main program entry |
| `AT_FLAGS`, `AT_SECURE` | Zero |
| `AT_UID`, `AT_EUID` | Current UID |
| `AT_GID`, `AT_EGID` | Current GID |
| `AT_RANDOM` | Address of 16 bytes supplied by the kernel random generator |
| `AT_EXECFN` | Address of the copied executable pathname |
| `AT_NULL` | End marker |

The ELF loader places a read-only information page above each object containing its original ELF header, program headers, and random bytes. `AT_PHDR` uses the main program’s header table within a user load segment when available, falling back to the metadata copy. This supplies accessible headers for layouts whose original header segment occupies skipped low memory.

`c/apps/crt0_cli.asm` reads `argc` and `argv`, publishes the environment pointer, calls `main`, and exits with its return value.

The GUI launcher in `c/kernel/gui/wm.c` calls `exec_interpreter_stack()` for interpreted images and AEX v3 packages. It uses the package’s stack hint, with a default of 1024 pages. The constructed argument vector contains the executable pathname and an optional launch argument. Legacy GUI startup uses `c/apps/crt0.asm`, which calls `app_main`.

## Limitations

- Each ELF object accepts at most 64 program headers. Its mapped `PT_LOAD` sizes, each rounded to page boundaries, sum to at most 256 MiB. TLS data is capped at 1 MiB, with alignment at most one page. Writable executable segments and executable stack requests are rejected.
- Static PIE accepts the RELA operations described above. Unsupported relocation types and dynamic tags are rejected by `c/kernel/exec/load/elf.c`.
- ELF interpretation supports one level, with a bare `ET_DYN` interpreter and a pathname of at most 128 bytes including its terminator. Duplicate and nested `PT_INTERP` entries are rejected. AEX v3 loading rejects interpreted images; its pathname snapshot is capped at 16 MiB.
- Shebang dispatch supports one script level through `proc_execve()`. Its interpreter must be a binary. The first line must end within the 128-byte parsing window, or at EOF when the complete file fits in that window.
- Oversized arguments return `LOGIT_EXEC_E2BIG`, whose value is `-7`. Shebang expansion must fit both the entry limit and the remaining string budget.
- Exec is refused when the process has multiple live threads. `uthread_exec_begin()` in `c/kernel/sched/uthread.c` excludes sibling creation throughout replacement.
- Placement is deterministic and provides no ASLR. The interpreter startup contract does not supply general shared-library symbol resolution or a foreign syscall ABI. `tests/unit/ptinterp_runtime.c` is an acceptance interpreter supporting relative relocations and one TLS block.

## See also

- [Process management](processes.md)
- [Virtual memory](virtual-memory.md)
- [Page cache, reclaim and swap](page-cache-and-reclaim.md)
- [Signals](signals.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/exec/load/elf.c` | ELF validation, mapping, PIE placement and relocation, interpreter loading, RELRO, metadata, and TLS |
| `c/kernel/exec/load/elf.h` | Loader interfaces, result structures, image limits, and auxiliary-vector tags |
| `c/kernel/exec/load/exec.c` | Exec transaction, argument copying, script dispatch, stack construction, and process launchers |
| `c/kernel/exec/load/interp.c` | Interpreter permission checks and file access |
| `c/kernel/exec/load/shebang.h` | Shebang parser |
| `c/kernel/exec/load/aex.c` | Container parsing, bare ELF recognition, package snapshots, and loading entry points |
| `include/abi/logit_exec.h` | Argument limits and oversized-input error |
| `c/kernel/exec/syscall.c` | Exec syscall dispatch, thread exclusion, and signal updates |
| `c/kernel/exec/proc.c` | Replacement address space publication |
| `c/kernel/exec/signal/ksignal.c` | Signal dispositions across exec |
| `c/kernel/sched/uthread.c` | Exec exclusion against sibling threads |
| `c/kernel/sched/sched.c` | Temporary address space tracking, user thread creation, and TLS installation |
| `c/kernel/mm/mm.h` | User address windows |
| `c/kernel/mm/virt/mmguard.h` | Address space transaction guards |
| `c/kernel/mm/virt/fault.c` | Demand population of stack and executable file pages, with memory shortage retries |
| `c/kernel/mm/reclaim/oom.c` | OOM decisions, victim selection, and bounded waits |
| `c/kernel/cpu/prot.c` | Active processor protection predicates |
| `c/apps/crt0_cli.asm` | CLI startup stack consumption |
| `c/apps/crt0.asm` | Legacy GUI startup |
| `c/kernel/gui/wm.c` | GUI loading and stack selection |
| `tests/unit/ptinterp_host.c` | Interpreter validation, collision, and allocation failure cases |
| `tests/unit/ptinterp_runtime.c` | Acceptance interpreter |
| `tests/unit/shebang_test.c` | Shebang parser cases |