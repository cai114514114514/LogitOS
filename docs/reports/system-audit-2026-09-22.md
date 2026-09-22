# System audit — 2026-09-22

Scope: the operating system, including boot, memory, scheduling, process
interfaces, storage, networking, drivers and the user runtime. This is a
coverage record for this pass, not a claim that the whole system is bug-free.
Changes were made in the original shared tree, without a formatter. Existing
unrelated changes and running virtual machines were preserved.

## Repairs

### VFS directory metadata

The old rename updated only the directory's own metadata record. A stored
0600 child consequently reverted to the default 0644 after its parent moved;
the public VFS/RAMFS regression actually read its six bytes as another uid.
Hard-link aliases also lost their canonical path, and a metadata-only long
descendant could be truncated after backend bytes had already moved.

`vmeta_rename_check` now checks the whole component-boundary subtree before
the backend callback. `vmeta_renamed` moves existing records in place after
success. This requires no spare record even at full capacity. Link identity
and literal symlink targets stay intact. Renaming an ancestor of an active
mount is refused because that mount has a separate operation owner and a
fixed mountpoint.

The frozen old implementation failed 30 of 98 assertions. The old-behavior
control fails 29 (the new nested-mount guard remains enabled in that control).
The repair passes 98/98 normally and with ASan/UBSan; existing VFS tests pass
156/156 in both modes. See [VFS evidence](evidence/2026-09-22/build-quality-vfs-meta/result.json).

The independent [fixed guest](evidence/2026-09-22/build-quality-vfs-meta/guest-fixed-sequential/result.json)
passes all 33 fixture and semantic checks through actual ring-3 commands.
The [frozen prior kernel](evidence/2026-09-22/build-quality-vfs-meta/guest-before/result.json),
using the same application artifacts and fixture, fails at the first metadata
check: `mode=644 uid=0 gid=0 nlink=1` instead of `600/1000/2000/3`. That run
stops at its first failure; later permission and long-path checks are not
claimed as executed on the old kernel. The fixed run checks owner/public/
outsider reads, both hard-link directions, canonical unlink, literal symlink
targets and refusal before a metadata-only path overflows.

The first guest fixture attempt itself was invalid: this shell treats `&&`
as background operators and does not support `;` as a statement separator.
The runner now checks every setup command independently and uses multiline
script input for a sequence that must retain one child process's credentials.
That failed apparatus run is preserved in `build-quality-vfs-meta/guest-fixed`.

This patch does not add direct renaming of metadata-only symlinks/hard-link
aliases or change the persistent metadata format. A separately observed
read-path permission-check ordering concern remains unverified; it is not
counted as a reproduced or fixed bug here.

### DNS record classes

The resolver requested Internet-class records but did not inspect the class
of answers or CNAMEs. A real query/poll fixture returned a CH-class address as
success. The parser now uses one `DNS_C_IN` constant for both query writing
and answer validation. It skips the entire foreign-class record and keeps
scanning, so an earlier foreign CNAME cannot change the lookup target or hide
a later legitimate Internet-class answer.

The frozen old parser fails 34 assertions. The matching negative control
also fails 34; normal and ASan/UBSan builds pass 111/111. The fixture covers A,
AAAA, mixed answers and CNAME chains. Existing DNS and IPv6 DNS tests pass
29/29 and 51/51 with their controls. The class fixture deliberately does not
define the separate, previously recorded CNAME minimum-TTL policy.

An isolated guest DNS responder was prepared, but its loopback UDP port 53
bind failed with `PermissionError: [Errno 13] Permission denied` before any VM
was launched. No service or host DNS setting was changed. This is explicitly
not guest-network acceptance; see [attempt log](evidence/2026-09-22/build-system-network/guest-dns.log).

The follow-up closes the separately recorded cache-lifetime defect as well.
The former five-second floor extended one-second and zero-TTL answers, CNAME
TTLs did not limit the terminal address, and a zero TTL doubled as the unset
sentinel when the other address family arrived. The resolver now keeps the
earliest absolute expiry from reception, including followed aliases. Waiting
for the second family does not restart that lifetime. The current query still
receives zero-TTL addresses; a subsequent query must go back to the transport.
An already expired refresh clears only the same-name cache entry.

