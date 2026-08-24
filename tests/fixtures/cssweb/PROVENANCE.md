# Provenance: tests/fixtures/cssweb

## Classification: A — verbatim third-party work, all rights reserved

All 15 subdirectories are unmodified HTML + CSS captured verbatim from live,
high-traffic third-party websites. Per `tests/fixtures/cssweb/README`:

> "The CSS corpus: 15 real, high-traffic pages, captured verbatim."
> "Captured 2026-08-08, unmodified. These are third-party pages kept verbatim
> as test input only; each remains the copyright of its publisher. MANIFEST
> in each directory records exactly where every byte came from."

Each subdirectory's own `MANIFEST` file records the exact source URL for
`index.html` and every `sheet-N.css`. Captured with
`tests/fixtures/cssweb/capture.py`, a Chrome-shaped User-Agent (see the
script, `UA = "Mozilla/5.0 ... Chrome/126.0 Safari/537.36"`), no modification
after capture.

| directory | site | publisher |
|---|---|---|
| 163 | 163.com | NetEase |
| apnews | apnews.com | The Associated Press |
| apple | apple.com | Apple Inc. |
| baidu | baidu.com | Baidu, Inc. |
| bbc | bbc.com | BBC |
| bing | bing.com | Microsoft Corporation |
| ddg | duckduckgo.com | DuckDuckGo, Inc. |
| github | github.com | GitHub, Inc. (Microsoft) |
| hn | news.ycombinator.com | Y Combinator |
| mdn | developer.mozilla.org | Mozilla Foundation |
| pydocs | docs.python.org | Python Software Foundation |
| qq | qq.com | Tencent |
| sina | sina.com.cn | Sina Corporation |
| tailwind | tailwindcss.com | Tailwind Labs |
| web-dev | web.dev | Google LLC |

Total: 119 files, 14 MB (`du -sh tests/fixtures/cssweb` = 14M).

## Consuming gates

| gate | file | reads |
|---|---|---|
| `audit-css` | `tests/cssweb.mk` | `AUDIT_DIRS := $(wildcard tests/fixtures/cssweb/*/index.html)` — every subdir |
| `audit-css-before` | `tests/cssweb.mk` | same `AUDIT_DIRS` |
| `bench-arena` | `tests/mem.mk` | `ARENA_PAGES := $(wildcard tests/fixtures/cssweb/*/index.html)` — same corpus, independent variable name |

None of these are `test-*` named (none are CI gates that can turn red on
their own); all three are reporters/benchmarks. All three were, before this
change, silent-but-uninformative on an absent corpus (`$(wildcard ...)`
already degrades to an empty list, so the underlying C binary just printed an
empty table) rather than SKIPping with a stated reason — fixed below.

## What happens now when the corpus is absent

All three targets print a named skip and exit 0:

```
audit-css: no corpus under tests/fixtures/cssweb -- nothing to measure.
audit-css: this is not a failure. Re-capture with (see tests/fixtures/cssweb/README):
  ./tests/fixtures/cssweb/capture.py <name> <url>
```

(`audit-css-before` and `bench-arena` print the analogous message.)

**Proved 2026-08-21**: `tests/fixtures/cssweb` was moved aside
(`mv tests/fixtures/cssweb tests/fixtures/cssweb.MOVED_ASIDE`) and
`make audit-css`, `make audit-css-before` and `make bench-arena` were each
run — all three printed the skip message above and exited 0 (`make`'s own
exit code, not just the recipe's first line). The directory was moved back
and all three were re-run as the negative control:
`audit-css` printed a 30-row ranked-declaration table with `display 8124 15
2593` (8,124 declared across all 15 pages) at the top; `bench-arena` printed
one row per page, 15 rows, `163 … 11876` KiB resident down to `web-dev …
19788` KiB; `audit-css-before` was proved to SKIP the same way (the negctl
binary takes long enough to build — it re-patches and recompiles LibCSS —
that the RUN half was not separately re-timed; it shares the identical
if/absent-then-skip/else-run recipe shape as `audit-css`, which the full RUN
proof already covers).

## Build-side note (unrelated to fixtures, fixed to make the proof possible)

`$(BUILD)/css_audit`, `$(NEGDIR)/css_audit_before` and
`$(BUILD)/arena_page_mem` link `c/apps/browser/css_engine.c`, which pulls in
LibCSS's `canon.c` (`floor`/`pow`/`fmod`/`log10`) with no `-lm` on the link
line — the same pre-existing defect CLAUDE.md's test-suite section already
names for a different target (`test-css-web-negctl canon.c -> floor()
(missing -lm)`). Without `-lm` none of the three targets link at all,
corpus present or not, so `-lm` was added to all three recipes in
`tests/cssweb.mk` and `tests/mem.mk` — required to run the proof above, not
a fixtures/licensing change.

## $(DISK) dependency

None. Nothing under `tests/fixtures/cssweb` rides the LogitOS disk image; all
three consumers are host-only tools.

## History

First added: commit `43aabc5b9` ("css: the corpus that ranks the work, and
the logical properties it ranked"), 2026-08-08. 514 commits have been made
since (inclusive), of 1,027 total in the repository — the corpus has been
part of every one of those commits' tree. It has never been modified since
(`git log --oneline -- tests/fixtures/cssweb` shows exactly one commit, the
add). Removing it from history (not from the current tree) would require a
`git filter-repo` rewrite, which invalidates every existing clone; that is a
decision for the user and was not run.
