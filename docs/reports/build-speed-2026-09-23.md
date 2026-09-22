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

`gmake BUILD=build-goal-glass all build-goal-glass/disk.img test-fs-format
test-disk-sparse test-snapshot-sparse test-disk-profile test-mk-wired` passed.
`test-fs-format` reported 28 checks, and the preservation suite reported 15.
An independent QEMU boot used a temporary clone of the newly packed disk;
it reached `LOGIT_BOOT_OK` and the expected trust-store markers, while the
source disk's SHA-256 remained unchanged. The snapshot still validates and
replays via the existing fsck code, keeps the input read-only, creates a
private 0600 output, and removes it on failure. Final replacement still uses
the image-use guard and atomic rename.
