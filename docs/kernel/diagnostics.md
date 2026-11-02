# Kernel diagnostics

LogitOS provides formatted console output, retained logs, diagnostic files, profiling, panic reports, and self-tests through `c/kernel/diag`. These facilities support inspection from the guest shell, collection of boot evidence, and controlled exercises of failure paths. Hardware reports and an optional desktop panel extend the same diagnostics to machines accessed through their display.

## Formatted output

`kprintf()` in `c/kernel/diag/kprintf.c` sends characters to VGA text output, COM1, and the retained log. While the boot console is active, `bootcon_putc()` also draws those characters into the framebuffer using the compiled font in `c/kernel/diag/hwscreen_font.h`. `bootcon_stop()` hands the display to the desktop or paged boot report.

The formatter supports `%s`, `%c`, `%d`, `%u`, `%x`, `%p`, and `%%`; numeric field widths; the `-` and `0` flags; and `l`, `ll`, and `z` length modifiers. `ksnprintf()` and `kvsnprintf()` share this formatter. For a positive buffer capacity, they reserve space for a terminating NUL and return the number of bytes stored. Diagnostic report builders use this return value to append fields.

A message lock serializes each formatted call across CPUs. `serial_puts()` shares this lock, and the UART driver protects its queue and register accesses with device locks. `c/drivers/core/io_lock.h` implements the lock and saved interrupt-state protocol.

Earlier serial output waited for the UART during each byte. The current implementation in `c/drivers/char/serial.c` queues output in a 64 KiB transmit ring and writes a burst when the hardware is ready. This keeps waiting for wire transmission out of the formatting critical section. COM1 is configured for 115200 baud, eight data bits, no parity, and one stop bit. FIFO detection permits bursts of up to 16 bytes.

`serial_tx_tick()` continues draining the queue from vector 32 timer interrupts on every CPU. It tries the queue lock and returns when another producer holds it. `serial_tx_stats()` exposes queued, sent, and dropped byte counts together with queue occupancy. After a loss, a subsequent producer queues a notice when sufficient space becomes available.

The formatter records calls that arrive with incorrect stack alignment through `kprintf_misaligned_calls()` and `kprintf_misaligned_caller()`. `/dev/kstat` exposes the count and last caller address. Counter updates avoid entering the logger recursively.

## Retained logs

`c/kernel/diag/klog.c` assembles text in a static buffer for each CPU. A newline commits a nonempty line; carriage returns are discarded. `klog_flush_partial()` commits unfinished lines during panic reporting.

`struct klog_rec`, defined in `c/kernel/diag/klog.h`, contains:

| Field | Meaning |
|---|---|
| `seq` | Sequence number assigned at commit |
| `ms` | Commit time from `timer_ms()` |
| `cpu` | CPU that assembled the line |
| `level` | Severity |
| `len` and `text` | Retained text length and contents |
| `dropped` | Characters truncated from this line |

Earlier documentation described a rolling capacity of 512 records. The current `KLOG_SLOTS` is 2,048, and each record holds up to `KLOG_TEXT`, or 112 text bytes. Wraparound replaces the oldest record. Statistics track committed records, rolling records overwritten, truncation, and records at each severity.

`KLOG_BOOT_SLOTS` preserves the first 2,048 records in another fixed array. The header records the motivation: periodic telemetry had displaced all boot lines after 77 seconds on an X79-VG4. `klog_get_boot()` retrieves these pinned records after rolling storage has overwritten them.

`klog_render()` emits pinned records that have aged out of the ring, a marker counting any intervening gap, and then rolling records. Overlapping ranges appear once. Rendered lines carry a timestamp, severity letter, CPU number, and any truncation count.

Interrupt masking protects each CPU’s line assembly. A ring lock protects publication, record copying, and statistics. Fixed storage and memory operations within the ring critical section allow the logging path to operate from interrupt handlers and callers holding other kernel locks.

Severity helpers are defined in `c/kernel/diag/klog.h`:

| Helper | Level |
|---|---|
| `kerr()` | `KL_ERR` |
| `kwarn()` | `KL_WARN` |
| `kinfo()` | `KL_INFO` |
| `kdebug()` | `KL_DEBUG` |

Lower level numbers represent greater urgency. Levelled messages always enter retained storage and reach the console when their level is at or below its threshold, initially `KL_INFO`. The helpers append a newline when the format string lacks one.

Plain `kprintf()` records at `KL_INFO` and reaches the console at every threshold. `klog_ring()` provides retained output that bypasses console sinks. Scheduler measurement summaries use this path to preserve the output-interference counters they report.

## Diagnostic files

`c/kernel/diag/kdiag.c` implements four synthetic files. The hooks in `c/fs/vfs/vfs.c` dispatch these paths before mounted filesystems and include their names in `/dev` enumeration. Using the file interface gives existing shell tools access to diagnostics.

| File | Interface |
|---|---|
| `/dev/kmsg` | Read formatted retained records |
| `/dev/kstat` | Read kernel counters and process summaries |
| `/dev/ktrigger` | Write diagnostic commands |
| `/dev/kprof` | Write profiling commands and read its report |

Basic inspection uses:

```sh
cat /dev/kmsg
cat /dev/kstat
```

`/dev/kstat` reports uptime, ticks, timer frequency, online CPUs, context switches, physical memory totals, log statistics, warning and panic counters, stack-alignment diagnostics, profiling totals, and process snapshots.

`kdiag_size()` renders content into a static 512 KiB buffer. `kdiag_read()` reuses the rendering when the cached path matches, allowing a size query and following read to use the same text as new records arrive. The VFS checks write permission before dispatching diagnostic commands.

`c/kernel/init/kmain.c` calls `kdiag_init()` after timer and physical memory initialization. This installs the logging and peer-halt interrupt gates and initializes the profiler.

## Boot and hardware reports

`bootmark()` in `c/kernel/diag/bootmark.c` logs a numbered boot stage and draws a colored square in the framebuffer. Its stage table covers CPU setup, memory, storage, networking, desktop initialization, SMP, and driver binding. Early drawing can use a temporary framebuffer mapping; boot removes it before physical memory initialization and resumes drawing after framebuffer setup.

`hwreport_dump()` in `c/kernel/diag/hwreport.c` collects the device registry, CPU topology, physical memory totals, DIMM SPD inventories, root-device information, and boot milestones. Milestone details describe the stage reached, including input initialization and submission of the first desktop frame.

The hardware report retains its `[hw]` transcript in a 64 KiB buffer. After a successful root mount, `c/kernel/diag/hwreport_log.c` saves it under `/var/log` using names formatted as `boot-%llu.log`. The largest canonical number among numbered regular files determines the next number. Later milestones update the same boot’s file. Creation of the new report precedes pruning to retain the latest 20 numbered reports.

After a failed root mount, `c/kernel/init/kmain.c` collects the board report and enters `hwscreen_report()`. `c/kernel/diag/hwscreen.c` displays summary lines followed by the rolling log, wrapping text with the compiled bitmap font. Each page remains visible for `HWSCREEN_PAGE_MS`, or 12 seconds. Each new cycle collects the ring again, providing pages suitable for photographs.

`make HWHUD=1` enables the desktop hardware panel through `LOGIT_HWHUD`. The window manager calls `hwhud_tick()` outside its state lock. The panel samples at most once a second and displays framebuffer and GPU information, CPU and clock readings, compositor timing, serial counters, interrupt and USB activity, network status, audio status, and scheduler observation counters.

`c/kernel/diag/hwhud_panel.h` defines placement and visibility decisions. Pressing the title row with the left mouse button collapses or expands the panel. When system overlays appear, the panel requests compositor damage for its former rectangle so those overlays remain accessible.

## Sampling profiles

`c/kernel/diag/kprof.c` samples instruction pointers to identify frequently executing code. Start recording, exercise the workload, then stop and read the report:

```sh
echo start > /dev/kprof
```

