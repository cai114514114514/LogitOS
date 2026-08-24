# Provenance: tests/fixtures/cssperf

## Classification: A — verbatim third-party work, all rights reserved

Per `tests/fixtures/cssperf/README`:

> "PROVENANCE (captured 2026-08-08, unmodified) ... These are third-party
> pages kept verbatim as test input only; deepseek.com's content is (c)
> DeepSeek; the Wikipedia article text is CC BY-SA 4.0 and its MediaWiki
> stylesheets are GPL-2.0-or-later."

| file | bytes | source | publisher / licence |
|---|---|---|---|
| `deepseek.html` | — | `https://www.deepseek.com/` | (c) Hangzhou DeepSeek AI, all rights reserved |
| `ds-1a1e18ce2de5eec1.css` | — | `.../_next/static/css/<hash>.css` (deepseek.com) | (c) Hangzhou DeepSeek AI |
| `ds-551fa65412a4f225.css` | — | same | (c) Hangzhou DeepSeek AI |
| `ds-cd0188618266f1d3.css` | — | same | (c) Hangzhou DeepSeek AI |
| `wikipedia.html` | — | `https://en.wikipedia.org/wiki/Operating_system` | article text **CC BY-SA 4.0**, Wikimedia Foundation |
| `wp-1.css` | — | Wikipedia's `load.php?...&only=styles` bundle | **GPL-2.0-or-later** (MediaWiki), per the README's own claim |
| `wp-2.css` | — | Wikipedia's `load.php?modules=site.styles` bundle | **GPL-2.0-or-later** (MediaWiki), per the README's own claim |

Total: 1,064,169 bytes (~1.0 MiB), all 7 files, per `du -sb`.

Note: the two Wikipedia stylesheets are the one place in this whole audit
where the README itself already names a copyleft licence (GPL-2.0-or-later)
rather than "all rights reserved" — worth flagging distinctly from the other
class-A directories, though the removal/skip treatment below applies to the
whole directory uniformly since all seven files are still third-party and
none are this project's own work.

**Follow-up fixed in this pass**: `LICENSES/` (this audit's ownership) held
`GPL-3.0-or-later.txt` (this project's own outer licence) but no
`GPL-2.0-or-later.txt` — the specific licence these two stylesheets are
under and the one `THIRD_PARTY.md:338` separately names for TinyCC's
`libtcc1.c` runtime exception. The two GPL versions are not
interchangeable text, so pointing at the wrong one would misstate the
grant. Fetched `https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt`
(HTTP 200, 338 lines, standard FSF text) and added as
`LICENSES/GPL-2.0-or-later.txt`.

## Consuming gates

| gate | file | reads | CI? |
|---|---|---|---|
| `bench-css` | root `Makefile:3252-3259` (embedded directly — **not** a separate `.mk` fragment, and out of this task's edit authority; see below) | `BENCH_PAGE ?= tests/fixtures/cssperf/deepseek.html`, `BENCH_CSS ?= $(wildcard tests/fixtures/cssperf/ds-*.css)` | no (bench, not `test-*`) |
| `bench-repaint` | root `Makefile` (`bench-repaint: $(ISO) $(DISK)`) → `tests/qmp/qmp_css_repaint.py` | `FIX = tests/fixtures/cssperf`, all three `ds-*.css` sheets, `deepseek.html` (its `<style>` block is hand-copied into the harness's own synthetic page, not fetched) | no |

Both are benches, not `test-*` gates, so neither is part of `make ci`'s
pass/fail surface — but both would still have crashed on a missing corpus
before this change (`css_bench.c`'s `slurp()` called `exit(2)` with `"css_bench:
cannot open %s"`; `qmp_css_repaint.py`'s `os.listdir(FIX)` would raise
`FileNotFoundError` if the whole directory were gone).

## What was changed

Root `Makefile`'s `bench-css`/`bench-repaint` recipes themselves could not be
touched (this task's instructions permit editing only the `RELEASE_NOTICES`
variable and the `$(DISK)` rule's `/licenses` lines in the root Makefile), so
the fix went into the two programs those recipes call instead:

1. **`tests/unit/css_bench.c`**: before calling `slurp()` on the primary
   page argument, `main()` now `stat()`s it; if absent, prints a named skip
   (naming the re-capture `curl` command from the README) and returns 0
   instead of `slurp()`'s old `exit(2)`.
2. **`tests/qmp/qmp_css_repaint.py`**: before `os.listdir(FIX)`, checks
   `os.path.isdir(FIX)` and the presence of `deepseek.html`; if either is
   missing, prints the skip message and `sys.exit(0)` before booting QEMU at
   all (this also saves the multi-minute boot on a machine with no corpus).

## Proof (run 2026-08-21, host-only; `bench-css` only — `bench-repaint` needs
`$(ISO)`/`$(DISK)`, see the note under "not run" below)

Control (fixture present): `make bench-css` printed a 14-row per-phase table
— `cascade 1.055 ms 36.4% 3492 styled`, `LOAD TOTAL 2.898` — nonzero.

`tests/fixtures/cssperf` moved aside
(`mv tests/fixtures/cssperf tests/fixtures/cssperf.MOVED_ASIDE`):
```
$ make bench-css
css_bench: tests/fixtures/cssperf/deepseek.html not present -- nothing to measure.
css_bench: this is not a failure. Re-capture with (see tests/fixtures/cssperf/README):
  curl -A 'Mozilla/5.0' https://www.deepseek.com/ -o tests/fixtures/cssperf/deepseek.html
$ echo $?
0
```
SKIPPED, not FAILED. Directory moved back; `make bench-css` re-run: full
14-row table reappeared, `LOAD TOTAL 2.898` again (deterministic on this
build) — nonzero, matching the pre-removal run.

**Not run**: `bench-repaint`'s device half — same `$(ISO)`/`$(DISK)` build
failure (unrelated `c/kernel/mm/pcache.c` compile errors, out of scope) noted
in the webapi and jsperf PROVENANCE.md files. The `qmp_css_repaint.py` check
was written and reviewed but not exercised against a live boot.

## $(DISK) dependency

None. Neither `bench-css` nor `bench-repaint` packs cssperf fixtures onto the
LogitFS image; `bench-repaint` serves them over a host HTTP server the same
way `qmp_bing.py` does.

## History

First added: commit `04e0c646d` ("css: the stylesheet was re-parsed on every
mutation, which is what \"repaints slowly\" was"), 2026-08-08. 563 commits
have been made since (inclusive) of 1,027 total. Not modified since the
initial add. Removing it from history would require a `git filter-repo`
rewrite; not run, a decision for the user.
