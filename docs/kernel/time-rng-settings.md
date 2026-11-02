# Time, randomness and settings

LogitOS implements kernel clocks, random generation and persistent settings in `c/kernel/core/`. Clocks provide deadlines and timestamps, the random generator supplies bytes from shared state, and the settings store combines system values with preferences for the active user. Applications reach these services through `c/kernel/exec/syscall.c`, using the ABI in `include/abi/logit_abi.h`.

## Clocks and time APIs

The kernel represents elapsed time as unsigned 64-bit nanoseconds. `time_mono_ns()` measures elapsed time from `time_init()`. `time_real_ns()` adds the wall clock offset that `time_wall_sync()` establishes from the CMOS RTC. Both clocks advance from the same source of elapsed time, keeping their rates aligned between offset updates. The RTC conversion treats the hardware clock as UTC. See `c/kernel/core/ktime.c` and `c/drivers/timer/rtc.c`.

Use monotonic time for deadlines and elapsed intervals, and realtime for timestamps associated with the Unix epoch.

| Kernel API | Meaning |
|---|---|
| `time_mono_ns()`, `time_mono_us()`, `time_mono_ms()` | Monotonic elapsed time in the named unit. |
| `time_mono_raw_ns()` | Reading of elapsed time before the shared monotonicity clamp, for diagnostics. |
| `time_real_ns()`, `time_real_unix()` | Unix time in nanoseconds or whole seconds. |
| `time_clock_gettime()` | Selected clock returned as seconds and nanoseconds through pointers. |
| `time_clock_res_ns()` | Resolution reported for a clock ID. |
| `time_cpu_ns()` | Stored user and system CPU-time counters for a process. |

The declarations and clock IDs are in `c/kernel/core/ktime.h`. `KCLOCK_BOOTTIME` selects the monotonic clock.

The legacy `timer_ticks()` interface counts delivered timer interrupts. `timer_ms()` converts that count using `TIMER_HZ`, defined as 100 in `c/drivers/timer/pit.h`. This preserves the published 10 ms step of `SYS_MONOTONIC_MS`. The conversion is implemented in `c/drivers/timer/pit.c`.

## Clock sources and startup

`pit_init()` programs PIT channel 0 and calls `time_init()`. Early calibration uses port I/O and CPU instructions, allowing clock initialization before the allocator for physical memory starts. Later, `kernel_main()` calls `time_platform_init()` after physical memory is initialized and the ACPI boot information has been handed off. This discovers and registers HPET. See `c/drivers/timer/pit.c` and `c/kernel/init/kmain.c`.

| Source | Selection |
|---|---|
| TSC | Requires a TSC, either invariant TSC support or a hypervisor indication from CPUID, and successful frequency calibration. |
| PIT | Supplies the initial fallback through the 100 Hz interrupt counter. |
| HPET | Becomes available after firmware discovery and supplies the fallback when TSC calibration remains unsuccessful. |

Initial TSC calibration takes three measurements against PIT channel 2 and chooses their median. Channel 2 provides a stopwatch while channel 0 keeps its configuration for the system tick. The median reduces sensitivity to interference affecting one sample. These mechanisms are in `c/kernel/core/ktime.c`.

When early calibration fails and HPET becomes available, `time_platform_init()` retries a usable TSC against HPET. `calibrate_tsc_hpet()` takes three samples of about 20 ms and chooses their median, accepting frequencies from 10 MHz to 100 GHz. HPET reads bracket each TSC endpoint; a bracket wider than approximately 10 microseconds causes a retry, with up to eight attempts per sample. This detects interference between the endpoint reads. Successful calibration selects TSC and retains HPET as an available source.

Clock reads combine a saved nanosecond accumulator with cycles elapsed since its last update. A sequence counter coordinates readers with accumulator and source updates. Conversion multiplies into an intermediate of 128 bits. Masked subtraction handles counter wrap, and signed deltas detect backward readings.

`time_mono_ns()` publishes a shared maximum through atomic operations. A reading behind that maximum receives the published value. Diagnostics count these corrections and record the largest one. `time_set_source()` banks elapsed time from the old source, then publishes the new source together with its matching counter baseline.

HPET supplies a continuously running counter through `c/drivers/timer/hpet.c`. Before enabling it, `hpet_init()` disables comparator interrupts and clears legacy replacement routing, returning IRQ0 and IRQ8 to the PIT and RTC. The driver documents firmware leaving this routing active and diverting PIT interrupts. The PIT continues to supply the periodic interrupt that services timers.

After `time_platform_init()`, a startup probe in `c/kernel/init/kmain.c` enables interrupts and counts PIT ticks over a 50 ms monotonic interval, printing an expectation of about five ticks. An iteration bound terminates the probe if time stops advancing.

