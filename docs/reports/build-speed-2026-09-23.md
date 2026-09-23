# Sparse disk packaging — 2026-09-23

The disk image is logically 512 MiB, but the build shown here used only 19,285
of 131,072 blocks. The packer previously wrote and synced all 512 MiB for the
final image; preservation also wrote a full 512 MiB private snapshot. Both now
leave zero blocks as sparse host-file holes. The packer writes its allocated
prefix and truncates to the required logical size. The snapshot scans the
validated, journal-recovered private mapping and writes nonzero block runs at
their original offsets. Reading either result yields the same raw bytes.

| Real 512 MiB image step | Dense, two runs | Sparse, two runs | Host allocation |
| --- | ---: | ---: | ---: |
| Checked snapshot | 0.123 / 0.130 s | 0.055 / 0.061 s | 512 MiB → 75.3 MiB |
| Final atomic write | 0.050 / 0.051 s | 0.009 / 0.014 s | 512 MiB → 75.3 MiB |

These are sequential `perf_counter` timings of the two write paths on the
2026-09-23 macOS host, using the same `build-goal-glass/disk.img` bytes and
distinct temporary outputs. The source and each output were SHA-256 compared;
all hashes matched. Timing excludes hashing. The final product's `stat` and
`du` reported 536,870,912 logical bytes and about 75 MiB allocated. These
component timings do **not** establish an end-to-end build speed ratio: make
parsing, compilation, filesystem assembly and validation also take time. A
full incremental build plus format and host gates completed in 5.992 s after
the change; no equivalent before timing was recorded.

The 32 MiB fixture tests both sparse paths against dense controls. The dense
mkfs control failed the allocation oracle while producing identical image
bytes. The dense snapshot control failed its allocation oracle while preserving
the same journal-replayed bytes. The positive snapshot occupied 114,688 bytes
for a 33,554,432-byte logical image; the positive mkfs output occupied 180,224
bytes. Smaller 8 and 16 MiB fixtures are not useful physical-allocation
oracles on this APFS host: truncation eagerly allocated their holes.

The first sparse change still built a full 512 MiB Python `bytearray` before
writing its prefix, and the preservation reader loaded a full 512 MiB snapshot
into `bytes`. The product path now allocates only through its last used block
and extends the file to the unchanged logical geometry. Only the validated
private snapshot uses a read-only mapping; mapping a live disk would make the
image-use guard correctly refuse a subsequent rebuild, so that reader remains
eager. In fresh Python processes on the same host, an empty builder's
serialization had peak RSS 558,546,944 bytes before and 23,134,208 bytes with
the compact path. Reading the same 512 MiB product image had peak RSS
558,415,872 bytes with the eager reader and 21,790,720 bytes with the private
mapping. The empty builder is a memory-allocation probe, not a full product
pack timing or peak-RSS claim for a populated image.

The 32 MiB gate compares the compact prefix with the full dense image and the
final full-size sparse file byte for byte. Its short-output control fails when
the writer omits the logical-size extension; its eager-read control fails the
reader's allocation bound. The existing dense-write control still fails only
the physical-allocation bound while retaining identical guest bytes.
`gmake BUILD=build-goal-diskmem test-disk-sparse test-disk-profile
test-snapshot-sparse test-fs-format test-mk-wired` passed after this change;
the preservation suite reported 15 checks and exercised the real compact
packer with indirect and double-indirect files.

The 2026-09-22 report correctly observed zero compiles and zero disk packs on
an unchanged real build. A later `make -q` check exposed a different defect:
the always-phony sysroot flag check, plus an ordinary dependency from copied
TCC sources to a newer snapshot stamp, made the query report "out of date"
and made `make -n` predict a rebuild. The flag now uses a content-tracked
configuration domain, and the copied-source edge is order-only; the TCC
objects still depend normally on the snapshot stamp. On the completed
`build-sysroot-idle` product, both GNU Make 4.4.1 and Apple Make 3.81 returned
0 for idle `-q` and 1 when `SYSROOT_MKFLAGS` changed. The latter was read-only.
Idle dry-runs printed only two up-to-date lines, with no compile or pack
commands. This fixes rebuild decisions and diagnostics; it does not establish
a measurable wall-clock improvement for an ordinary no-op invocation.

`gmake BUILD=build-goal-glass all build-goal-glass/disk.img test-fs-format
test-disk-sparse test-snapshot-sparse test-disk-profile test-mk-wired` passed.
`test-fs-format` reported 28 checks, and the preservation suite reported 15.
An independent QEMU boot used a temporary clone of the newly packed disk;
it reached `LOGIT_BOOT_OK` and the expected trust-store markers, while the
source disk's SHA-256 remained unchanged. The snapshot still validates and
replays via the existing fsck code, keeps the input read-only, creates a
private 0600 output, and removes it on failure. Final replacement still uses
the image-use guard and atomic rename.

The media and model product rules formerly listed every header in their
directory as a prerequisite of every object. Compiler `.d` files from the
isolated product build show that `mpeg4.h` reaches 3 of 28 video objects,
`opus.h` reaches 1 of 14 audio objects, and `kvcache.h` reaches 1 of 7 model
objects. `make -n -W` predicted all 28, 14, and 7 compiles under the old
rules; the narrowed rules predict 3, 1, and 1. This is a rebuild-count result,
not a measured end-to-end time ratio. `test-build-incremental` compiles 20 real
objects and their `.d` files, checks the exact consumer sets, and watches the
old broad dependencies fail its negative control. It also checks recovery
when product `.d` files disappear. Two video diagnostic objects outside that
recovery inventory keep their broad fallback; a separate check removes each
diagnostic `.d` and confirms a video header edit still schedules compilation.
