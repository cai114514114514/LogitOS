# BKL removal and performance verification

SPDX-License-Identifier: MIT

## Scope

The ordinary kernel no longer contains `g_bkl`, its owner, the syscall exemption
list, or scheduler lock handoff/reacquisition. Synchronization follows object
ownership. Kernel timer preemption remains disabled inside an active kernel
operation; explicit waits can sleep and migrate. Ordinary top-level ring-3
syscalls allow hardware interrupts. Nested IRQs do not schedule or run softirqs,
and signal return and the final entry epilogue retain IRQ masking.

Browser and third-party development in the shared checkout belongs to other
work. This change does not require rebuilding their applications. The acceptance
disk reuses existing application artifacts and explicitly rebuilds its own BKL,
signal and heap-stress fixtures.

## Ownership replacing the entry lock

| Area | Ownership and lifetime |
|---|---|
| Scheduler | Short irqsave publication locks; prepared threads become runnable only after initialization; pending wakeups survive the prepare-to-block window. Entry depth follows the resumed CPU. |
| Address spaces | Recursive task-owned guards hashed by CR3; ordered pairs for clone; reclaim uses nonblocking acquisition. Live roots and remote copies revalidate lifetime. |
| Shared kernel mappings | A separate owner protects the canonical kernel root, publication into existing address spaces, and root creation/retirement. Partial page-table allocation failure still publishes valid shared roots; range mapping performs one shootdown. |
| Physical memory | COW and usercopy pin pages; reclaim freezes aliases and acknowledges remote invalidation before reusing frames. Page-cache lookup returns a held reference and per-call miss accounting. |
| TLB invalidation | One serialized request/ACK transaction. Spin waiters service requests with IRQs off. A missing or excessive ACK fails closed and retains page ownership. |
| Process and files | Descriptor acquisition holds a reference. Offset changes, close/dup/fork publication, pipes, eventfd and timerfd have local ownership. Exit detaches the thread before publishing a reapable process. |
| VFS and storage | Short registry locks plus per-mount/per-medium operation owners. Unmount and shutdown drain admitted operations before releasing resources. |
| Devices | Driver-local queue/register gates and lifecycle owners. IRQ handlers hold live references; teardown drains callbacks before freeing state. PCI port transactions and IOAPIC route updates are indivisible. |
| Network services | Packet-path irqsave ownership; separate socket/HTTP/TLS/DNS operation owners. IRQ-capable route reporting copies values into its own stack, preserving interrupted syscall scratch. |
| GUI and shared services | WM operation owner, separate input/event queues and borrowed-object references; clipboard/settings snapshots; IME persistence runs through workqueue and VFS ownership. |
| Console and modules | Complete console messages have local ownership. Module/device initialization prepares privately and publishes under a short lock; callbacks run outside registry locks. |

The shared kernel mapping lock may be acquired while holding an AS guard. Root
publication does not acquire a blocking AS guard in the reverse direction.
Reclaim never waits for an AS already being operated on by another task.

## Performance changes

- Kernel heap magazines and their accounting are separated by CPU and cache line.
  Ticket-lock release uses a release store instead of another locked RMW.
- Page-cache hit/miss attribution is local to the lookup. Argument copy-in uses
  page-sized chunks rather than repeatedly copying one byte.
- Syscall histograms have 256 slots per CPU, including newer ABI numbers.
  IRQ-masked recording selects the actual exit CPU after possible migration.
  Reports read a nondestructive snapshot; sorting cannot erase another CPU's
  counters. The report frame was compiled at 4,264 bytes against a 16 KiB stack.
- Tasklet processing has a finite per-action budget as well as the outer softirq
  budget. Shared kernel range mapping batches its TLB confirmation.
- Whole-syscall IRQ masking was removed after a real heap workload exposed
  approximately 117 ms masked intervals and roughly 20% lost BSP ticks. The
  interruptible-call gate checks clock progress and stable thread, CR3 and TLS
  while all four CPUs remain in ring 0.

The heap-stress workload and strict `TN < 1.6*T1` requirement are unchanged.
Its former RTC-second measurement could report 5/8 seconds and reject a measured
5351/8396 ms run that meets that requirement. It now uses the existing monotonic
clock in milliseconds and retains the two-second minimum baseline.

## Reproduce

From the repository root, with an independent build directory:

```sh
make BUILD=/tmp/logitos-bkl-acceptance test-bkl-all
```

For a historical comparison, provide the actual pre-change kernel artifacts
(`kernel.elf`, `logit.iso`, `esp.img`) and the saved source archive:

```sh
make BUILD=/tmp/logitos-bkl-acceptance \
  BKL_BASELINE_BUILD=/tmp/logitos-bkl-20260910/before \
  BKL_BASELINE_TAR=/tmp/logitos-bkl-20260910/before/source.tgz \
  test-bkl-all
```

`WIDE_BASE_BUILD` selects the existing application build, defaulting to `build`.
The baseline used during this task has kernel SHA-256
`ce550a5577388f72e646fc62debb1c3f4fcab92c1fa9d621359c441e17f4e2bb`.
Without baseline artifacts the runner explicitly reports performance comparison
as unmeasured; it never manufactures an old kernel from current source.

