# Provenance: tests/fixtures/webapi

`.gitattributes` in this directory (`* -text`) is repository hygiene, not
vendored data: the corpus below is the input to a byte-offset measurement, so
this file stops Git's CRLF handling from silently changing the captured
bytes. It has no provenance of its own to record.

## Mixed classification — read this before treating the directory as one unit

This directory is NOT uniformly class A. Nine of its eleven entries are
verbatim third-party captures; one (`logreporter`) is project-authored; one
(`xtweet`) is an empty, untracked leftover of a reverted commit.

### Class A — verbatim third-party, all rights reserved (9 subdirectories)

Captured with `tests/fixtures/webapi/capture.py`, whose own header states the
reason and the method:

> "WHY THE CORPUS IS CAPTURED AND COMMITTED, NOT FETCHED ... These are the
> exact bytes the numbers in the commit message were measured on."
> "THE USER-AGENT MATTERS AND IS THEREFORE PINNED ... the default UA here is
> byte-for-byte the one js_page.c publishes as navigator.userAgent."

`UA = "Mozilla/5.0 (LogitOS; x86_64) Logit/1.0"` (`capture.py:39`). Each
subdirectory's `manifest.txt` records "`<src attribute>\t<local file>`" for
every `<script src>` (including transitive module-graph chunks, see the
script's "MODULE GRAPHS ARE WALKED" section) and `SOURCE` records the exact
URL fetched plus the UA used.

| directory | URL (from its own `SOURCE`) | publisher |
|---|---|---|
| baidu | https://www.baidu.com/ | Baidu, Inc. |
| baidureal | http://www.baidu.com/ | Baidu, Inc. |
| bing | https://www.bing.com/ | Microsoft Corporation |
| deepseek | https://www.deepseek.com/ | Hangzhou DeepSeek AI |
| example | https://example.com/ | IANA (example.com is IANA's reserved example domain; conventionally treated as free to use, but still not this project's own content) |
| kimi | https://www.kimi.com/ | Beijing Moonshot AI |
| mdn | https://developer.mozilla.org/en-US/ | Mozilla Foundation |
| nodejs | https://nodejs.org/ | OpenJS Foundation / Node.js contributors |
| wikipedia | https://en.wikipedia.org/wiki/Operating_system | Wikimedia Foundation (article text is CC BY-SA 4.0; the page's own script/style bundles are Wikimedia's) |

Total (these 9 dirs only): 16,450,690 bytes (~15.7 MiB) of
`tests/fixtures/webapi`'s 17 MB.

### Class C — project-authored reduction (`logreporter/`, 5,124 bytes)

Its own file header says this explicitly — it is not a capture:

> "A REDUCTION, NOT A CAPTURE -- and the distinction matters, because every
> other directory here is committed bytes off the wire (see capture.py) and
> this one is not. What is committed here is the SHAPE of the two functions
> the 2026-08-16 scoreboard named in a stack, written out unminified so a
> test can assert on it ... WHY THERE IS NO index.html HERE."

It reduces two named functions (`getCurrentDomain`, `setCookie`) from
bilibili's `log-reporter.js` to their call shape, hand-written by this
project. Consumed by `test-logreporter` (`tests/logreporter.mk`, wired into
`ci-host`). Not touched by this audit's removal/skip work — it is not class A.

### `xtweet/` — empty, not tracked

`git ls-files tests/fixtures/webapi/xtweet/` returns nothing. The directory
holds no files on disk (`ls -la` shows only `.`/`..`). History:
commit `efbae04bf` ("fixtures: a tweet, which needs one API we do not have")
added an x.com capture; commit `9efff4b2b` ("Revert \"fixtures: a tweet...\"")
reverted it the same day because `capture.py`'s module-graph walk timed out
mid-capture and the committed bytes were 1.9 MB of scripts the probe could
never have wired up (no `manifest.txt` was written). The empty directory on
disk is a leftover of the revert, not a tracked artifact — `git status`
does not show it because git does not track empty directories.

## Consuming gates

| gate | file | reads |
|---|---|---|
| `probe-webapi` | `tests/webapi_platform.mk` | `WEBAPI_FIXTURES := $(wildcard tests/fixtures/webapi/*/index.html)` — every subdir with an `index.html` (the 9 class-A ones; `logreporter` and `xtweet` have none, by `logreporter`'s own design — see above) |
| `test-bing` | `tests/webapi_platform.mk` (device, boots QEMU) | `tests/fixtures/webapi/bing/{index.html,manifest.txt}` directly, via `tests/qmp/qmp_bing.py` |
| `test-frameworks` / `probe-frameworks` | `tests/frameworks.mk` | the **separate** `tests/fixtures/frameworks` corpus — not this directory (see that PROVENANCE.md) |
| `test-logreporter` (+ `-negctl`/`-asan`) | `tests/logreporter.mk`, wired into `ci-host` | `tests/fixtures/webapi/logreporter` only (class C, untouched) |

## What happens now when the class-A subdirectories are absent

`probe-webapi`:
```
probe-webapi: no corpus under tests/fixtures/webapi -- nothing to measure.
probe-webapi: this is not a failure. Re-capture with:
  ./tests/fixtures/webapi/capture.py <name> <url>
```
`test-bing` (Makefile-level check before booting QEMU, plus the same check
duplicated inside `tests/qmp/qmp_bing.py` for anyone invoking the script
directly):
```
test-bing: tests/fixtures/webapi/bing/index.html not present -- nothing to measure.
test-bing: this is not a failure. Re-capture with:
  ./tests/fixtures/webapi/capture.py bing https://www.bing.com/
```

**Proved 2026-08-21 (`probe-webapi`, host-only)**: `tests/fixtures/webapi`
was moved aside in full (including `logreporter`, for a clean "whole corpus
absent" test) and `make probe-webapi` printed the skip message above and
exited 0. The directory was moved back and `make probe-webapi` was re-run:
9 sites probed, 114 distinct missing-global names ranked, 27 total uncaught
exceptions (`bing 1, mdn 8, nodejs 18`) — nonzero, matching the pre-removal
run byte for byte (same corpus, same probe).

**NOT run**: `test-bing`'s device half. `$(ISO)`/`$(DISK)` do not currently
build in this tree — `make bench-js-os` (a different, unrelated $(ISO)/$(DISK)
build) failed during this same session on `c/kernel/mm/pcache.c:822: call to
undeclared function 'reclaim_low'` and a `want` redefinition, both inside
`c/kernel/**`, which is out of this task's scope and appears to be a
concurrent, in-progress edit by another session (the CLAUDE.md system
reminder names three such workflows explicitly running beside this one). The
Makefile-level file-existence check and the matching check inside
`qmp_bing.py` were both written and are believed correct by inspection
(same `os.path.exists` / shell `[ -f ... ]` pattern verified working for
every host-side gate above), but the actual boot was not exercised — do not
read this PROVENANCE.md as claiming it was.

## $(DISK) dependency

None from this directory. `test-bing`'s fixture bytes are served to the guest
over a host HTTP server inside the QMP harness (`http.server` in
`qmp_bing.py`), not packed onto the LogitFS image.

## History

First added: commit `c18adacbe` ("js: measure which globals real pages miss,
then provide them"), 2026-08-08. 527 commits have been made since
(inclusive) of 1,027 total. Individual subdirectories were added across five
more commits the same day (`7acd4e046`, `9331f9133`, `efbae04bf`/`9efff4b2b`
add+revert, `265a7f90d`) as the corpus and the probe it drives grew. Removing
the class-A subdirectories from history would require a `git filter-repo`
rewrite (invalidates every clone); not run, a decision for the user.