```sh
echo stop > /dev/kprof
cat /dev/kprof
```

A successful start resets measurements and enables sampling and span recording. The boot processor’s local APIC timer drives `KPROF_VEC`, vector 237. Its handler sends the same vector to the other online CPUs. The interrupt stub preserves general registers and floating-point state before calling the sampler directly.

Timer programming occurs immediately when the request runs on the boot processor. Requests from another CPU use a timer callback serviced on the boot processor. Application processors retain their local APIC timers for scheduler preemption.

Kernel and user instruction pointers occupy different hash tables. Atomic slot publication and hit increments support simultaneous sample handlers. Each CPU also counts samples, providing a check against the shared histogram.

| Report field | Meaning |
|---|---|
| `samples` | Samples counted by handlers |
| `recorded` | Hits recovered from both address tables |
| `overflow` | Samples that exhausted the hash probe budget |
| `cores_sampled` | Bitmap of CPUs that produced samples |
| `sample_hz_measured` | Aggregate samples per second over the profile interval |
| `timer_arm` | Timer programming counters and pending request |
| `KPROF_INTEGRITY` | Comparison of handler counts with hits and accounted losses |

The initial APIC timer count is 60,000. `start N` selects another count, which subsequent starts retain. The report calculates the achieved sampling rate from collected samples and elapsed time.

`tools/ksymbolize.py` resolves addresses against a kernel linker map and groups hits by function. Its `--user` option accepts an application ELF for user addresses. Use symbol files from the build that produced the profile.

## Named spans

`KPROF_BEGIN(name)` and `KPROF_END(name)` in `c/kernel/diag/kprof.h` bracket an operation. The timestamp lives in a local variable, supporting nested spans and completion after migration to another CPU.

Each span records its count, total nanoseconds, maximum duration, and calculated average. An outer span includes nested work and time spent preempted. Spans read `time_mono_raw_ns()` to avoid updating the shared monotonic clamp in `time_mono_ns()`, implemented in `c/kernel/core/ktime.c`. Completion clamps a backward clock delta to zero.

Applications can submit a duration measured in nanoseconds:

```sh
echo "span tls 12345678" > /dev/kprof
```

`spans 0` and `spans 1` control timestamp capture at span begins. Submitted durations enter the accumulator directly. `reset` clears measurements while retaining interned names, preserving slots cached by span macros.

## Scheduler observation

Scheduler event recording lives in `c/kernel/sched/observe/`. `SYS_SCHED_OBSERVE`, defined in `include/abi/logit_abi.h`, accepts the commands and requests in `include/abi/logit_sched_observe.h`. The interface starts and stops runs, retrieves status, and exports CPU event streams, the input ledger, coalesced groups, and metadata.

Starting a run requires root credentials. `c/kernel/sched/observe/observe_control.c` reserves the recording pools before publishing the run. Scheduling and interrupt hooks record wakeups, timer expiry, context switches, execution starts, and interrupt activity.

`c/kernel/sched/observe/flight_recorder.c` provides 65,536 ring slots for each recorded CPU. Readers validate a slot’s version before and after copying its atomic payload. Status distinguishes essential evidence losses from overwritten flight events.

Console output and writes to the UART transmit register increment interference counters and invalidate an active measurement. UART accounting includes bytes queued before the run and transmitted during it. `c/kernel/sched/observe/observe_hud.c` formats these counters for the hardware panel. Inactive recording hooks use activity checks to skip clock reads and event collection.

## Panic reports and warnings

`panic()` and failed `KASSERT()` checks enter the reporter in `c/kernel/diag/panic.c`. Assertions supply the expression, source file, and line. `KASSERT_MSG()` first emits its context through warning reporting, then reports the assertion failure.

The reporter masks interrupts, requests that peer CPUs halt, switches console and log storage into panic mode, and flushes partial lines. It emits:

1. `LOGIT_PANIC` and the CPU number.
2. The reason and captured machine state.
3. A backtrace.
4. Up to 40 preceding records still available in the rolling log.
5. `LOGIT_PANIC_END`, followed by a halt loop.

