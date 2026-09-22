# Display, glass and filesystem boundary fixes — 2026-09-22

These changes were written by hand in the shared source tree. No formatter or
`.clang-format` was used. The preceding glass decomposition is described in
[the readability report](glass-readability-2026-09-22.md): bounds and allocation,
blur, geometry, shading, and rendering have separate responsibilities. This
pass repairs four reproduced failures without merging those responsibilities
back into one function.

## Behavior and regressions

| Area | Observed old behavior | Change and evidence |
| --- | --- | --- |
| Glass allocation failure | The frost fallback rounded the cropped rectangle, painting 227 pixels outside one offscreen lens. | Leave the target unchanged and retain `OL_LIMIT`. Twenty allocation-failure scenarios now pass; restoring the old fallback fails all twenty. The complete geometry suite passes 1067 assertions in ordinary, slow and sanitized builds. |
| Framebuffer binding | A fresh 5120×2880 surface rendered `ff8040` as `0000ff`; a prior small bind hid the problem. Rejected targets could still receive writes. | Packed display targets have their own 8192 dimension limit. The framebuffer wrapper publishes only validated targets and disables drawing after failure. Fifty-one ordinary and sanitized assertions pass; the original source fails five. Required controls fail seven and three respectively. |
| IPv6 parsing | Trailing single colons and `::` representing zero groups were accepted. | Parse groups, validate compression, then commit destination bytes. The original source fails sixteen assertions; the corrected parser passes 369 in ordinary, sanitized and actual guest runs. Three required mutations are observed failing. |
| RAMFS directory rename | Only the directory's own stored path changed; children remained under their old names. Moving into descendants and overlong descendant paths were also accepted. | Preflight every affected path, then rewrite the subtree. Original and control builds fail 22 of 156 host assertions; corrected ordinary and sanitized builds pass all 156. The guest comparison changes from 18 failures to all 52 checks passing. |

The generic `ol_display_bind` retains its previous context on rejection. Only
the framebuffer compatibility wrapper clears its target, because its drawing
API cannot return a bind failure. Both contracts have separate assertions.
`GFX_MAX_W` remains 4096: it bounds one vector coverage span. Enlarging packed
targets does not enlarge static raster arrays or claim support for arbitrarily
large source images. The 8192-square packed target's final pixel is tested.

The old libc differential called Darwin's library “glibc.” Its name is now
accurate, and 37 shared-behavior cases pass. Deliberate strict `inet_pton`
leading-zero rejection is tested separately from legacy `inet_aton` forms;
the parser was not changed to imitate a platform-specific permissive result.

## Actual guest evidence

All runs used separate copied disks. Each report records input hashes,
confirms that source artifacts remained unchanged, and confirms QEMU exited.

- [5K before screenshot](evidence/2026-09-22/build-quality-glass/wide-before/desktop.png):
  the dock and its colored tiles appear blue.
- [5K corrected screenshot](evidence/2026-09-22/build-quality-glass/wide-fixed/desktop.png):
  actual 5120×2880 scanout has the intended glass and icon colors.
- [Before guest results](evidence/2026-09-22/build-quality-glass/wide-before/result.json)
  and [corrected guest results](evidence/2026-09-22/build-quality-glass/wide-fixed/result.json):
  the same RAMFS commands, with mount identity and fixture bytes verified.
- [Libc guest output](evidence/2026-09-22/build-quality-glass/candidate-bounds/result.json):
  `inet contract: 369 checks, 0 failed` and `LIBC_OK 578/578`, both exit zero.
  [Packed artifact hashes](evidence/2026-09-22/build-quality-inet/guest-input-final/artifacts.json)
  verify the inet fixture, ordinary libc battery and browser before launch.

At 1280×800, all 27 captured scenes are pixel-identical outside the live clock
rectangle `[1232, 10, 1269, 23)`.
[Complete pixel comparison](evidence/2026-09-22/build-quality-glass/comparison-bounds-pixels.json).

Guest WM glass timing, with two warmup pairs excluded and ten measured frames
per theme, remains in the same range:

| Theme | Before median / p95 | After median / p95 |
| --- | --- | --- |
| Light | 11.481 / 14.135 ms | 11.757 / 12.002 ms |
| Dark | 11.415 / 12.021 ms | 11.330 / 11.910 ms |

Every measured frame requests 131764 glass rectangle pixels. These are guest
WM counters, not host elapsed time or actual covered-pixel counts. With ten
samples, nearest-rank p95 is the maximum. This batch establishes no stable
speedup. [Raw comparison](evidence/2026-09-22/build-quality-glass/comparison-bounds.json).

## Build and checks

`make BUILD=build-quality-glass -j6 all build-quality-glass/disk.img` completed
with an actual kernel link and app-disk repack.
[Build log](evidence/2026-09-22/build-quality-glass/build-final.log).

The following final command passed, including the required negative controls:

```sh
make BUILD=build-quality-glass -j4 \
    test-glass test-glass-material test-glass-geometry \
    test-glass-geometry-asan test-fb-clip test-fb-native-present-host \
    test-fb-display test-mk-wired
```

[Final graphics gates](evidence/2026-09-22/build-quality-glass/bounds-final-gates.log),
[RAMFS gates](evidence/2026-09-22/build-quality-ramfs/final-gates.log),
[IPv6 gates](evidence/2026-09-22/build-quality-inet/fixed-gate.log).
The wiring audit reports 383 fragments, 382 reachable and one declared wrapper.
An earlier concurrent audit saw the new framebuffer fragment before its include
was added; the final audit above reran after wiring and passed.

An independent allocator audit also covered twelve glass failure/growth/recovery
sequences: no duplicate releases, no remaining allocations after destruction,
and no sanitizer diagnostics.
[Boundary and ownership review](evidence/2026-09-22/build-quality-geometry/bounds-audit/DISPLAY-BOUNDARY.md).

## Remaining work

This is a completed batch of fixes, not a claim that the whole OS is bug-free.
VFS descendant permission/owner/link metadata migration remains a separate
existing rename problem; this patch changes RAMFS path reachability and content.
The full host libc aggregate proceeds past inet but still encounters an existing
Darwin libgen link failure involving `memrchr` and `stderr`. The complete guest
libc battery above passes. QEMU scanout evidence does not prove physical 5K
hardware, and this material is an independent implementation, not Apple's
proprietary shader.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
