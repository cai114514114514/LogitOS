# Provenance: tests/fixtures/jsperf

## Mixed classification

Eight files are class A (verbatim third-party bundles, all rights reserved
by their publishers). One (`baidu-jquery-1.10.2.js`) is class B (a
third-party library redistributed under its own permissive MIT licence,
which the file itself does not carry — see below).

### Class A — verbatim third-party captures (8 files)

Per `tests/fixtures/jsperf/README`:

> "WHY THEY ARE COMMITTED RATHER THAN FETCHED ... These are the exact bytes
> the before/after numbers in the commit messages were measured on."
> "These are third-party programs kept verbatim as test input only, and are
> not part of LogitOS. ... The remaining files are (c) their respective
> sites (Moonshot AI, Baidu, DeepSeek, Microsoft) and are used here solely
> as fixed compiler input."

| file | bytes | source URL (from the README) | publisher |
|---|---|---|---|
| `kimi-index.mjs` | 1,623,655 | `https://statics.moonshot.cn/kimi-web-seo/assets/index-h6DE6Ow7.js` | Beijing Moonshot AI |
| `kimi-polyfills.mjs` | 96,106 | `.../assets/polyfills-BmY-sjxE.js` | Beijing Moonshot AI |
| `baidu-async-search.js` | 805,094 | `https://pss.bdstatic.com/r/www/cache/.../all_async_search_f9d4473.js` | Baidu, Inc. |
| `baidu-polyfill.js` | 41,984 | `https://pss.bdstatic.com/r/www/cache/.../polyfill_9354efa.js` | Baidu, Inc. |
| `deepseek-6559.js` | 191,620 | `https://www.deepseek.com/_next/static/chunks/6559-c46c52cda76e1166.js` | Hangzhou DeepSeek AI |
| `deepseek-86b88eeb.js` | 171,906 | `.../chunks/86b88eeb-ec19e45782d2c43b.js` | Hangzhou DeepSeek AI |
| `deepseek-polyfills.js` | 91,460 | `.../chunks/polyfills-c67a75d1b6f99dc8.js` | Hangzhou DeepSeek AI |
| `bing-uCMySA2.js` | 34,498 | `https://www.bing.com/rp/uCMySA2WQ3cNt4c2N2n7j7QAXrE.js` | Microsoft Corporation |

Total: 3,060,323 bytes (~2.9 MiB).

### Class B — jQuery 1.10.2 (`baidu-jquery-1.10.2.js`, 143,929 bytes)