Panic takeover bypasses console locks and resets the ring lock because the failing context may hold them. The reporter saves the log sequence before printing its body, allowing the tail dump to select preceding events. Tail output goes directly to VGA and serial sinks.

`serial_panic_takeover()` makes subsequent serial writes drain synchronously with bounded UART polling. The compact exception reporter in `c/kernel/cpu/irq/interrupts.c` calls `serial_flush_blocking()` before halting.

For a C call to `panic()`, the captured instruction pointer is the caller’s return address. Frame and stack pointers describe the panic function’s frame; flags, segment selectors, and control registers are captured during reporting. `struct kregs`, defined in `c/kernel/diag/panic.h`, uses `have_gpr` to identify general registers supplied by an exception frame.

The unwinder checks frame alignment and readable memory, bounds the walk relative to the captured stack pointer, and requires frame pointers to advance toward higher addresses. Return addresses must fall within the kernel image. A call-instruction heuristic marks questionable frames with `?`. A chain containing zero or one frame triggers a stack scan labelled as candidates. The kernel’s `-fno-omit-frame-pointer` flag in `Makefile` supports this walk.

Machine checks use a dedicated vector 18 handler installed by `c/kernel/cpu/irq/idt.c`. `c/kernel/cpu/power/mce.c` first prints raw serial reports for valid machine-check banks, including address and miscellaneous registers when their validity bits permit. Fatal classifications enter `panic()` after this initial report.

`WARN()` reports a message and a short backtrace, then returns. `WARN_ON()` reports when its condition is true and evaluates to that condition. `WARN_ON_ONCE()` uses a static suppression flag at each invocation site. Warning counts appear in `/dev/kstat`.

## Diagnostic probes and self-tests

Commands written to `/dev/ktrigger` are implemented in `c/kernel/diag/kdiag.c`:

| Command | Behavior |
|---|---|
| `bt` | Print a backtrace and return |
| `warn` | Exercise `WARN()` |
| `warn_on` | Exercise `WARN_ON()` and report continued execution |
| `loglevel N` | Set the console severity threshold |
| `logstorm N` | Generate records and check retained record integrity |
| `irqstorm N` | Generate thread and interrupt records through local APIC IPIs |
| `netwedge MS [wm]` | Park network polling; `wm` selects the TCP timer control path |
| `assert` | Fail an assertion and enter the panic halt path |
| `panic MSG` | Report the supplied message and enter the panic halt path |

`assert` and `panic MSG` terminate execution through the reporter. The interrupt logging probe uses vector 239. Its records repeat a counter and include CPU identity; verification compares these fields to detect torn text. Reports distinguish interrupt deliveries from surviving records because rolling storage can overwrite probe output.

The profiler implements these measurement commands:

| Command | Measurement |
|---|---|
| `selftest [ms]` | Compare sampled shares with clock measurements for interleaved workloads constructed with a 70/20/10 iteration split |
| `overhead [ms]` | Compare identical work with sampling disabled and enabled, then measure span costs |
| `smpcheck` | Compare handler counts with histogram hits and accounted losses |

The self-test permits six percentage points between sampled and clock-measured shares. It also checks the clock-measured split against 70/20/10 with a five-point tolerance. The guest profiling harness requires at least 500 samples inside the workload.

`wait_selftest_start()` launches a diagnostic thread from `sched_init()`. `c/kernel/diag/wait_selftest.c` exercises FIFO wake-one, wake-all, semaphore transfers, mutex exclusion, deferred work that sleeps, and polling versus blocking over a 200 ms interval. Deadline checks report stalled phases. Its final marker is `WAITQ_SELFTEST_OK` or `WAITQ_SELFTEST_FAIL`.

Boot calls `cpu_simd_selftest()` after programming the PIT, and invokes Rust inflate and PNG self-tests at their initialization points in `c/kernel/init/kmain.c`.