The `make TIMENOCH2=1` option forces PIT channel 2 calibration to fail, allowing the HPET calibration path to be exercised. `time_report()` labels the calibration source as `pit-ch2`, `hpet` or `none`. See `Makefile` and `c/kernel/core/ktime.c`.

## Timers and application clocks

`struct ktimer`, declared in `c/kernel/core/ktime.h`, holds a deadline, optional period, callback, caller argument and heap position. `ktimer_add()` arms or moves a timer, `ktimer_cancel()` removes it, and `ktimer_next_ns()` reports the earliest queued deadline.

The implementation in `c/kernel/core/ktime.c` uses a binary minimum heap with `KTIMER_MAX` set to 128. Each timer records its position, giving cancellation direct access to its entry. The heap provides logarithmic insertion and removal while preserving nanosecond deadline ordering. The design comments connect this choice to a small timer population and frequent cancellation.

Periodic timers advance from their previous deadline to avoid accumulating service delay. When the next deadline is already due, the timer advances to one period after the current service time. `ktimer_run()` drains due timers and releases the timer lock before invoking each callback.

For interrupt vector 32 on the boot CPU, the handler calls `timer_tick()`. This increments the legacy tick counter and calls `time_tick()`, which updates the accumulator, samples CPU accounting and runs due timers. See `c/kernel/cpu/irq/interrupts.c` and `c/drivers/timer/pit.c`.

The application interfaces are dispatched in `c/kernel/exec/syscall.c` and declared in `include/abi/logit_abi.h`:

| Syscall | Behavior |
|---|---|
| `SYS_CLOCK_GETTIME` | Copies the selected clock into `struct logit_timespec`. |
| `SYS_CLOCK_INFO` | Reports source, frequency, resolution, clock readings, clamp diagnostics and timer counts. A nonnegative second argument selects an available source. |
| `SYS_NANOSLEEP` | Validates the duration fields and waits against a monotonic deadline. |
| `SYS_MONOTONIC_MS` | Returns the legacy millisecond count derived from timer ticks. |

For sleeps with more than two tick periods remaining, `SYS_NANOSLEEP` calls `sched_sleep_ms()`, aiming one tick before the deadline. Near the deadline, it calls `sched_poll_wait()` between clock checks.

## Random generation and entropy

`kernel_random_bytes()` supplies bytes from the shared generator in `c/kernel/core/rng.c`. Initialization occurs on first use, including a call to `rng_strong()`. A spinlock acquired with interrupts disabled protects seeding, generation and state updates.

The generator uses SHA-256, a state of 32 bytes and a block counter. Three tags distinguish reseeding, output generation and state evolution:

| Tag | Hash input and result |
|---|---|
| `0x00` | Previous state, tag and seed material produce the reseeded state. |
| `0x01` | State, tag and counter produce an output block. |
| `0x02` | State, tag and counter produce the next state. |

Each block supplies up to 32 output bytes and advances the state and counter. Distinct tags give output and subsequent state different hash inputs. Temporary seed material, hash contexts and output buffers are cleared through `crypto_wipe()`, defined in `c/crypto/crypto.h`.

Seed collection attempts to obtain four hardware words of 64 bits each. For each word, it prefers RDSEED and retries up to 16 times. If a sample remains unavailable, it tries RDRAND with the same retry bound. Collection also mixes RDTSC and `timer_ticks()` readings. Hardware instruction availability comes from the shared CPUID decoder in `c/kernel/cpu/cpufeat.c`.

`rng_strong()` reports whether a reseed has mixed at least two successfully returned hardware words. The instruction’s carry flag establishes sample success. Once established, this status persists through later reseeds that obtain fewer hardware words.

At the start of a generation request, the generator reseeds when the current epoch has reached 1,024 requests or 1 MiB of output. These counters track calls to `kernel_random_bytes()`, including individual chunks generated for a random syscall.

The TLS client checks `rng_strong()` before starting a handshake. The server checks it before generating certificate keys and before handshake setup. These gates are implemented in `c/net/tls/tls.c` and `c/net/tls/tls_server.c`.

## Random bytes for applications

`SYS_GETRANDOM` reaches `rng_syscall()` in `c/kernel/core/rng.c`. A null buffer queries `rng_strong()`. A buffer request validates the writable range, generates up to 4,096 bytes through a kernel staging buffer of 256 bytes, and returns the byte count. Each chunk is copied to userspace after generation. Staging keeps user addresses outside the locked section of the generator.

