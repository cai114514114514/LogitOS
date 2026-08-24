# Provenance: tests/fixtures/video265

## Mixed classification

### Class C — project-generated (`sample.h265`, `sample.crc32`)

Per `tests/fixtures/video265/README`: encoded by this project with
`ffmpeg`/`libx265 4.1` at fixed, deterministic settings
(`pools=none:frame-threads=1:wpp=0:rc-lookahead=0`), from a synthetic
`testsrc2` lavfi source (same pattern as `tools/genvideo265.sh` — no input
file, no third-party content). `sample.crc32` is a checksum this project
computed and pinned against ffmpeg's own HEVC decoder output. No third-party
rights involved.

### Class D — cannot determine (`main10.h265`, `main10.txt`, `main10.crc32`)

This is the one genuinely uncertain item found in this entire audit, and it
is recorded as such rather than guessed either way.

Per the README:

> "main10.h265  The ITU/JCT-VC conformance bitstream WP_A_MAIN10_Toshiba_3,
> from the official draft_conformance set."

This is a bitstream from the JCT-VC (Joint Collaborative Team on Video
Coding, the ITU-T/ISO-IEC body that standardised HEVC) conformance test
suite, contributed by Toshiba during standardisation, and used industry-wide
(x265's own test suite, ffmpeg's fate-suite, and others draw from the same
`draft_conformance` set) as a reference decode target. `main10.txt` — the
only accompanying documentation shipped with the stream in this repository —
reads, in full:

```
conformance: HM-10.0
resolution: 416x240
purpose: weighted sample prediction for P slices with plural reference indices
MD5: SEI message in the bitstream
```

**No licence, copyright, or redistribution statement accompanies the file in
this repository.** I could not locate one: the file is not accompanied by a
README from the original ITU/JCT-VC distribution package (only the minimal
`main10.txt` above, which is the per-stream description file the conformance
package itself ships, not a licence). I did not have network access to the
authoritative distribution point
(`ftp3.itu.int/av-arch/jctvc-site/bitstream_exchange/draft_conformance/`) to
check for a package-level licence file, and did not want to state a
conclusion about redistribution terms I could not verify. **This is a
cannot-determine, not a "this is fine because it's an industry-standard test
vector" assumption** — conformance bitstreams contributed by a standards
participant (here, Toshiba) are widely redistributed in downstream test
suites, which is evidence of common practice, not evidence of an explicit
grant.

This file was **not** moved aside for the class-A-style skip proof (it is
not classified A), and `test-h265`'s handling of its absence was **not**
modified in this pass — the task's mandate was specifically class-A files;
this is class D. For the record, `test-h265` (`tests/h265.mk`) currently
reads `tests/fixtures/video265/main10.h265` unconditionally and would FAIL,
not skip, if it were removed (`crc=`... on a missing file, compared against
the pinned `main10.crc32`, prints `H265-FAIL main10 fixture crc  want ...`
and the recipe's `if [ "$crc" != "$want" ]` exits 1) — this is flagged here
as a finding for whoever resolves the class-D provenance question, not fixed,
since fixing the gate before the provenance question is settled would risk
hiding exactly the signal a licence review needs (a hard failure is a strong
prompt to look here; a silent skip is not).

## Consuming gates

| gate | file | reads |
|---|---|---|
| `test-h265` | `tests/h265.mk`, wired broadly | `tests/fixtures/video265/sample.h265` (class C, unconditional) **and** `tests/fixtures/video265/main10.h265` (class D, unconditional) |
| `$(DISK)` | `tests/h265.mk:146` | `tests/fixtures/video265/sample.h265` only (class C) — `main10.h265` does **not** ride the disk image |

## History

First added: commit `509fbcd4b` ("h265: the gates, the fixture, and the
decoder running on LogitOS"), 2026-08-08 (`sample.h265`); `main10.h265` added
one commit later the same day, `9a3b09c2a` ("h265: gate Main 10 on a real
conformance stream, not only on our own encoder").
