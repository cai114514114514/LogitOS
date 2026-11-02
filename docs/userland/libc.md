# The C library

LogitOS provides an application C library in `c/apps/libc`, called mini-libc in source comments and build rules. It supplies selected ISO C, POSIX, GNU and BSD interfaces, combining computation in user space with adapters for the LogitOS syscall ABI. Public headers describe the application interface; the implementations establish the behavior and porting constraints covered here.

## Scope and standards coverage

| Area | Facilities | Main sources |
|---|---|---|
| Core C | Strings, allocation, environment variables, sorting, formatted I/O, calendar conversion and wide characters | `c/apps/libc/src/string.c`, `c/apps/libc/src/malloc.c`, `c/apps/libc/src/stdlib.c`, `c/apps/libc/src/stdio.c`, `c/apps/libc/src/scanf.c`, `c/apps/libc/src/time.c`, `c/apps/libc/src/wchar.c` |
| C11 | Threads, synchronization and conversion between multibyte text and UTF-16 or UTF-32 characters | `c/apps/libc/src/threads.c`, `c/apps/libc/src/uchar.c` |
| Selected C23 interfaces | Bit utilities and checked integer arithmetic | `c/apps/libc/include/stdbit.h`, `c/apps/libc/include/stdckdint.h` |
| POSIX interfaces | Descriptors, directories, processes, signals, polling, sockets, locale objects and encoding conversion | `c/apps/libc/src/io.c`, `c/apps/libc/src/dirstat.c`, `c/apps/libc/src/signal.c`, `c/apps/libc/src/poll.c`, `c/apps/libc/src/socket.c`, `c/apps/libc/src/locale_ext.c`, `c/apps/libc/src/iconv.c` |
| GNU and BSD extensions | Memory streams, long command options, packed argument and environment vectors, and additional string operations | `c/apps/libc/src/memstream.c`, `c/apps/libc/src/getopt.c`, `c/apps/libc/src/argz.c`, `c/apps/libc/src/envz.c`, `c/apps/libc/src/string_ext.c` |

Decimal parsing and formatting share the integer arithmetic engine in `c/apps/libc/src/dtoa.c`. In `c/apps/libc/src/stdlib.c`, `strtof` requests conversion at float precision, avoiding the extra rounding that can occur when decimal input first becomes a double.

Basic float math routines live in `c/apps/libc/src/math_basic.c`. `Makefile` compiles the musl math subset under `third_party/libm` into `LIBM_OBJ`, alongside the application library’s `LIBC_OBJS`. Application link rules select the objects they need.

## Building and startup

`Makefile` collects C and assembly sources under `c/apps/libc/src` into `LIBC_OBJS`. Application compilation uses `UCFLAGS`, including the freestanding x86-64 target and C11 language mode. The include path places `c/apps/libc/include/uonly` before the shared directory list so the application scheduling header takes precedence over the kernel header with the same basename.

`tools/mksysroot.py`, called through `tests/sysroot.mk`, packages headers, a static library archive and startup objects for TinyCC. It flattens the `uonly` directory into the installed include directory and assembles the archive from libc and math objects.

The CLI startup code in `c/apps/crt0_cli.asm` reads the argument vector from the initial stack, publishes the environment through `environ` and `__libc_environ_hook`, and calls `main`. Environment functions in `c/apps/libc/src/stdlib.c` adopt that vector on first use and maintain a managed vector as the environment changes.

Calling `exit` runs registered handlers in reverse order, flushes streams and invokes the kernel exit syscall. File streams, memory streams and pipe streams share the registry in `c/apps/libc/src/stdio.c`. Their common representation and internal interfaces live in `c/apps/libc/src/libc_internal.h`.

Some CLI programs use the inline helpers in `c/apps/clib.h`. A program’s includes and link rule identify which runtime facilities it uses.

## How syscall wrappers work

The wrappers issue `int 0x80` with the call number in `rax`, arguments in `rdi`, `rsi` and `rdx`, and the result returned in `rax`. `include/abi/logit_abi.h` defines the syscall contracts and request structures. The dispatcher in `c/kernel/exec/syscall.c` routes calls to kernel subsystems.

The library translates C interfaces into the kernel’s argument layouts and converts the results:

| Interface | Translation |
|---|---|
| `read`, `write` | Convert the kernel’s nonblocking and interrupted results to `-1` with `EAGAIN` or `EINTR`. |
| `close` | Report an invalid descriptor as `EBADF` and failure during final file writeback as `EIO`. |
| `mmap` | Convert protection bits and package file mapping arguments in `struct logit_mmap_file_req`. |
| IPv4 sockets | Convert addresses and ports between network byte order and the kernel ABI’s host byte order. |
| `select` | Convert descriptor bitmaps into a `poll` request, then rebuild the result sets. |
| `statvfs` | Query filesystem capacity through the kernel and translate the returned fields into the public structure. |

These adapters live in `c/apps/libc/src/io.c`, `c/apps/libc/src/mman.c`, `c/apps/libc/src/socket.c`, `c/apps/libc/src/poll.c` and `c/apps/libc/src/fsmisc.c`. Keeping `select` in libc lets the library own the bitmap representation while the kernel supplies descriptor readiness.

Error conversion follows each call’s return contract. A generic kernel failure receives an error chosen by the wrapper; call families with more detailed results receive corresponding translations. Socket wrappers share their conversion function in `c/apps/libc/src/socket.c`.

Signal registration supplies a return trampoline from the application’s executable code. `c/apps/libc/src/signal.c` passes its address to the kernel so a returning handler invokes `SYS_SIGRETURN`. The interrupt entry in `c/kernel/cpu/irq/interrupts.c` intercepts that call and invokes context restoration in `c/kernel/exec/signal/ksigframe.c`.

The native diagnostic call `SYS_SCHED_OBSERVE` uses the same register convention: `rdi` selects a command, `rsi` points to a request and `rdx` carries its size. `include/abi/logit_sched_observe.h` defines the versioned request layouts. `c/kernel/sched/observe/observe_control.c` checks request size and version before dispatching commands to begin, stop, inspect or read a measurement run.

## Threads and synchronization

`c/apps/libc/src/pthread.c` implements thread creation, joining, detachment, identity and exit over the kernel services in `c/kernel/sched/uthread.c`. Synchronization includes normal, recursive and error checking mutexes, condition variables, reader/writer locks, reusable barriers, spin locks, once initialization, thread data keys and unnamed semaphores. Pthread operations that report errors return the error number directly; semaphore operations use `-1` and `errno`.

A normal mutex acquires its uncontended state with an atomic operation in user space. Contention parks through `SYS_FUTEX`. Condition variables use a sequence counter: a waiter reads it before releasing the mutex and asks the futex to wait for that value. A notification during this transition changes the sequence, allowing the kernel’s comparison to catch the event before sleeping.

Thread creation reserves the stack, TLS image and control data in one anonymous mapping. It applies `mprotect` to the bottom of the reservation to form a guard. Retaining the reservation prevents later mappings from occupying the guard’s address range. The default requested stack is 8 MiB, with a guard of 4096 bytes; both defaults are defined in `include/abi/logit_abi.h`.

The entry stub in `c/apps/libc/src/pthread_entry.asm` installs the thread pointer before calling C code. TLS data sits below that pointer, and `%fs:16` holds a pointer to libc’s control data. The main thread reuses a thread pointer supplied by the ELF loading path when one is present.

`c/apps/libc/logit_tls.ld` publishes the template bounds used when libc constructs TLS blocks for new threads. `tests/thread.mk` demonstrates linking with this script. Thread data key destructors run during `pthread_exit`, with up to four passes so a destructor can install another value for destruction.

The C11 interface in `c/apps/libc/src/threads.c` adapts pthread objects and operations. Its creation trampoline converts an integer result into the pthread return representation, and the adapters translate pthread error numbers into C11 status values.

The allocator in `c/apps/libc/src/malloc.c` uses segregated free lists and boundary tags for allocation and coalescing. When the pthread implementation is linked, lock hooks protect allocator metadata with one lock.

## Locale and character conversion

`setlocale` accepts `"C"`, `"POSIX"` and the empty string, returning the canonical name `"C"`. `localeconv` supplies that locale’s numeric and monetary fields. Both live in `c/apps/libc/src/locale.c`.

`c/apps/libc/src/locale_ext.c` implements `newlocale`, `duplocale`, `freelocale` and `uselocale` around a shared C locale handle. The selected handle is stored per thread. Classification and collation adapters in `c/apps/libc/src/ctype_l.c` call the corresponding C locale operations.

