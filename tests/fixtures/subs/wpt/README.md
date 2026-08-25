# WebVTT file-parsing corpus (extracted from web-platform-tests)

**Where this came from, and why it is here instead of vendored the way the
rest of this tree's WPT usage works.** `tools/wpt_fetch.sh`'s `SUBSETS` list
(the corpus `make test-wpt` pulls into `build/wpt`, gitignored) does not
include `webvtt/` at all -- `dom`, `html/dom`, `html/semantics`, `encoding`,
`url`, `console` and `css` are the layers that subset was built for, and
nobody had added a text-track consumer to this tree yet when it was written.
So the task this directory exists for ("the WebVTT spec's own test corpus...
if absent, say so and use a committed subset") is exactly this situation:
absent, said so, and this is the subset.

**What is actually here.** The 38 `*.vtt` files were extracted from
`webvtt/parsing/file-parsing/support/*.test` in the upstream repo at the pin
below -- each `.test` file is a title, an HTML-metadata line, a block of JS
`assert_equals` calls against a loaded `<track>`'s cues, a `===` separator,
and the WebVTT body itself (Python `unicode_escape`-encoded, per the
upstream `tools/build.py`). Only the WebVTT body was extracted; the JS
assertions were read once, by hand, to build tests/unit/subs_oracle.py (an
independent re-implementation of the same spec algorithm the JS assertions
describe -- see that file's own header for why it is not a transcription of
the JS) and tests/unit/subs_test.c's hand-written `test_*` cases for the
handful of files traced directly against the spec text. The other 10
`*.vtt` files (`empty.vtt`, `signature-*.vtt`) are upstream's raw fixture
files, copied byte for byte -- they carry no `.test` wrapper because they
are the "invalid signature, must not parse" half of the corpus
(`signature-invalid.html` in the upstream tree loads each one and asserts an
`error` event).

Upstream revision: `841f08ffcef5430c77cf4b4c6ad1d5f6625880da` (the same pin
`tools/wpt_revision.txt` uses for the rest of this tree's WPT corpus, fetched
the same day). Extraction script: not committed (a one-off `curl` + a small
Python `unicode_escape`-equivalent unescaper run against
`webvtt/parsing/file-parsing/support/*.test` at that revision) -- re-running
it is `git log`-able as "how tests/subs.mk's fixtures were produced" if this
corpus ever needs to be refreshed against a newer WPT revision.

License: these files are data from web-platform-tests, itself BSD-3-Clause-ish
(`LICENSE.md` upstream) -- the same license basis `third_party/wpt` already
carries elsewhere in this repository.

## What the gate does with these

`tests/subs.mk`'s `test-subs-wpt-diff` target runs every `.vtt` here through
BOTH `subs_test cues` (the C library under test) and `subs_oracle.py` (an
independently-written second implementation of the same spec, sharing no
code with subs.c) and diffs the two field by field via `subs_diff.py` --
numeric fields with a tolerance (a WebVTT `line` value can legitimately be
`Number.MAX_VALUE`), everything else exactly. Files whose names start with
`signature-` and are NOT also present as `.test`-derived valid cases (i.e.
`empty` and the nine `signature-*` raw `.vtt` files) are expected to produce
`FORMAT-ERROR` from both sides -- see `subs_diff.py`'s `EXPECT_FORMAT_ERROR`
list, built from this directory's own file names rather than hand-copied, so
a fixture added here without updating anything else is checked by default
rather than silently skipped.
