# Provenance: tests/fixtures/subs

## Mixed classification — no class-A files in this directory

Nothing here is a verbatim capture of a live site (the shape every other
class-A directory in this audit has). The 48 `.vtt` files under `wpt/` are
class-B test DATA extracted from web-platform-tests under its own permissive
licence; everything else is class-C, project-authored or project-generated.
**This directory needed no removal/skip work** (item 2 of the task mandate)
because it contains no class-A file — recorded here as a finding, not
assumed.

### Class B — `wpt/*.vtt` (48 files, 37,607 bytes) — BSD-3-Clause, web-platform-tests contributors

Per the directory's own `tests/fixtures/subs/wpt/README.md` (already
present, written when this subset was extracted):

> "The 38 `*.vtt` files were extracted from
> `webvtt/parsing/file-parsing/support/*.test` in the upstream repo at the
> pin below" / "License: these files are data from web-platform-tests,
> itself BSD-3-Clause-ish (`LICENSE.md` upstream) -- the same license basis
> `third_party/wpt` already carries elsewhere in this repository."
> (`tests/fixtures/subs/wpt/README.md`, "Where this came from" and
> "License" sections)

Pinned upstream revision: `841f08ffcef5430c77cf4b4c6ad1d5f6625880da`
(`tests/fixtures/subs/wpt/README.md`, "Upstream revision" — the same pin
`tools/wpt_revision.txt:1` names for the rest of this tree's WPT usage).
`third_party/wpt` itself is **not vendored** in this tree any more (commit
`4179053ef`, "wpt: the corpus is fetched at a pinned revision, not vendored
-- 59,422 files leave the tree"; `tools/wpt_fetch.sh` fetches it into
`build/wpt`, gitignored, on demand) — which is exactly why this subset
exists as a small committed exception rather than as a `wpt_fetch.sh`
subset: nobody had wired a WebVTT consumer into that script (the README's
own words, quoted above), and a `<track>` parser test needs its corpus
present even on a machine that never runs `make test-wpt`.

**The licence text travels with it already, via an identical existing file
in this repository**: `third_party/html5lib-tests/LICENSE.wpt:1-3` carries
the verbatim 3-Clause BSD text for `web-platform-tests contributors` —

```
# The 3-Clause BSD License

Copyright © web-platform-tests contributors
```

(`third_party/html5lib-tests/LICENSE.wpt:1,3`) — the same copyright holder
and licence this subset's data comes from (both are WPT data, imported from
the same upstream project, just at different times and for different
purposes). This satisfies the "reproduce this notice" condition of clause 2
of that licence for a copy already in this repository; it was **not**
duplicated a second time under `tests/fixtures/subs/` since the identical
text at `third_party/html5lib-tests/LICENSE.wpt` already exists and covers
the same upstream copyright — see LICENSING policy note below.

**10 of the 48 files are raw upstream fixtures, copied byte for byte, not
`.test`-file extractions** — the README names them: `empty.vtt` and the nine
`signature-*.vtt` files (`signature-bom.vtt`, `signature-invalid.vtt`, etc.)
— "they carry no `.test` wrapper because they are the 'invalid signature,
must not parse' half of the corpus" (`wpt/README.md`). The other 38 are this
project's own extraction of the WebVTT body out of each `.test` file's
Python-`unicode_escape`-encoded fixture block — data, not upstream code, and
the extraction script is documented as "not committed" (a one-off `curl` run
against the pinned revision).

### Class C — everything else (5 files, 1,590 bytes; all project-authored or project-generated)

| file | bytes | classification | evidence |
|---|---|---|---|
| `sample.vtt` | 859 | hand-authored | its own first line: `WEBVTT - a hand-authored sample, not from web-platform-tests` (`tests/fixtures/subs/sample.vtt:1`) |
| `sample.ffmpeg.srt` | 393 | tool-generated from `sample.vtt` | `tests/unit/subs_srt_roundtrip.py:18-19`: "`sample.ffmpeg.srt` is COMMITTED (generated once by `ffmpeg -y -i sample.vtt -f srt sample.ffmpeg.srt`)" |
| `srt-clean.srt` | 100 | hand-authored | cue text is self-describing test prose ("Clean SRT, CRLF-free.", "Second cue.") in the same style as `sample.vtt`'s cues; no `.test`/`SOURCE`/README claims a third-party origin, and `subs_fuzz.c:206` lists it beside `sample.vtt` as one of the "real fixtures" corruption phase 1 mutates, with no distinct provenance note |
| `srt-clean-crlf.srt` | 108 | hand-authored | same file as `srt-clean.srt` with CRLF line endings (verified with `od -c`: every line ends `\r\n`) — a deliberate CRLF-handling test variant, not a capture |
| `srt-malformed.srt` | 130 | hand-authored | cue text is again self-describing ("this is not a timing line" / "Text that never gets a cue." / "The valid cue after the malformed block.") — built specifically to exercise `subs.c`'s skip-and-count behaviour on one bad cue block, per `subs_negctl.py`'s header describing exactly this shape of fixture |