Multibyte conversion uses UTF-8, with `wchar_t` occupying 32 bits and representing a Unicode code point. `mbrtowc` in `c/apps/libc/src/wchar.c` preserves partial decoding state and rejects malformed sequences, overlong encodings, surrogates and values above U+10FFFF. The C11 character conversion functions in `c/apps/libc/src/uchar.c` and the UTF-8 paths in `c/apps/libc/src/iconv.c` reuse this codec.

`iconv` implements the following encoding names:

| Encoding names | Representation and byte order |
|---|---|
| UTF-8 | Variable length Unicode encoding |
| UTF-16, UTF-32 | Input consumes an initial byte order mark and adopts its order; input without a mark defaults to little endian. Output emits a little endian mark before the first character. |
| UTF-16LE, UTF-16BE, UTF-32LE, UTF-32BE | Named byte order; a byte order mark is character data |
| UCS-2 | Little endian, restricted to the Basic Multilingual Plane with surrogates rejected |
| UCS-4 | Big endian |
| ASCII, US-ASCII | ASCII |
| ISO-8859-1, LATIN1 | Latin-1 |
| WCHAR_T | Alias for UTF-32LE |

Encoding names are matched without regard to case, hyphens or underscores. Conversion updates the caller’s input and output pointers and remaining lengths, including progress made before an error. `E2BIG` reports insufficient output space, `EINVAL` reports incomplete input, and `EILSEQ` reports invalid or unrepresentable data.

The destination option `//IGNORE` discards invalid or unrepresentable input and continues. A call that reaches the end after discarding data returns an error with `EILSEQ`. Resetting a descriptor clears its byte order mark state.

Message functions in `c/apps/libc/src/intl.c` return the supplied message text and select singular or plural text from the count. Domain and binding setters retain queryable state.

## Limitations

- Standards coverage is partial. Regex alternation chooses the first successful alternative. Although `c/apps/libc/include/regex.h` promises to reject backreferences, the parser in `c/apps/libc/src/regex.c` treats `\1` as a literal `1`.
- Thread safety varies by interface. `errno` is a process global in `c/apps/libc/src/io.c`; stream state and registration in `c/apps/libc/src/stdio.c` lack internal locking; decimal parsing and formatting share a static working buffer in `c/apps/libc/src/dtoa.c`.
- `pthread_cancel`, `pthread_setcancelstate`, `pthread_setcanceltype` and `pthread_kill` return `ENOSYS`; `pthread_testcancel` is a no-op. Synchronization shared between processes is unsupported because futex keys incorporate the address space. The VMA table has 32 entries, and a thread reservation with its default guard consumes two areas. See `c/apps/libc/src/pthread.c`, `c/kernel/sched/uthread.c` and `c/kernel/mm/virt/vma.h`.
- Locale support covers C formatting and ASCII classification. Catalog loading and transliteration are absent; requesting `//TRANSLIT` fails with `EINVAL`. `nl_langinfo(CODESET)` reports `"ANSI_X3.4-1968"` while multibyte conversion uses UTF-8. See `c/apps/libc/src/locale.c`, `c/apps/libc/src/wchar.c`, `c/apps/libc/src/langinfo.c` and `c/apps/libc/src/iconv.c`.
- Writable file mappings are rejected with `ENOTSUP`; `msync`, `mlock` and `munlock` return `ENOSYS`. Initial `mmap` requests floor `PROT_NONE` to read access; inaccessible guards require `mprotect`. See `c/apps/libc/src/mman.c`.
- `waitpid` exposes the kernel’s raw exit code as the status value. For names without a slash, `execvp` tries `/bin` and then the literal program name. The stored `FD_CLOEXEC` flag has no effect on descriptor closure at exec. See `c/apps/libc/src/io.c` and `c/kernel/exec/proc.c`.
- Unimplemented operations include `flock`, `getppid`, `fchdir`, `fstatvfs` and file timestamp setters. Valid requests reach `ENOSYS`; argument and object checks can report earlier errors. See `c/apps/libc/src/uio.c`, `c/apps/libc/src/io.c` and `c/apps/libc/src/fsmisc.c`.
- `SYS_SCHED_OBSERVE` has no libc wrapper. Its native request ABI is defined in `include/abi/logit_sched_observe.h`.
- `strtold` computes at double precision, and alignment requests above 16 bytes fail in `aligned_alloc` and `posix_memalign`. Returning from CLI `main` invokes the kernel exit syscall directly, bypassing libc’s registered handlers and stream flush. See `c/apps/libc/src/stdlib.c` and `c/apps/crt0_cli.asm`.

## Validation sources