In `HWHUD=1` builds, `c/kernel/audio/boot_test_tone.c` schedules a one-second 440 Hz tone after at least five seconds have passed since the first panel poll. Playback uses the kernel stream and mixer path. Its report includes hardware position before and after playback and stream underruns. A boot module named `NOSOUND` selects the skipped state.

The AMD boot framebuffer driver invokes the Polaris self-test after successful resource probing in `HWHUD=1` builds. `c/drivers/gpu/amd/polaris/selftest/selftest.c` checks device identity, opt-out state, arena availability, and firmware availability before acquiring an ownership lease. Its stages report mapping, runtime binding, firmware loading, SMU startup, and SDMA testing through `[amd-polaris-runtime]` lines and retained panel status.

The repository defines these checks:

| Target | Checks encoded in the sources |
|---|---|
| `make test-klog` | Formatting, levels, truncation, wraparound, pinned records, interleaving, rendering, and partial-line flushing |
| `make test-klog-control` | Expected failure with shared line assembly and interrupt guards removed |
| `make test-panic-log` | Guest log readback, counters, warnings, wraparound, and interrupt logging |
| `make test-panic` | Panic output, retained log, halt markers, and backtrace addresses checked against a linker map |
| `make test-prof-host` | Accumulation, spans, reports, commands, and concurrent histogram updates |
| `make test-prof-negctl` | Expected failure with non-atomic histogram updates |
| `make test-prof` | Distribution and overhead checks on one CPU, then sampling integrity on four CPUs |
| `make test-prof-control` | Expected rejection of a build whose report identifies profiling as disabled |
| `make test-wait-smp` | Wait self-test markers with one, two, and four CPUs |
| `make test-amd-bootfb-host` | Boot framebuffer and self-test adapter checks in plain and HWHUD builds, with prerequisite negative controls |

## Limitations

