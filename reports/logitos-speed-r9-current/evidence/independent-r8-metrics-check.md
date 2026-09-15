# LogitOS speed R8 independent metrics check

Audit date: 2026-09-13 (Asia/Shanghai)

## Evidence incident and scope

This check began against `build-browser-speed-final-r8/evidence/` and
`build-browser-speed-r6/net-ab/raw.json`.  I only issued read-only commands
(`find`, `cat`, `stat`, `sha256sum -c`, and Python JSON/statistics reads).  Between
two reads, the shared-workspace cleanup removed both original `build-*` trees.
The source manifest remained intact according to the separate recovery audit, and
a rebuild is taking place in `work-r8-recovery`; this audit did not read or modify
that recovery directory.

The results below distinguish values independently read and recomputed before the
cleanup from checks that must be rerun against the recovered evidence.  The
original deleted paths cannot now serve as durable citations, so recovered evidence
must be copied under `reports/logitos-speed-r8/evidence/` before final release.

## Checks completed before cleanup

The original R8 release manifest passed `sha256sum -c` for all three artifacts:

| artifact | bytes | SHA-256 |
|---|---:|---|
| `logit.iso` | 15,640,576 | `86113a8e8087c6076796957b88f5a83ef8da62a47b86c558e9523a757b660d2c` |
| `disk.img` | 536,870,912 | `1362bb1abc68bb13b466330cc5e10171e43c3a8cf34eae842cc5bd00dd5d9ef9` |
| `browser.aex` | 6,788,616 | `39b33c60bd38687a1f06510f04c1aed577de14c89a26b996b50865119787811d` |

All 18 entries in the original `KEY-SOURCES.sha256` passed against the then-current
source tree.  The separately preserved release audit also extracted
`/browser.aex` from `disk.img` and matched it byte-for-byte to the independent AEX;
that is supporting evidence, not a substitute for rechecking the recovered files.

All nine JSON files then present in the R8 evidence tree parsed successfully.  The
browser candidate contained three rounds for each of five cases.  The raw
`total_ms` samples and independently recomputed statistics were:

| case | baseline samples | R8 samples | medians | elapsed reduction | speedup |
|---|---|---|---:|---:|---:|
| lean | 1720, 1590, 1660 | 910, 600, 560 | 1660 -> 600 ms | 63.86% | 2.767x |
| style | 1690, 2040, 2380 | 810, 770, 740 | 2040 -> 770 ms | 62.25% | 2.649x |
| script | 1520, 2220, 2320 | 790, 730, 750 | 2220 -> 750 ms | 66.22% | 2.960x |
| deepseek fixture | 7190, 6350, 8340 | 890, 810, 880 | 7190 -> 880 ms | 87.76% | 8.170x |
| wikipedia fixture | 26390, 13430, 19520 | 8360, 5700, 5010 | 19520 -> 5700 ms | 70.80% | 3.425x |

Formulae were `(candidate_median / baseline_median - 1) * 100` for signed elapsed
change and `baseline_median / candidate_median` for speedup.  Every number matched
`browser-comparison-current.json`.  All 15 R8 rows recorded `script: true` and
`case_complete: true`.

The R8 web-acceleration result recorded first visit 3,100 ms and second visit
2,320 ms.  Against the independently read R6 baseline of 9,250 ms and 6,160 ms,
the reductions are:

- first: `(3100 / 9250 - 1) * 100 = -66.49%` (2.984x);
- second: `(2320 / 6160 - 1) * 100 = -62.34%` (2.655x).

The second visit arithmetic also matched its guest timestamps
`29650 - 27330 = 2320 ms`, and its `loadend_counts` were
`[14, 0, 0, 22, 797, 14, 0]`: 14 requests, zero dials and 14 cache hits.

The R8 interaction JSON parsed with `passed: true`, unchanged before/after artifact
records, exactly five uniquely named checks, and the expected nine-request sequence.
Its recorded results were:

| interaction | result |
|---|---:|
| close during held stylesheet | 377.8 ms host observation bound |
| close during infinite JavaScript | 487.6 ms host observation bound |
| scroll while stylesheet response held | 120 px in 171.8 ms |
| scroll while initial-script response held | 120 px in 170.6 ms |
| scroll while image response held | 160 px in 165.6 ms |

For every scroll case, `before_y - during_y` exactly equalled
`moved_during_load_px`, and `response_released_after_visual_proof` was true.  For
the image case, the after-load box equalled the during-load box, so the 160 px
movement survived load completion.  The JSON artifact hashes matched the R8 release
manifest and its before/after records were equal.

The previously read seven-pair network A/B raw values were:

- control `page_net_ms`: 580, 603, 625, 590, 735, 578, 577; median 590 ms;
- candidate `page_net_ms`: 563, 511, 519, 521, 329, 362, 645; median 519 ms;
- signed median change: `(519 / 590 - 1) * 100 = -12.03%`;
- paired candidate-minus-control deltas: -17, -92, -106, -69, -406, -216,
  +68 ms; paired median -92 ms and 6/7 pairs faster;
- guest `g_page_ms` medians: 600 -> 530 ms, or -11.67%.

This network evidence is a fixed local DNS/TCP/HTTP page with cached DNS and warm
ARP.  It does not measure public DNS, Internet RTT or TLS.

## Required hard gates for `final-verification.json`

`final-verification.json` did not exist before the shared cleanup.  A recovered
version should be generated from the recovered raw files and must make the following conditions machine-checkable.
No top-level `passed: true` should be emitted if any required input is absent.

1. **Artifact identity**
   - The ISO, disk and AEX exist, their actual SHA-256 values equal the embedded
     manifest, and every key-source hash matches.
   - Read-only extraction of `/browser.aex` from the disk is byte-identical to the
     preserved `browser.aex`.
   - Every guest run records the artifact hashes it actually consumed, and those
     hashes equal the final release artifacts.  A timestamp or path alone is not
     sufficient.

2. **Browser page-load gate**
   - Exact case set: `lean`, `style`, `script`, `deepseek`, `wikipedia`.
   - Exactly three distinct rounds per case; every row has `case_complete: true`
     and `script: true`.
   - Stored medians, changes and ratios recompute exactly from raw rows.
   - Every candidate median is below its baseline median.  For a meaningful speed
     acceptance gate, require at least 20% elapsed reduction for every case; R8's observed
     minimum before cleanup was 62.25%.

3. **Animation gate**
   - Raw baseline and candidate JSON are both present and hash the same fixture.
   - Both record 48 frames, `inputs >= 1`, `geometry_same: true`, and the expected
     layout-build count.
   - Candidate elapsed time and max rAF gap are both below baseline; require at
     least 20% reduction and a candidate max gap no greater than 100 ms.
   - This gate remains **unverified in this independent audit** because the source
     tree was removed before the animation JSON could be read and recomputed.

4. **Web acceleration/cache gate**
   - `first_ms == visit[0].t_loadend - visit[0].t_nav` and similarly for visit 2.
   - Both visits are at least 20% faster than their frozen baselines.
   - Second visit has exactly 14 requests, zero dials and 14 hits; no failed load
     marker is present.
   - The run embeds the consumed ISO/disk hashes.  The deleted R8 web JSON did not
     carry an artifact hash manifest, so this must be added by the recovered
     verification wrapper.

5. **Close and live-scroll gate**
   - `artifacts.before == artifacts.after`, and they equal the release artifact
     hashes.
   - Exact five-case set: two close cases and held stylesheet/script/image scroll.
   - Both close cases observe the exact WM-gone marker within 1,000 ms host bound.
   - Each scroll records `response_released_after_visual_proof: true`, at least
     80 px movement while the response remains held, and a visual proof within the
     harness's 5,000 ms maximum.
   - The image movement survives load completion; raw PPM-derived boxes match the
     JSON.  The old busy-JavaScript artifact must reproduce the expected 8-second
     negative before the current fix is accepted.

6. **Network gate**
   - Exactly seven complete pairs with one control and one candidate per pair.
   - Candidate `page_net_ms` median is at least 5% below control, paired median is
     negative, and at least 5/7 pairs are faster.  The observed result was 12.03%,
     -92 ms and 6/7.
   - Guest `g_page_ms` candidate median is below control.

7. **Host regression/sanitizer gate**
   - Current close/load host cases pass for stage, script, resource, wheel,
     wheel-cancel, wheel-script and module.
   - Named old negative controls fail for the expected assertion, while the
     pre-existing resource-close control remains green.
   - The same seven current cases pass ASan/UBSan with no diagnostic.  Record
     `detect_leaks=0` explicitly and do not translate this into a leak-free claim.
   - `browser-loading`, loader and progress-paint final markers pass; expected
     negative-control `FAIL` lines are classified and cannot satisfy a positive
     marker.

8. **Evidence boundary**
   - Encode that the page, animation, network and interaction results use QEMU/TCG
     and fixed local fixtures.
   - Encode sample counts and whether timings are guest clocks or host observation
     bounds.
   - Explicitly exclude physical X79/E5, i7-14700KF, GTX 1050, native GPU
     acceleration and public-Internet performance from the acceptance claim.

## Still required after recovery

- Recompute animation values and percentages from the recovered raw baseline and
  candidate JSON.
- Recheck all recovered release and run-input hashes rather than carrying forward
  the deleted tree's hashes by assertion.
- Preserve the recovered raw browser, web, animation, interaction and network
  evidence under `reports/logitos-speed-r8/evidence/` so another auditor can repeat
  every calculation after `build-*` cleanup.