The repository defines host comparisons and guest fixtures for different parts of the library.

| Entry point | Intended coverage | Definition |
|---|---|---|
| `make test-libc-diff` | Numeric conversion, formatting and other computation against a host reference, including a sabotaged conversion control | `Makefile`, `tests/unit/libc_diff_test.c`, `tests/unit/libc_sabotage.c` |
| `make test-libc-host` | Additional interface comparisons and contract assertions, with platform dependent skips | `tests/libc.mk` |
| `make test-libc`, `make test-libc2` | Application library behavior in the guest | `Makefile`, `tests/libc.mk` |
| `make test-thread` | Threads, mutexes, barriers, TLS and C11 adapters in the guest | `tests/thread.mk`, `tests/guest/sched/thrtest.c` |

## See also

- [System calls](../architecture/system-calls.md)
- [Processes and threads](../architecture/processes-and-threads.md)
- [Program loading](../kernel/program-loading.md)
- [Testing](../architecture/testing.md)

## Source map

| File or directory | Contents |
|---|---|
| `c/apps/libc/include/` | Public application headers and interface contracts |
| `c/apps/libc/src/string.c`, `c/apps/libc/src/string_ext.c` | String and memory operations |
| `c/apps/libc/src/io.c` | Core syscall wrappers, process interfaces and `errno` |
| `c/apps/libc/src/dirstat.c`, `c/apps/libc/src/fsmisc.c`, `c/apps/libc/src/uio.c` | Directory, metadata, filesystem and vector I/O interfaces |
| `c/apps/libc/src/stdio.c`, `c/apps/libc/src/scanf.c`, `c/apps/libc/src/dtoa.c` | Streams, formatted I/O and numeric conversion |
| `c/apps/libc/src/libc_internal.h`, `c/apps/libc/src/memstream.c` | Shared stream representation and memory stream backends |
| `c/apps/libc/src/malloc.c`, `c/apps/libc/src/stdlib.c` | Allocation, environment handling, conversion and termination |
| `c/apps/libc/src/time.c`, `c/apps/libc/src/math_basic.c` | Calendar conversion and basic float math |
| `c/apps/libc/src/getopt.c`, `c/apps/libc/src/argz.c`, `c/apps/libc/src/envz.c` | Option parsing and packed vectors |
| `c/apps/libc/src/pthread.c`, `c/apps/libc/src/threads.c` | Pthread implementation and C11 adapters |
| `c/apps/libc/src/pthread_entry.asm`, `c/apps/libc/logit_tls.ld` | Thread entry and TLS template layout |
| `c/apps/libc/src/locale.c`, `c/apps/libc/src/locale_ext.c`, `c/apps/libc/src/ctype_l.c` | Locale formatting, objects and adapters |
| `c/apps/libc/src/wchar.c`, `c/apps/libc/src/uchar.c`, `c/apps/libc/src/iconv.c` | Unicode and encoding conversion |
| `c/apps/libc/src/langinfo.c`, `c/apps/libc/src/intl.c` | Locale metadata, catalog interfaces and message fallback |
| `c/apps/libc/src/mman.c`, `c/apps/libc/src/poll.c`, `c/apps/libc/src/socket.c`, `c/apps/libc/src/signal.c` | Memory, readiness, socket and signal adapters |
| `c/apps/libc/src/regex.c` | Regex parsing and matching |
| `c/apps/crt0_cli.asm`, `c/apps/clib.h` | CLI startup and inline application helpers |
| `include/abi/logit_abi.h`, `c/kernel/exec/syscall.c` | Syscall contracts and dispatch |
| `include/abi/logit_sched_observe.h`, `c/kernel/sched/observe/observe_control.c` | Scheduler observation request ABI and command handling |
| `c/kernel/cpu/irq/interrupts.c`, `c/kernel/exec/signal/ksigframe.c` | Signal return entry and context restoration |
| `c/kernel/sched/uthread.c`, `c/kernel/exec/load/elf.c`, `c/kernel/exec/load/exec.c` | Kernel thread services and initial TLS setup |
| `c/kernel/exec/proc.c`, `c/kernel/mm/virt/vma.h` | Process status handling and VMA capacity |
| `Makefile`, `tools/mksysroot.py`, `tests/sysroot.mk`, `third_party/libm/` | Object collection, sysroot assembly and musl math subset |
| `tests/libc.mk`, `tests/thread.mk`, `tests/guest/sched/thrtest.c` | Host and guest validation entry points |