None of the five carry any third-party content; all are inputs this
project's own test authors or `ffmpeg` wrote/derived for the subtitle
parser (`c/lib/media/subs.c`, untracked/in-progress at the time of this
audit — see "State of the consumer" below).

## Consuming gates — and a finding: the gate the README names does not exist yet

`tests/fixtures/subs/wpt/README.md`'s own text names `tests/subs.mk`'s
`test-subs-wpt-diff` target as the consumer. **`tests/subs.mk` does not
exist in this tree** (`ls tests/subs.mk` — no such file; `grep -rn subs
Makefile` finds no `include tests/subs.mk` line either). The actual
consumers present today are the host-level scripts and binaries themselves,
invoked directly (not yet wired into `make`):

| consumer | reads | wired into a `make` target? |
|---|---|---|
| `tests/unit/subs_test.c` | takes a file path as `argv[2]`, no directory scan | no (no `.mk` target builds/runs it yet) |
| `tests/unit/subs_diff.py` | `glob(fxdir/*.vtt)` over an argument directory (intended: `tests/fixtures/subs/wpt`) | no |
| `tests/unit/subs_oracle.py` | one file path, `argv[1]` | no |
| `tests/unit/subs_negctl.py` | `glob(fxdir/*.vtt)` + `glob(fxdir/*.srt)` over an argument directory | no |
| `tests/unit/subs_srt_roundtrip.py` | two explicit paths (`sample.vtt`, `sample.ffmpeg.srt`) passed as `argv[2]`/`argv[3]` | no |
| `tests/unit/subs_fuzz.c` | a hard-coded list of 19 relative paths under a `fxdir` argument that defaults to the literal string `"tests/fixtures/subs"` (`subs_fuzz.c:203-211`) | no |

**State of the consumer**: `c/lib/media/subs.c`/`subs.h` are themselves
untracked (`git status --porcelain -- c/lib/media/subs.c` → `??`), alongside
every other file in this table — this is an in-progress, not-yet-wired
feature, consistent with the CLAUDE.md system note about concurrent sessions
working in this tree. **No `test-*` gate currently exercises this directory
at all**, so there is nothing to add a skip-on-absence branch to (item 2's
mandate is to make an existing gate skip cleanly; there is no existing gate
here yet). Recorded as a finding for whoever wires `tests/subs.mk` in: at
that point, the same wildcard/skip pattern already applied to
`tests/cssweb.mk`'s `AUDIT_DIRS` and `tests/jsperf.mk`'s
`JSPERF_GUEST_FIXTURES` (see those `PROVENANCE.md` files) is the template —
though strictly for the class-B `wpt/` subdirectory, not because it is
class A (it is not), simply because any fixture-reading gate should degrade
gracefully rather than hard-fail, the same discipline `bench-css`/`bench-js`
were just given.

## What was NOT done, and why

No file was moved aside and no gate was modified in this directory: with no
class-A file and no wired gate to test, there is nothing the task's item 2
(class-A skip) or item 3 (prove the skip) applies to. `subs_diff.py`'s and
`subs_negctl.py`'s `glob()` calls already degrade silently on a missing
directory (an empty match list, not an exception) — a **soft** no-op rather
than a stated skip — but since no `.mk` target invokes them yet, fixing
that message would be dead code with nothing to prove it against
(no CI gate to run before/after). Left as-is and named here rather than
silently improved out of scope.

## $(DISK) dependency

None found. No `Makefile`/`tests/*.mk` reference to `tests/fixtures/subs`
packs anything from this directory onto the LogitFS disk image (there is no
such reference at all yet, per the table above).

## History

Not yet committed as of this audit (`git log --oneline -- tests/fixtures/subs`
returns nothing; the whole directory is untracked, `?? tests/fixtures/subs/`
in `git status`). No first-commit date exists yet, and therefore no
filter-repo history question applies — it has not entered history.