- Formatted output holds its message lock with interrupts masked during formatting and output. Large messages increase interrupt latency. Serial queue overflow and abandoned blocking drains lose bytes; the driver counts these losses.
- Retained records truncate after 112 text bytes. Diagnostic rendering uses bounded shared buffers, and concurrent readers can replace the cached rendering. The paged screen reads rolling records; pinned boot records are exposed through `/dev/kmsg`.
- Sampling uses maskable interrupts, so execution with interrupts disabled defers samples and biases attribution. Tables contain 8,192 kernel buckets and 1,024 user buckets with a probe budget of 16. User addresses carry no process identity. Reports list up to 48 kernel and 16 user addresses; the symbolizer derives function percentages from those listed rows.
- Span storage holds 64 names and retains 23 characters per name. Names sharing that prefix resolve to the same slot. Profile reads lack a transaction covering every counter. `KPROF_DISABLE` removes span macros and changes the report, while sampling control remains implemented.
- Fatal kernel faults reaching the generic dispatcher use the compact `panic_exception()` reporter in `c/kernel/cpu/irq/interrupts.c`. `panic_from_exception()` remains uncalled.
- `c/kernel/diag/panic.h` declares `ksym_lookup(uint64_t, uint64_t *)` for address resolution. `c/kernel/module/ksyms.c` defines `ksym_lookup(const char *)` for module imports. These incompatible signatures share a linker name, making panic symbol resolution unsafe.
- Panic peer stopping uses maskable IPIs and a bounded delay without acknowledgements. Report completion depends on usable stacks, memory, and output devices.
- Hardware-report persistence stops after a filesystem refusal. Interrupted pruning can leave more than 20 numbered reports. The boot-tone and Polaris diagnostic sources identify physical X79-VG4 verification as pending.
- Scheduler observation covers up to 32 CPUs and reports additional CPUs as unobserved. Its interrupt-masking timing covers instrumented lock paths; bare `cli` and `sti` sequences remain outside that coverage.
- `tests/boot/run-panic-log-test.sh` still expects a 512-record boot banner, while the current kernel prints 2,048. This stale assertion prevents treating that target as evidence of a passing current logging suite.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/diag/kprintf.h`, `c/kernel/diag/kprintf.c` | Formatter, message locking, buffer formatting, and alignment counters |
| `c/kernel/diag/klog.h`, `c/kernel/diag/klog.c` | Record layout, severity helpers, rolling and pinned storage, rendering, and panic takeover |
| `c/kernel/diag/kdiag.h`, `c/kernel/diag/kdiag.c` | Diagnostic files, commands, interrupt gates, and logging probes |
| `c/kernel/diag/bootmark.h`, `c/kernel/diag/bootmark.c` | Boot-stage squares and framebuffer console |
| `c/kernel/diag/hwreport.c`, `c/kernel/diag/hwreport_log.c` | Hardware inventory, milestones, transcript persistence, and retention |
| `c/kernel/diag/hwscreen.h`, `c/kernel/diag/hwscreen.c`, `c/kernel/diag/hwscreen_font.h` | Paged report, text wrapping, and compiled font |
| `c/kernel/diag/hwhud.c`, `c/kernel/diag/hwhud_panel.h` | Hardware panel collectors, placement, visibility, and interaction |
| `c/kernel/diag/kprof.h`, `c/kernel/diag/kprof.c` | Sampling, spans, reports, commands, and measurement self-tests |
| `include/abi/logit_abi.h`, `include/abi/logit_sched_observe.h` | Scheduler observation syscall and request formats |
| `c/kernel/sched/observe/observe_control.c`, `c/kernel/sched/observe/flight_recorder.c`, `c/kernel/sched/observe/observe_hud.c` | Observation lifecycle, event rings, counters, and panel summary |
| `c/kernel/diag/panic.h`, `c/kernel/diag/panic.c` | Assertions, warnings, state capture, unwinding, and panic reporting |
| `c/kernel/diag/wait_selftest.c`, `c/kernel/sched/sched.c` | Wait self-test and its launch, plus scheduler observation hooks |
| `c/kernel/init/kmain.c` | Diagnostic initialization, boot reports, and boot self-tests |
| `c/kernel/cpu/irq/interrupts.c`, `c/kernel/cpu/smp/smp.c` | Exception reporting, serial timer draining, and processor timer setup |
| `c/kernel/cpu/irq/idt.c`, `c/kernel/cpu/power/mce.c` | Machine-check entry and bank reporting |
| `c/kernel/core/ktime.c` | Raw and clamped monotonic clocks |
| `c/kernel/module/ksyms.c` | Module symbol lookup |
| `c/kernel/gui/wm.c` | Hardware-panel calls and pointer routing |
| `c/kernel/audio/boot_test_tone.h`, `c/kernel/audio/boot_test_tone.c` | Diagnostic tone generation, playback, and progress reporting |
| `c/drivers/gpu/amd/bootfb.c`, `c/drivers/gpu/amd/polaris/selftest/selftest.c` | GPU self-test integration, stages, and results |
| `c/drivers/core/io_lock.h`, `c/drivers/char/serial.c` | Device locks, serial queue, draining, and loss counters |
| `c/fs/vfs/vfs.c`, `c/net/core/net.c` | Diagnostic file dispatch and network diagnostic controls |
| `Makefile`, `tests/prof.mk`, `tests/smpstorm.mk` | Logging, panic, profiling, and wait test targets |
| `tests/unit/log_test.c`, `tests/unit/prof_test.c` | Host logging and profiling assertions |
| `tests/boot/run-panic-log-test.sh`, `tests/boot/run-panic-test.sh` | Guest logging and panic checks |
| `tests/boot/run-prof-test.sh`, `tests/boot/run-wait-smp.sh` | Guest profiling and wait checks |
| `tests/gpu/amd/bootfb.mk`, `tests/gpu/amd/bootfb_test.c` | GPU adapter checks with a stubbed self-test |
| `tools/ksymbolize.py` | Profile address resolution and function aggregation |

## See also

- [Kernel initialisation](init.md)
- [Scheduling](scheduling.md)
- [Synchronisation](synchronization.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Testing](../architecture/testing.md)
- [Writing tests](../contributing/writing-tests.md)