The libc `getrandom()` wrapper loops over the syscall’s request cap and returns the number of bytes obtained. The same file provides `getentropy()`, `arc4random_buf()`, `arc4random()` and `arc4random_uniform()`. `getentropy()` accepts requests of up to 256 bytes. The uniform helper uses rejection sampling to remove modulo bias. See `c/apps/libc/src/random.c`.

## Settings ownership and loading

The settings store centralizes preferences consumed by kernel services and applications. Its rationale in `c/kernel/core/settings.h` identifies a coordination problem: applications saving complete copies of a shared file could overwrite each other’s keys. A common table and serialized writer coordinate updates.

Startup loads settings after a successful mount of the root filesystem and before network and window manager initialization. Network configuration consumes them in `c/net/core/netcfg.c`; appearance initialization reads them in `c/kernel/gui/wm.c`. The startup order is in `c/kernel/init/kmain.c`.

The loader combines two files:

| Runtime location | Role |
|---|---|
| `/etc/settings.conf` | System values loaded before login. |
| Account home followed by `/.config/settings.conf` | User values loaded over the system layer. |

`settings_prepare_user()` resolves the home directory from `/etc/passwd` through the account parser in `c/apps/coreutils/accounts.h`. Preparation creates the configuration directory and settings file as needed, assigns account ownership, and requests modes `0700` and `0600`.

`SYS_SETSESSION` performs preparation before changing credentials, then calls `settings_adopt_user()` after a successful session change and preparation. A settings session lock spans preparation, credential publication and adoption or discard. This keeps concurrent session changes from exchanging their pending destinations. See `c/kernel/core/settings.c` and `c/kernel/exec/syscall.c`.

Entries in the main table record whether they came from the system layer. User commits select entries that override that layer. A snapshot of the main system table preserves previously readable defaults for later loads under user credentials.

## Schema and validation

`struct setting_def` describes a recognized setting through its key, label, type, group, bounds, default and enumeration choices. The current schema includes appearance, desktop and network preferences. The store also retains unknown keys. Keys beginning with `app.` use another bounded table, preserving capacity in the main table. See `c/kernel/core/settings.h` and `c/kernel/core/settings.c`.

| Bound | Value |
|---|---:|
| Key length, excluding the terminator | 47 bytes |
| Value length, excluding the terminator | 159 bytes |
| Entries in the main table | 96 |
| `app.` entries | 64 |
| Serialization buffer | 32,768 bytes |

These bounds come from `SET_KEYLEN`, `SET_VALLEN`, `SET_MAXKV`, `SET_APP_MAXKV` and `SET_BUFSZ`.

Numeric and address getters validate stored text when reading it and fall back to schema or caller defaults. String reads also apply the recognized setting’s validation. Validation on read covers values introduced through manual file edits. `settings_copy_str()` copies a value while holding the store lock, providing a snapshot for later use.

For `app.` entries, the type held in memory records whether the latest setter used a string or integer interface. Loading from disk assigns the string type because the file stores textual key/value pairs.

## Persistence and synchronization

The serializer emits `key = value` lines with terminating newlines, followed by a CRC32 comment. The parser records malformed entries and discards an unterminated final line. CRC mismatches set a diagnostic while parsed values remain available, supporting manual repair. `settings_diag()` exposes these findings.

Kernel setters take a commit argument: zero stages the value in the shared table, and a nonzero argument invokes persistence. `settings_commit()` serializes the active store and issues one write of the complete file. Serialization visits main-table entries that override system defaults, followed by `app.` entries.

On LogitFS, file replacement allocates fresh blocks while deferring release of the previous blocks. The filesystem orders data before the journal commit and installs the associated metadata through that journal. These mechanisms are implemented in `c/fs/logitfs/logitfs.c`.

A recursive mutex protects the tables, serialization buffer and destination across filesystem operations. Syscall strings are copied into kernel buffers before acquiring the mutex, and results are copied out after release. The mutex implementation is in `c/kernel/gui/gui_sync.h`.

## Settings interfaces and change tracking

Applications use these interfaces from `include/abi/logit_abi.h`:

| Interface | Purpose |
|---|---|
| `SYS_SETTING_GET` | Read an effective value with schema validation and defaults. |
| `SYS_SETTING_SET` | Stage a textual value; flag bit 0 requests a commit. |
| `SYS_SETTING_ENUM` | Enumerate schema metadata, or schema and `app.` entries matching a prefix. |
| `SYS_SETTING_CTL` | Commit, reset, reload, inspect diagnostics, iterate stored keys or query limits. |

For schema writes and resets, the syscall gate permits root or the owner of the active user store. A successful commit advances `settings_gen()`; the reset path also advances it. Applications read the counter through `SETCTL_GEN`. Convenience wrappers, including `setting_gen()` and `setting_commit()`, are in `c/apps/logit.h`.