The runner executes host controls, ordinary/test/control image builds, a real
serialized-entry negative control, a masked-IRQ negative control, and then the
BIOS/UEFI × 512 MiB/2 GiB/8 GiB matrix. It also runs wide memory/PIE/DMA/IME,
wait queues at 1/2/4/8 CPUs, signal/thread tests, 120 fork/exec iterations at
1 and 4 CPUs, and heap stress. Historical measurements alternate the same
program and disk between old and new kernels. Logs, source/image hashes and
machine-readable results are written below the selected build directory.

The structural source/ELF check complements the runtime four-CPU rendezvous;
absence of a lock symbol alone is not treated as concurrency proof. Real-source
host controls cover missing AS/kernel-map ownership, omitted clone coverage,
partial-root publication, TLB ACK mistakes, lifetime/publication races,
per-CPU accounting, device-register transactions and lost console messages.

Test process ownership also has controls. USB and serial harnesses terminate and
reap only their own child processes; cancellable input scheduling avoids waiting
for a 420-second producer after a guest has already exited. They do not stop
another task's QEMU. Benchmark results record other emulator CPU load.

## Verification boundary

The task host is macOS arm64, Mac16,5, 16 logical CPUs, 128 GiB RAM, running QEMU
11.0.0 with x86 MTTCG. These are emulator and real-source host results, not
physical-hardware measurements. RTL8169 still has no local QEMU device model.
The allocator source comparison isolates allocator behavior with the same
ticket-lock implementation on both sides; it is not an OS throughput result.

The compositor still serializes a complete frame. The low kernel heap and DMA
limitations documented by the earlier memory/DMA rounds remain. This change
does not add preemptive kernel scheduling, IOMMU/NUMA, shared-library loading or
a high-address kernel heap.

An additional core-dump probe produced a dump with matching fault registers,
but its external-reader checks could not run because this host lacks GDB.
That probe is not counted as a passed end-to-end core-reader test.

## Frozen-source acceptance: passed

`/tmp/logitos-bkl-20260910/release/bkl-acceptance.json` reports
`passed: true`, `source_unchanged: true`, 581 hashed source/harness inputs and
23 successful stages, including their negative controls. The run took 928 s.
The ordinary kernel SHA-256 is
`39d73e7e56ff8a7b06174acb77509d9487ab88430de11d052d3ad7a77ab1c0cd`.

| Measurement/check | Result |
|---|---|
| Same four-process memory/file workload, before | 990, 970, 1024 ms; median **990 ms** |
| Same program, ordinary new kernel | 336, 334, 333 ms; median **334 ms**, **2.96x** throughput for this workload |
| Heap-stress guest, one/four workers | 5731 / 7047 ms, four distinct CPUs, zero corruption; unchanged strict ratio passed |
| User threads | 620 / 960 ms for one/four threads; TLS values 4096–4099, exact mutex count 24000; 2000 create/join cycles returned slots from 1 to 1 |
| IRQ-masked negative control | Clock advanced only 0–1 ticks during a 200 ms kernel interval; named interruptibility assertion failed |
| Interruptible four-CPU kernel intervals | 20 ticks per 200 ms; thread, CR3, TLS and entry depth remained stable |
| BIOS/UEFI × 512 MiB/2 GiB/8 GiB | Both BKL/IRQ and wide-memory/static-PIE matrices passed |
| DMA/device, shutdown, wait, signal, fork | Full prior DMA acceptance, both shutdown paths, 1/2/4/8 CPU waiting, 52 signal checks and 120 fork/exec cycles at 1/4 CPUs passed |

The same benchmark program SHA-256 on both kernels is
`bdd5b3d16facebfb3c97a16334b5a4660110d19b8d8909c3cd0644aea3acce86`.
Runs alternated before/after, after/before, before/after. No other QEMU process
was observed at any benchmark boundary; host load averages were approximately
6.0–6.4. The 2.96x figure uses guest monotonic timing. Host command/serial
timings are retained in JSON for diagnosis and are not the OS speedup claim.

The separate native allocator source probe had median 128.907 / 25.844 ms over
three samples (4.99x). It used eight host threads, the actual old/new allocators
and identical current ticket locks. This remains an allocator-only result.

Evidence directories below the release build include `logs/`,
`parallel-results/`, `serialized-results/`, `irq-masked-results/`,
`regression/dma-acceptance.json`, `benchmark-before-*`, `benchmark-after-*` and
`heap-comparison/`. The full source fingerprint and each boot image fingerprint
are in the acceptance JSON.

The inherited IME harness used one CPU in that matrix. Its default has now been
changed to four CPUs (`IME_TEST_SMP=1` remains an explicit diagnostic override).
A separate ordinary-kernel BIOS/UEFI 8 GiB run **passed all six updated harness
cases**, without replacing or relabeling the earlier single-CPU evidence.
Each saved ASCII, Chinese-candidate and 66-byte sentence file matched exactly;
all six boot logs reported `4/4 CPUs online`. Results are under
`ordinary-ime-smp4/`. Reproduce this supplemental check with:

```sh
python3 tests/boot/run-dma-ime.py \
  --build /tmp/logitos-bkl-20260910/release/ordinary \
  --disk /tmp/logitos-bkl-20260910/release/parallel/bkl-disk.img \
  --out /tmp/logitos-bkl-ordinary-ime-smp4
```

`/tmp/logitos-bkl-20260910/release/delivery.json` binds the unchanged acceptance
report to the supplemental four-CPU evidence, its updated harness hash and the
ordinary kernel hash. Final verification found all 581 recorded inputs and 15
boot artifacts unchanged and no task-owned QEMU remaining. The default
`test-bkl-all` entry now inherits the four-CPU IME harness for future runs.