This follows the maximum-lifetime and zero-TTL rules in
[RFC 1035 section 3.2.1](https://www.rfc-editor.org/rfc/rfc1035.html#section-3.2.1)
and [RFC 2181 section 8](https://www.rfc-editor.org/rfc/rfc2181.html#section-8).
The old floor rationale remains beside a correction in the source; its
existing test assertion is corrected rather than silently removed.

The [TTL evidence](evidence/2026-09-22/build-system-dns-ttl/results.json) records 36 failures
against the frozen old source, then 171/171 checks normally and with ASan/UBSan.
Four prerequisite controls independently restore alias omission, lifetime
extension, zero-as-unset merging and age resetting; they fail 14, 22, 6 and 6
specific assertions with status 1. Public async tests check new outgoing
queries at expiry as well as the returned address bytes. Existing class,
DNS and IPv6 DNS gates still pass 111, 29 and 51 checks respectively. This is
production parser/state-machine code over a modeled transport and clock;
controlled guest TTL behavior remains unverified.

### LogitFS failed rename

A valid 128-block image with one free block exposed another rename failure:
the operation returned `-1`, but a bystander directory entry disappeared from
the mounted view. The old compensating `dir_remove` also needed free blocks,
so it could fail while trying to undo the first change. `log_abort` discarded
staged writes without restoring the in-memory inodes and allocation bitmap.
The first observation recovered on remount; the expanded fixture corrects
that limited observation: a later ordinary commit can persist the broken
state before remount.

Rename now snapshots the source and two parent inodes and the full allocation
bitmap before starting the transaction. A precommit failure drops staged
writes and restores that state without allocating during rollback. The
snapshot costs 16 KiB of bitmap storage on the default 512 MiB image plus
three inode copies. This repair makes no performance improvement claim and
does not change failures after the commit record may already be durable.

The [expanded regression](evidence/2026-09-22/build-system-storage/rename-expanded-result.json)
passes 3,514 assertions with ASan/UBSan. Restoring the former rollback behavior
fails 300 assertions with exit status 1. Cases include v4 and v5 images,
same-parent and cross-parent moves, directories spanning two data blocks,
five allocation failures, journal exhaustion, later commits and remounts.
For v5, identities, revisions, timestamps and reference paths are checked.
The host model has one bitmap block; this is not multi-bitmap or postcommit
I/O-failure acceptance.

Existing cache, journal, crash, fsck and format checks pass 29, 48, 1,744,
167 and 28 assertions respectively. The crash sweep covers 76 write points
under three loss patterns. Its new prerequisite control changes deferred
frees back to immediate frees in a private source copy and observes 31
specific old-or-new-content oracle failures; arbitrary crashes and compiler
errors do not satisfy that control. See the [crash log](evidence/2026-09-22/build-system-storage/test-fs-crash-fixed.log)
and [control log](evidence/2026-09-22/build-system-storage/fs-crash-control/run.log).

Guest persistence is **not fully accepted**. Four actual boots of one private
disk completed byte verification; two also completed create/delete churn.
On the fifth boot, the small verifier wrote its success output but the shell
never returned to the next command. After that VM stopped, an independent
byte formula confirmed all three persisted files (100, 30,000 and 120,000
bytes) were intact. A same-length wrong-file control had earlier failed at
byte zero as expected. A separate copy of the failed disk then completed
12 verifier executions. This does not erase the intermittent exit/wait
failure or turn the five-boot run green. The original source disk and ISO
hashes stayed unchanged. Evidence: [five boots](evidence/2026-09-22/build-system-storage/durability-capture-ready/result.json),
[offline bytes](evidence/2026-09-22/build-system-storage/durability-capture-ready/offline-bytes.json),
[isolated replay](evidence/2026-09-22/build-system-storage/exit-replay/result.json).

A further 90 same-boot executions and a separate five-cold-boot/15-execution
run also passed. The cold-boot variable was tested explicitly because the
original stop occurred just after startup. These bounded follow-ups did not
reproduce the failure, so there is still no stopped-CPU diagnostic state and
no confirmed cause or repair. See [exit pressure](evidence/2026-09-22/build-system-storage/exit-stress/result.json)
and [cold boots](evidence/2026-09-22/build-system-storage/exit-cold-boots/result.json).

### Host libc test entrypoint

The libgen gate did not link on Darwin: the host exports `__stderrp` instead
of `stderr`, and does not provide `memrchr`. The new small runner links the
real project string implementation under its existing renamed symbols and
bridges only its host allocator imports. It supplies Darwin's stream symbol
without mixing FILE layouts. The fixture includes the header declaring BSD
`setprogname`.

After linking, ten rows differed because the project intentionally keeps a
double-slash root while Darwin collapses it. No production behavior was
changed to match that host choice. All 108 rows remain checked: the project
root policy is asserted explicitly and every other field is compared with
the reference. Removing root preservation causes ten failures; breaking
trailing-slash stripping causes 46. Both controls are prerequisites of the
ordinary gate. All eight err/warn cases also match stdout, stderr and status.

The later libc pass repairs the `search` and `uio` entrypoints as well. It
compares 61 common search assertions and 37 common vectored-I/O assertions
against actual Darwin libc, then separately checks 10 and 9 project-specific
contracts. Normal and ASan/UBSan runs agree. Four prerequisite controls catch
broken tree lookup, a missing destroy callback, lost positioned-I/O cursor
restoration and a wrong read-after-progress return value. See the
[portable fixture report](evidence/2026-09-22/build-libc-portable/report.md) and
[ordinary gate log](evidence/2026-09-22/build-system-libc/portable/common-gates.log).

The earlier statement below that the uio reference needs APIs absent from
Darwin was too broad: this host provides `preadv` and `pwritev`; the fixture's
feature macros hid their declarations. Common mode now exposes and calls
those real APIs. Linux-only setup constants are replaced by a small host
fixture helper using system headers, not by implementations of the APIs
under test. GNU search extensions and platform-specific uio cases remain
explicitly skipped on Darwin.

`strptime` and `getopt` also have reference-policy differences beyond their
old stream-symbol link errors. Their original GNU differential gates now
declare the glibc requirement. `pathx` and `iconv` additionally require the
Linux x86-64 ABI. Unsupported hosts print a named SKIP with the command to
run on that platform; supported hosts retain the original full gates. The
aggregate footer says only that available gates completed, not that skipped
coverage passed. No production libc behavior changed in these fixture repairs.

### Executable-loader test entrypoints

The ordinary host loader gate had two missing dependencies: its allocator
header and the SHA-512 implementation now used by the linked signature code.
Both are present now, with the host allocator isolated in the fixture. The
Darwin frame-move backend uses real fixed mappings and copies instead of the
Linux-only `mremap` call. Existing PIE fixture users still compile.

All nine ordinary, sanitizer, control and capability-probe binaries link.
Removing each restored dependency was observed to fail compilation/linking,
then restoring it returned those builds to green. Execution is a separate
result: this host has 16 KiB native pages and cannot run the compiled x86-64
fixture, so the runtime loader, fuzz and control gates explicitly SKIP. The
probe checks real mapping/protection capability before any runtime verdict;
missing probes and unexpected crashes remain failures. The independent base
address gate passes for 88 AEX artifacts. The fuzz fixture lacks a case marker,
so an expected fault is only an additional observation, never a passing
control verdict. See the [loader report](evidence/2026-09-22/build-system-exec/report.md).

## Coverage and evidence

| Area | This pass | Evidence and limit |
| --- | --- | --- |
| Boot ABI | 28 field layouts and BIOS/UEFI writers; three mutants rejected | [Boot contract](evidence/2026-09-22/build-system-core/boot-contract/results.json). Source-contract validation, not physical firmware execution. Mutants generated with counted Python replacements; no sed invocation. |
| Physical/virtual memory | PMM 15, page-table publication 168, concurrent MM 50 assertions; eight controls | [Memory results](evidence/2026-09-22/build-system-memory/results.json). 128 MiB simulated RAM and sparse virtual mappings; TLB calls are fixture counters. |
| Process/thread state | 88 existing synchronization/FD/exit assertions plus 55 thread-capacity and allocation-failure assertions | [Thread boundaries](evidence/2026-09-22/build-system-memory/thread-boundary/results.json). Real extracted production code, modeled scheduling and allocation seams. |
| Scheduler in QEMU | Independent 1-, 2-, 4-core boots all pass wait selftest | [Guest results](evidence/2026-09-22/build-system-memory/wait-guest/results.json). Actual CPU counts checked, 200 ms blocked dispatch count zero, 192 semaphore items, 1000/1000 mutex increments and deferred work executed. This does not establish general fairness or interrupt latency. |
| Process interfaces/time | poll 26, time 208, procfs 73; all controls rejected | [Core gates](evidence/2026-09-22/build-system-core/initial-gates.log). Host fixtures do not prove hardware clock accuracy. |
| Network | TCP 254, IPv4/UDP/ICMP/DNS protocol 55, new DNS class 111; existing DNS 29 and IPv6 DNS 51 | [Network regression](evidence/2026-09-22/build-system-network/dns-final.log). Host protocol gates use modeled Ethernet/clock leaves. |
| PCI | BAR-safety 5 assertions plus five failing controls | [PCI gate](evidence/2026-09-22/build-system-network/gate-3.log). Real probe/restore code, modeled PCI configuration access; not physical hardware compatibility. |
| VFS | New rename 98 and existing mount 156, normal and sanitizer; guest 33/33 | [Guest evidence](evidence/2026-09-22/build-quality-vfs-meta/guest-fixed-sequential/result.json). Host adds full-store and mount-boundary coverage. Frozen old kernel fails the actual mode/owner/link-count check. |
| libc | fnmatch 52, inet 37 plus strict contract 369, regex 34, libgen 108, err/warn 8, expansion 57 | [Full host log](evidence/2026-09-22/build-system-libc/host-gates-full.log). Aggregate remains red at the seven entries listed below. |

The complete current kernel ISO and ring-3 disk build succeeds with
`make BUILD=build-quality-glass -j6 all build-quality-glass/disk.img`.
[Build log](evidence/2026-09-22/build-system-core/system-build.log) and
[source/artifact hashes](evidence/2026-09-22/build-system-core/source-and-build-hashes.json)
bind the result to the actual input files. `test-mk-wired` passes with 385
fragments, 384 reachable and one explicitly declared wrapper.

After the LogitFS repair, that same full kernel-and-disk command was run again
and succeeded: [storage build log](evidence/2026-09-22/build-system-storage/system-build.log).
The resulting ISO and packed native verifier hashes are recorded in the
five-boot result above. The added rename fragment brings the current wiring
count to 386 fragments, 385 reachable and one declared wrapper; the wiring
check passes.

After the DNS lifetime repair, the full build succeeded again:
[DNS build log](evidence/2026-09-22/build-system-network/ttl-system-build.log).
The TTL gate brings the latest wiring count to 387 fragments, 386 reachable
and one declared wrapper; the [integrated run](evidence/2026-09-22/build-system-dns-ttl/integrated.log)
passes that check. Earlier artifact hashes above describe their own frozen
runs, not this later build.

## Outstanding checks

The broad libc host target is still red. `search` and `uio` reference builds
need APIs absent from Darwin; `strptime` and `getopt` have Darwin stream-symbol
link errors; `pathx` assumes glibc's errno accessor; `iconv` pulls x86 inline
assembly into the ARM host build. The pure-host `memstream_reference` process
also crashes. These are seven uncompleted test entrypoints, not seven proven
LogitOS runtime defects. Their outputs remain in the full host log; no gate
was deleted, silently skipped or reported green.

Correction after the follow-up above: `search` and `uio` now have passing
portable coverage, while the four GNU/Linux-dependent gates explicitly
report unsupported-host SKIP. `memstream_reference` remains an unresolved
host-reference crash. The full libc aggregate has not been rerun to a claimed
green result.

The concurrent VFS permission-ordering reproduction and subsequent host
reference-crash diagnosis were stopped by automatic safety review. They
remain unverified follow-up items; no production change is based on them.

The initial pass did not cover filesystem crash recovery; the later host
crash sweep above adds modeled recovery coverage, not physical power-loss
testing. Physical devices, every syscall/driver, general scheduler fairness,
and the complete browser/runtime surface remain outside completed coverage.
The prior graphics and ring-3 libc evidence is recorded separately
in [the earlier boundary report](quality-boundaries-2026-09-22.md); it was not
recounted as new coverage in this pass.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
