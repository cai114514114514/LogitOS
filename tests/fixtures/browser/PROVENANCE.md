# Provenance: tests/fixtures/browser

## Mixed classification

### Class C — project-authored (`links.html`, `list.html`, `media.html`, `table.html`)

Per `tests/fixtures/browser/README`: "Small hand-written pages for the
layout and paint tests." Written by this project, no external content.

### Class A — verbatim third-party capture (`baidu.html`, `baidu-ua-stub.html`)

Per the README:

> "THE SAME URL, https://www.baidu.com/, fetched twice on 2026-08-08 with
> nothing different but the User-Agent header. They are the evidence for the
> 'the browser fetches a page and then shows nothing' bug ..."

| file | bytes | how captured | publisher |
|---|---|---|---|
| `baidu-ua-stub.html` | 227 | `curl -H 'User-Agent: Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0' -H 'Accept: */*' https://www.baidu.com/` | Baidu, Inc. |
| `baidu.html` | 697,700 | `curl --compressed -H 'User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36' https://www.baidu.com/` | Baidu, Inc. |

Both are (c) Baidu, Inc., kept verbatim as test input (`baidu.html` "Stored
DECODED" per the README — the wire body is chunked gzip and the committed
bytes are what remains after `Content-Encoding` is undone, not a re-encoding
of new content).

Total (these two files only): 697,927 bytes.

## Consuming gates

| gate | file | reads | CI? |
|---|---|---|---|
| `test-loader` | `tests/loader.mk`, prerequisite of `test-browser` | `tests/fixtures/browser/baidu.html` **and** `baidu-ua-stub.html`, directly, in `tests/unit/loader_test.c` | likely — `test-browser` is broadly referenced across the tree |
| `test-loader-negctl` | same | same `LOADER_SRC`, same file | its own gate |
| `test-loader-asan` | same | same | ASan build |
| `test-tabs-negctl` | same | `g_real_html` sourced from `baidu.html` at the bottom of `loader_test.c`'s `main()`, used by `part3_tabs()` | its own gate |
| `test-script-nav`, `test-tabs` (device) | `tests/loader.mk` → `tests/qmp/qmp_script_nav.py`/`qmp_tabs.py` | **not this directory** — both scripts bake their own reduced/synthetic fixture (a hand-copied "baidu's real stub" string in `qmp_script_nav.py`, and two hand-written pages in `qmp_tabs.py`); neither reads `tests/fixtures/browser` at runtime, so neither needed the skip treatment |

**This was the most consequential single test file in the audit.** Every
part of `loader_test.c` (parts 1 through 3 — navigation, the vite probe,
`document.domain`, inserted `<script>`s, the watchdog, and the tab test)
either directly asserts on the two baidu captures or is reached only after
`slurp("tests/fixtures/browser/baidu.html", ...)` at the top of `main()`,
whose old behaviour on a missing file was `exit(2)` with `"FAIL: cannot open
fixture ..."` — a hard failure partway through the file, not a skip.

## What was changed

`tests/unit/loader_test.c`: added a `fixtures_present()` check (`stat()` on
both files) at the very top of `main()`. If either is absent, prints a named
skip (the two `curl` commands from the README, so the message doubles as a
regeneration recipe) and returns 0 **before** `css_init()` or any of parts
1–3 run. The file's own docstring already argues why a partial skip would be
meaningless: "This whole file is a diagnosis of one pair of captured bytes
... There is no reduced version of this test that means anything without
them."

## Proof (run 2026-08-21, host-only)

Control (fixture present): `make test-loader` → `loader_test: PASS`, 76 `ok:`
lines (`./build/loader_test 2>&1 | grep -c '^ok:'` = 76).

`tests/fixtures/browser` moved aside
(`mv tests/fixtures/browser tests/fixtures/browser.MOVED_ASIDE`):
```
$ make test-loader
loader_test: tests/fixtures/browser/baidu{,-ua-stub}.html not present -- nothing to measure.
loader_test: this is not a failure. Re-capture with (see tests/fixtures/browser/README):
  curl -H 'User-Agent: Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0' https://www.baidu.com/ -o tests/fixtures/browser/baidu-ua-stub.html
  curl -H 'User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36' --compressed https://www.baidu.com/ -o tests/fixtures/browser/baidu.html
$ echo $?
0
```
SKIPPED, not FAILED (note: `links.html`/`list.html`/`media.html`/`table.html`
were also moved aside as part of this test since they share the directory,
but nothing in `loader_test.c` reads them — the class-C files are unrelated
to this gate). Directory moved back; `make test-loader` re-run:
`loader_test: PASS` again, 76 `ok:` lines again — matching the pre-removal
run exactly.

## $(DISK) dependency

None. `test-loader` is a host-only link (`c/apps/browser/browser.c` built
against `tests/unit/loaderhost` stand-ins for the window and network); no
fixture bytes ride the LogitOS disk image.

## History

First added: commit `c75c4bebe` ("browser: the page said \"go somewhere
else\" and the loader never listened"), 2026-08-08. 537 commits have been
made since (inclusive) of 1,027 total. (The directory itself — with the four
hand-written class-C pages — existed earlier, from `117a5b4d4`, 2026-08-05;
only `baidu.html`/`baidu-ua-stub.html` date from `c75c4bebe`.) Neither file
has been modified since its add. Removing them from history would require a
`git filter-repo` rewrite; not run, a decision for the user.