Captured verbatim off Baidu's CDN (`README`: "144 KB of hand-written,
non-minified library code ... The opposite input to a minified chunk"), but
the file's own content **is** the jQuery library, and jQuery is MIT-licensed
by the jQuery Foundation — a fact the README already states ("jquery 1.10.2
is MIT (jQuery Foundation)") without the licence text travelling with the
file. The captured bytes do not contain jQuery's own `MIT-LICENSE.txt`
banner (its first ~200 bytes are the minified-looking preamble
`(function(window,undefined){var readyList,...`, not a comment header — this
build of jQuery 1.10.2 as served by Baidu's CDN carries no licence comment).

**Fixed in this pass**: the real, upstream `MIT-LICENSE.txt` for jQuery
1.10.2 (fetched from `https://raw.githubusercontent.com/jquery/jquery/1.10.2/
MIT-LICENSE.txt`, the tag matching the exact version in the filename) is
quoted below verbatim, so the required "above copyright notice and this
permission notice" (see `LICENSES/MIT.txt:9-10` in this repo) travels beside
the file it covers rather than only being asserted in prose:

```
Copyright 2013 jQuery Foundation and other contributors
http://jquery.com/

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Verified byte-for-byte against the GitHub raw file at that tag on 2026-08-21
(`curl https://raw.githubusercontent.com/jquery/jquery/1.10.2/MIT-LICENSE.txt`);
not recalled from memory, per house style. The generic
`LICENSES/MIT.txt` in this repository names "Logit OS contributors" as the
copyright holder and does **not** satisfy MIT's copyright-notice condition
for this file — a different holder's name is not "the above copyright
notice" — which is why it is quoted here specifically rather than pointed at.

## Consuming gates

| gate | file | reads | CI? |
|---|---|---|---|
| `bench-js` | `tests/jsperf.mk` | `JSPERF_HOST_FIXTURES := $(wildcard .../*.js .../*.mjs)` — all 9 files | no (bench, not `test-*`) |
| `bench-js-os` | `tests/jsperf.mk` | `JSPERF_GUEST_PATHS`, derived from `JSPERF_GUEST_FIXTURES` (3 of the 9: `baidu-polyfill.js`, `deepseek-6559.js`, `kimi-index.mjs`) | no |
| `test-js-syntax` | `tests/jsperf.mk` | `$(JSPERF_DIR)/baidu-polyfill.js` (one check inside `tests/unit/js_syntax_test.c`, plus 37 fixture-independent literal/syntax checks) | **yes** |
| `test-js-syntax-control` | `tests/jsperf.mk` | same file, via the negative-control binary | yes (its own gate) |
| `$(DISK)` (the LogitOS disk image itself) | `tests/jsperf.mk:34` (was: `$(DISK): $(BUILD)/jsbench.aex $(JSPERF_GUEST_FIXTURES)`) | the same 3 guest files, as a **hard Make prerequisite of the whole disk image** | indirectly, every boot test |

**The $(DISK) line was the most serious finding in this whole audit.**
Before this change, `JSPERF_GUEST_FIXTURES` was a bare list of three literal
paths (`$(JSPERF_DIR)/baidu-polyfill.js ...`) used directly as a
prerequisite of `$(DISK)`. Removing any of the three class-A files would not
have failed a *test* — it would have failed the **Make dependency graph
itself** with `No rule to make target 'tests/fixtures/jsperf/....js'`,
taking down `$(DISK)` and therefore every boot test in the tree (`test`,
`test-nvme`, `test-smp`, the whole `tests/qmp/*` suite — everything that
needs a disk image), over three files that only `bench-js-os` and
`test-js-syntax` actually need.

## What was changed to make an absence a clean SKIP

1. **`tests/jsperf.mk`**: `JSPERF_GUEST_FIXTURES` now uses
   `$(wildcard ...)` per file instead of a bare path list, so a missing file
   drops out of the list — and therefore out of `$(DISK)`'s prerequisites and
   `JSBENCH_PACK` — instead of hard-failing `make`.
2. **`tests/jsperf.mk`**: `bench-js` and `bench-js-os` each print a named
   skip and exit 0 when their respective fixture list is empty.
3. **`tests/unit/js_syntax_test.c`**: the one fixture-dependent check ("real
   page fixture") no longer calls `fail()` when the file cannot be read; it
   prints a skip line naming the re-fetch command and simply does not run
   that one check, leaving the 37 fixture-independent literal/syntax checks
   (including the four `"... literal then property access"` checks the
   negative control reverts) to run and decide the exit code as before.

## Proof (run 2026-08-21, host-only)

Control (fixture present): `make test-js-syntax` → `js_syntax_test: 38
checks pass`; `make bench-js` → all 9 files compiled, `JSBENCH-TOTAL 3200252
bytes in 118.4 ms ... JSBENCH-DONE 0`.

`tests/fixtures/jsperf` moved aside in full
(`mv tests/fixtures/jsperf tests/fixtures/jsperf.MOVED_ASIDE`):
```
$ make test-js-syntax
js_syntax_test: tests/fixtures/jsperf/baidu-polyfill.js not present -- skipping the real-page check (37 literal/syntax checks above still ran).
js_syntax_test: this is not a failure. Re-fetch with (see tests/fixtures/jsperf/README):
  curl -A 'Mozilla/5.0' --compressed https://pss.bdstatic.com/r/www/cache/static/protocol/https/bundles/polyfill_9354efa.js -o tests/fixtures/jsperf/baidu-polyfill.js
js_syntax_test: 37 checks pass
js_hash_test: 405200 comparisons pass (hash_string8 == upstream == hash_string16)
$ echo $?
0
$ make bench-js
bench-js: no corpus under tests/fixtures/jsperf -- nothing to measure.
bench-js: this is not a failure. Re-fetch with (see tests/fixtures/jsperf/README):
  curl -A 'Mozilla/5.0' --compressed <url> -o tests/fixtures/jsperf/<name>.js
$ echo $?
0
```
Both SKIPPED, neither FAILED (exit 0 both times; `test-js-syntax` degraded
gracefully — 37 of 38 checks, still green — rather than skipping wholesale,
since only one of its checks actually needs the fixture).

Directory moved back; negative control re-run: `test-js-syntax` → 38 checks
(back to the full count); `bench-js` → all 9 files again, `JSBENCH-DONE 0`.

**Not separately proved**: `$(DISK)`'s wildcard-based prerequisite shrink.
`$(ISO)`/`$(DISK)` do not currently build in this tree for an unrelated
reason (`c/kernel/mm/pcache.c` compile errors, out of this task's scope —
see `tests/fixtures/webapi/PROVENANCE.md`'s note on the same failure). The
`$(wildcard ...)` fix is a standard, widely-used idiom already relied on
elsewhere in this same tree (`tests/cssweb.mk`'s `AUDIT_DIRS`) and was
verified by reading `make -n $(DISK)`'s expansion, not by a live disk build:
```
$ wsl make -n build/disk.img 2>&1 | grep -c jsperf
0
```
(zero matches with the corpus absent — confirms the three fixture paths do
not appear as literal, unconditional prerequisites any more).

## History

First added: commit `08d261591` ("js: what four real pages cost this engine,
measured on the machine"), 2026-08-08. 504 commits have been made since
(inclusive) of 1,027 total. `git log --oneline -- tests/fixtures/jsperf`
shows the directory has not been modified since the initial add. Removing
the 8 class-A files from history would require a `git filter-repo` rewrite;
not run, a decision for the user. `baidu-jquery-1.10.2.js` (class B) needs no
such removal — it is redistributable under its own licence, now travelling
with it above.