The Settings application polls the generation counter and detects changes while edits are pending. The window manager uses it to refresh desktop preferences. See `c/apps/gui/settings/settings.c` and `c/kernel/gui/wm.c`.

## Limitations

- Timer callbacks are serviced by the 100 Hz interrupt, so clock resolution exceeds the precision of callback delivery. Callbacks run in interrupt context; the contract in `c/kernel/core/ktime.h` forbids blocking, heap allocation and taking the big kernel lock.
- Time derived from the PIT advances with delivered interrupts. Interrupt delays can lose elapsed periods from that count. `time_mono_raw_ns()` can return readings behind values previously observed by another caller.
- CPU accounting samples the boot CPU’s current process and keeps 64 records with eviction. Execution on other CPUs is omitted. Both CPU-time clock IDs return the current process’s record; `time_account_switch()` has no scheduler caller.
- The forwarding described in `c/kernel/core/ktime.h` differs from libc: `c/apps/libc/src/time.c` reads monotonic and CPU clocks through `SYS_MONOTONIC_MS`, realtime through RTC time in whole seconds, and implements sleep using the legacy millisecond clock. The kernel `SYS_NANOSLEEP` path caps its computed duration at one hour.
- Timing inputs provide the RNG’s weak fallback. The generator has no collection path from `c/drivers/virtio/virtio_rng.c`. The raw random syscall supplies bytes in weak mode and ignores its flags.
- `c/kernel/core/rng.h` says the syscall never returns a short count; `rng_syscall()` caps a request at 4,096 bytes. Direct callers must handle the returned count.
- The active settings store is global. Prefix enumeration provides filtering without application ownership checks, and writes outside the schema bypass the schema permission gate. Only a commit or a reset advances the generation counter, so polling cannot observe staged changes or reloads.
- Settings enumeration copies values into the 80-byte field in `struct logit_setting`, truncating values longer than 79 bytes. The store itself accepts 159 bytes.
- The combined settings tables can exceed the serialization buffer. The serializer omits entries beyond its remaining buffer budget.

## See also

- [Kernel initialisation](init.md)
- [Interrupts and time](../architecture/interrupts-and-time.md)
- [Timer hardware](../drivers/timers.md)
- [The C library](../userland/libc.md)
- [Cryptography overview](../crypto/overview.md)
- [LogitFS](../filesystems/logitfs.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/core/ktime.h` | Clock IDs, kernel time APIs and timer callback contract. |
| `c/kernel/core/ktime.c` | Clock sources, calibration, conversion, monotonicity clamp, timer heap and CPU accounting. |
| `c/drivers/timer/pit.h`, `c/drivers/timer/pit.c` | Tick frequency, legacy time interfaces and early clock startup. |
| `c/drivers/timer/rtc.h`, `c/drivers/timer/rtc.c` | CMOS clock access and conversion to Unix time. |
| `c/drivers/timer/hpet.h`, `c/drivers/timer/hpet.c` | HPET discovery, validation, mapping, interrupt disarming and counter access. |
| `Makefile` | Build option for exercising TSC calibration against HPET. |
| `c/kernel/cpu/irq/interrupts.c` | Timer dispatch on the boot CPU. |
| `c/kernel/core/rng.h`, `c/kernel/core/rng.c` | Generator, seed collection, status of hardware inputs and syscall backend. |
| `c/kernel/cpu/cpufeat.c` | Shared CPU feature detection. |
| `c/crypto/crypto.h` | Helper for wiping memory. |
| `c/drivers/virtio/virtio_rng.c` | Reads from the VirtIO entropy device and a boot diagnostic. |
| `c/net/tls/tls.c`, `c/net/tls/tls_server.c` | Randomness gates for handshakes and server key generation. |
| `c/kernel/core/settings.h`, `c/kernel/core/settings.c` | Schema, tables, parser, user overlay, synchronization and persistence. |
| `c/kernel/gui/gui_sync.h` | Recursive mutex used by the settings store. |
| `c/kernel/init/kmain.c` | Clock and settings startup order. |
| `c/kernel/exec/syscall.c`, `include/abi/logit_abi.h` | Syscall dispatch and application ABI. |
| `c/apps/coreutils/accounts.h` | Account parser used to resolve the settings home. |
| `c/apps/logit.h` | Application settings wrappers. |
| `c/apps/libc/src/time.c`, `c/apps/libc/src/random.c` | Libc clock, sleep and random interfaces. |
| `c/net/core/netcfg.c`, `c/kernel/gui/wm.c` | Network and desktop settings consumers. |
| `c/apps/gui/settings/settings.c` | Settings editor and generation polling. |
| `c/fs/logitfs/logitfs.c` | File replacement, deferred block release and journal commit. |
