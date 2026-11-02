# Testing the browser

Browser tests exercise production components on the host, load packaged pages in QEMU, and check the connection between application sources, linked code, and test runners. Host suites provide focused assertions over parsing, bindings, layout, and resource lifetimes. Guest harnesses observe network requests, keyboard and pointer input, downloaded bytes, and displayed pixels. The recipes and harnesses cited below define the inputs and evidence for each check.

## Choose a test by module

Run commands from the repository root. `Makefile` includes the fragments that define these entry points.

| Module | Host tests | Guest tests |
|---|---|---|
| HTML parsing | `test-html5lib-tok` checks tokenization, and `test-html5lib STRICT=1` compares tree construction against a baseline of expected failures; `Makefile` defines both. | `test-css-components-guest` loads an HTML fixture with external CSS and images. `test-html-priority-guest` exercises documents containing scripts, forms, and a Worker. See `tests/css_guest.mk` and `tests/html_priority.mk`. |
| DOM and event bindings | `test-dom-iface` checks interface inheritance; `test-dom-wrapper-lifetime` checks retained JavaScript state and wrapper collection; `test-events` checks dispatch ordering. See `tests/domiface.mk`, `tests/dom_wrapper_lifetime.mk`, and `tests/events.mk`. | `test-live-page` exercises handlers, DOM mutation, timers, and navigation cancellation. `test-forms-device` checks focus, typing, and submission. See `Makefile` and `tests/forms.mk`. |
| CSS and CSSOM | `test-css-extra-selector`, `test-css-extra-cascade`, `test-cssom`, and `test-csstyle` check selector matching, cascade results, stylesheet interfaces, geometry, and computed-style flushing. See `tests/css_extra_selector.mk`, `tests/css_extra_cascade.mk`, `tests/cssom.mk`, and `tests/csstyle.mk`. | `test-css-components-guest` checks external stylesheets and nesting. `test-stylesheet-interface-guest` exercises stylesheet changes using supplied positive and control images. See `tests/css_guest.mk` and `tests/cssom.mk`. |
| Layout and text | `test-css-layout` checks aspect ratios, flex columns, and grid geometry. `test-csstext` checks line breaking and CSS text processing. See `tests/css_layout.mk` and `tests/csstext.mk`. | `test-css-fidelity` measures painted widths, preformatted text, and flex wrapping. `test-css-components-guest` checks rectangles reported by the page. See `Makefile` and `tests/css_guest.mk`. |
| Painting | `test-browser` includes paint assertions. `test-css-background`, `test-css-gradients`, and `test-css-background-layers` compare geometry and pixels with fixture expectations. See `Makefile`, `tests/css_background.mk`, `tests/css_gradients.mk`, and `tests/css_background_layers.mk`. | `test-css-guest` gathers component, background, gradient, and layer checks against captured scanout. See `tests/css_guest.mk`. |
| JavaScript and page lifecycle | `test-jssem` compares engine output with Node.js. `test-page-runtime` checks callback ordering and lifetime through the page runtime. See `tests/jssem.mk` and `tests/page_runtime.mk`. | `test-jssem-os` runs the engine probe using the browser’s engine objects. `test-modules` checks module loading and resolution; `test-platform-page` exercises selectors, tasks, observers, and storage in a rendered page. See `Makefile`, `tests/jssem.mk`, and `tests/webapi_platform.mk`. |
| Loading and Fetch | `test-webapi`, `test-fetch-body`, `test-browser-loading`, and `test-browser-close-load` check request handling, body bytes, loading state, and resource cleanup. See `Makefile`, `tests/fetch_body.mk`, `tests/browser_loading.mk`, and `tests/browser_close_during_load.mk`. | `test-webapi-page` checks Fetch through DOM updates and pixels. `test-fetch-firstbyte` exercises stalled responses; `test-abort-guest` checks cancellation. See `Makefile`, `tests/fetchdl.mk`, and `tests/abort.mk`. |
| Frames and document policy | `test-frame` checks frame behavior and lifecycle. `test-frame-sandbox-host` exercises document restrictions through browser consumers. See `tests/frame.mk` and `tests/frame_sandbox.mk`. | `test-passive-frame-guest` checks frame rendering. `test-frame-sandbox-guest` runs a policy fixture and a browser control. See `tests/passive_frame.mk` and `tests/frame_sandbox.mk`. |
| Workers and streams | `test-worker`, `test-worker-fetch`, and `test-html-streams` check completion, termination, Fetch ownership, and stream behavior. See `tests/worker.mk`, `tests/worker_fetch.mk`, and `tests/html_streams.mk`. | `test-html-priority-guest` validates page and Worker assertion groups. `test-abort-guest` checks cancellation during page and Worker execution. See `tests/html_priority.mk` and `tests/abort.mk`. |
| WebAssembly | `test-wasm-js` checks JavaScript integration; `test-wasm-lifecycle` exercises cleanup through page closure. See `tests/wasm.mk` and `tests/wasm_lifecycle.mk`. | `test-wasm-page` fetches modules, checks computed answers, and observes the rendered result. Its prerequisite control serves a truncated module. See `tests/wasm.mk`. |
| Storage, cryptography, and downloads | `test-storage-backend`, `test-storage-persistence`, `test-web-crypto-keys`, and `test-browser-downloads` check storage consumers, persistence, key operations, and download bookkeeping. See `tests/storage_backend.mk`, `tests/storage_persistence.mk`, `tests/web_crypto_keys.mk`, and `tests/browser_downloads.mk`. | `test-platform-page` includes storage in its page workflow. `test-download-keys-os` checks key operations, native download actions, and saved file contents. See `tests/webapi_platform.mk` and `tests/browser_downloads.mk`. |
| Media | `test-mse` exercises the media engine and its controls. See `tests/mse.mk`. | `test-video-page` checks direct video and MediaSource playback through frame progress, playback completion, and captured audio. See `tests/mse.mk` and `tests/qmp/qmp_video_page.py`. |

For a broader host pass, `test-browser` compiles and runs tests for DOM, CSS, layout, paint, JavaScript bindings, HTTP, cookies, and connection pools. `test-browser-expansion` gathers later integration gates, including loading, storage, WebAssembly lifecycle, and browser wiring. Both are defined in `Makefile` and `tests/browser_expansion.mk`.

## Run a focused test

Choose the target closest to the behavior being changed:

```sh
make test-css-layout
make test-dom-iface
make test-fetch-body
```

These targets run their negative controls as prerequisites. A control introduces a known defect and checks that the test detects it. In `tests/css_layout.mk`, controls disable handling of aspect ratios, restore incorrect sizing of flex columns, change grid alignment, and omit grid bounds. Each invocation must exit with the expected status and print the assertion selected by its recipe.

The Fetch body controls apply stronger output checks: `tests/fetch_body.mk` compares the complete set of failing assertions with committed expectation files. This catches controls that fail additional cases because their harness broke.

Instrumented variants are available for memory checks:

```sh
make test-css-layout-san
make test-fetch-body-san
make test-browser-close-load-sanitize
```

These recipes enable AddressSanitizer and UndefinedBehaviorSanitizer. Their flags and settings for leak detection are defined in `tests/css_layout.mk`, `tests/fetch_body.mk`, and `tests/browser_close_during_load.mk`.

Pass `BUILD` on the `make` command line to select a scratch artifact directory. `Makefile` assigns its default internally, so the command line wins.

For a focused guest check:

```sh
make test-css-components-guest
```

`tests/css_guest.mk` makes the relevant host gates, ISO, and application disk prerequisites of this target. The disk contains the packaged browser application; `Makefile` makes the default `all` target depend on the ISO.

The guest target for the stylesheet interface has different setup requirements. `tests/cssom.mk` requires `SHEET_GUEST_ISO`, `SHEET_GUEST_DISK`, and `SHEET_GUEST_NEG_DISK` to identify prebuilt images. It runs the control image before the positive image.

## Run Web Platform Tests

The Web Platform Tests (WPT) harness executes upstream test data through the browser’s DOM, layout, and Web API implementations. `tests/wpt.mk` derives its source list from browser build variables and provides host adapters for application services.

Prepare the corpus and check the runner before selecting a subset:

```sh
make wpt-fetch
make test-wpt-link
make test-wpt-harness
make test-wpt ONLY=dom/nodes V=20 J=1
```

`wpt-fetch` populates the destination selected by `WPT_ROOT`; its implementation is `tools/wpt_fetch.sh`. An existing corpus can be selected through the same variable. `tests/wpt.mk` assembles these filters:

| Variable | Meaning |
|---|---|
| `ONLY` | Select paths containing a substring. |
| `SUBSET` | Select a corpus subset. |
| `V` | Limit printed unexpected failures. |
| `J` | Set concurrent file execution. |
| `ORDER` | Seed file-order shuffling. |

`test-wpt` uses strict mode to reject failures absent from `tests/unit/wpt_expected_fail.txt`. `wpt` prints the measurement, `wpt-rank` groups failure causes, and `wpt-baseline` writes a baseline. Review baseline changes together with the behavior and corpus that produced them.

The runner in `tests/unit/wpt_test.c` executes files in child processes and returns their results to the parent. Its report distinguishes subtest results from file crashes and harness failures. Compare these counts together: a change in executed files can also change the subtest denominator.

## Check the WPT runner

`test-wpt-link` checks source coverage against the browser’s build groups. Every selected browser source must appear in the runner’s sources, shared HTML parser sources, or `WPT_BROWSER_OUT`. Every exclusion must still belong to the browser’s selected sources. The exclusions in `tests/wpt.mk` describe application shell and transport components handled by host adapters.

Exercise both failure branches with:

```sh
make test-wpt-link WPT_TEST_SRC= WPT_BROWSER_OUT=
make test-wpt-link WPT_BROWSER_OUT=Makefile
```

Both commands are expected to fail. The first removes runner coverage and exclusions; the second introduces a stale exclusion.

`test-wpt-harness` checks execution through a fixture embedded in `tests/unit/wpt_test.c`. It checks synchronous assertions, an expected failing assertion, timers, promises, and counts of load events. Assertions about styled width and logical margin exercise the calls to `css_apply()`, `css_extra_apply()`, and `layout_page()`. These calls also appear in the application pipeline in `c/apps/gui/browser/app/main.c`.

`test-wpt` has two runner controls as prerequisites:

| Control | Mutation and check |
|---|---|
| `test-wpt-negctl` | Builds with `WPT_NEGCTL`, removes `document.createElement`, and requires a strict DOM run to fail against a baseline generated by the current unmodified runner. |
| `test-wpt-fire-negctl` | Builds with `WPT_DOUBLE_FIRE`, dispatches an additional load event, and requires the harness self-check to exit with failure. |

An older comment in `tests/wpt.mk` names `document.getElementById` as the removed method. The control implementation in `tests/unit/wpt_test.c` removes `document.createElement`.

## Check browser and Make wiring

Run these checks when changing installation calls or source membership:

```sh
make test-browser-wiring
make test-mk-wired
```

`tools/browser_wiring_audit.py` asks Make to expand browser source and object groups, follows included implementation files, checks reviewed patterns of source calls, and inspects strong symbols in the browser ELF. Its JSON report includes source hashes, an ELF hash, installer inventory, and freshness information.

`test-browser-wiring-negctl` is a prerequisite of the audit. `tests/unit/browser_wiring_audit_test.py` removes the `js_cssom_set_reflow(browser_cssom_reflow)` registration from a temporary source overlay while retaining the supplied ELF. The control requires the corresponding `MISSING_CALL cssom_reflow` failure. This tests the audit’s ability to distinguish a linked provider from its source registration.

`tools/mk_wired.py` inventories test fragments recursively and follows literal includes from `Makefile`. It joins Make continuations before traversing those edges. `tests/tools/make/wired.mk` adds `test-mk-wired-regression` as a prerequisite. Its fixtures in `tests/tools/make/wired.py` require a missing nested include to fail, restore the include, and require success. They also cover continued lines, include cycles, and fragments sharing a basename.

## Check pages in QEMU

The CSS harnesses in `tests/qmp/qmp_css_components.py` and `tests/qmp/qmp_css_background.py` connect page execution to geometry and pixels:

1. Start a host HTTP server serving fixtures and receiving page reports.
2. Boot the supplied ISO and disk using VirtIO storage, VirtIO graphics, and an emulated Ethernet device.
3. Wait for boot and desktop markers, launch the browser through the QEMU Machine Protocol (QMP), and navigate through `10.0.2.2`.
4. Receive rectangles measured by the guest page over HTTP.
5. Capture scanout and compare geometry and sampled colors with fixture expectations.

`tests/qmp/qmp_ui.py` maps browser client coordinates using the window geometry reported by the guest. The background harness reads pixel oracles from `tests/fixtures/engine-expansion/css-background-pixels.csv`, `tests/fixtures/engine-expansion/css-gradients-pixels.csv`, and `tests/fixtures/engine-expansion/css-layers-pixels.csv`. It waits for two successive samples with matching colors before evaluating the frame, allowing an incorrect stable frame to produce an assertion failure.

`tests/css_guest.mk` gives each invocation a unique evidence directory. `test-css-guest` gathers all four CSS guest targets.

Other harnesses use observations matched to their subject:

| Harness | Evidence checked |
|---|---|
| `tests/qmp/qmp_html_priority.py` | Page and Worker assertion groups reported over HTTP, native form input, completion text, and a scanout marker. |
| `tests/qmp/qmp_abort.py` | Partial response delivery, cancellation outcomes, Worker execution, server requests, and the painted completion marker. |
| `tests/qmp/download_keys_guest.py` | Native download actions, results of key operations, downloaded bytes read after QEMU exits, and signature verification through the host cryptography library. |
| `tests/qmp/qmp_video_page.py` | Increasing counts of presented frames, playback completion, and audio samples captured by QEMU’s WAV backend. |

Guest controls can replace a producer in the browser link and package the resulting application on a control disk. `tests/abort.mk` does this for cancellation ordering; `tests/frame_sandbox.mk` tests frame restrictions; `tests/fetchdl.mk` tests the deadline for the first response byte. Keep both control and positive artifacts when reporting these gates.

## Compare rendered references and live sites

`test-reftest` compares WPT test/reference pairs after rendering through the browser pipeline and host framebuffer and text adapters. `tests/unit/refhost/refrender.c` applies CSS, runs layout, and calls the browser painter. `tests/reftest.mk` defines the baseline, comparator controls, diagnostics that remove CSS, and a control that perturbs one pixel.

For a focused investigation, `reftest-diff` accepts a path relative to the corpus through `TEST` and writes test, reference, and amplified difference images. `REFT_MANIFEST` selects the cached candidate list, and `REFT_AHEM` selects the Ahem font. Both are defined in `tests/reftest.mk`.

For live websites, `scoreboard` runs the corpus in `tests/qmp/sites_corpus.tsv` through `tests/qmp/qmp_site.py`. Each site receives a QEMU boot and a local readiness probe. `scoreboard-diff` compares snapshots; `scoreboard-regress` returns a failure for regressions or an inconclusive comparison when controls fail. These commands are defined in `tests/sites.mk`, with aggregation in `tests/qmp/sites_run.py`.

## Find the results

The following are generated outputs. Directory variables identify the locations constructed by the recipes; the cited files give the filenames.

| Test | Generated evidence and location |
|---|---|
| Focused host tests | Assertions on standard output, with retained logs where the recipe redirects output. Layout controls use `CSS_LAYOUT_DIR` in `tests/css_layout.mk`; Fetch body runs use `FETCH_BODY_DIR` in `tests/fetch_body.mk`. |
| WPT | Summary and regression details on standard output. Control baselines, control logs, and ranking files are written directly beneath `BUILD` by `tests/wpt.mk`. |
| Browser wiring | Positive and control JSON reports in the wiring subdirectory beneath `BUILD`, selected by `tests/browser_wiring.mk`. |
| Reftests | Gate output, progress output, and control logs in the reftest subdirectory beneath `BUILD`. `reftest-diff` creates an image subdirectory there. See `tests/reftest.mk`. |
| CSS guest tests | Unique run directories beneath `CSS_GUEST_DIR`, defined in `tests/css_guest.mk`. They retain JSON results, serial logs, and screenshots. The background harness also retains HTTP reports, requests, and packet capture. |
| HTML priority guest tests | Timestamped run directories beneath `HTML_PRIORITY_DIR`, defined in `tests/html_priority.mk`, containing reports, coverage results, navigation evidence, serial logs, screenshots, and packet capture. |
| Cancellation guest tests | Timestamped directories beneath `ABORT_DIR`, defined in `tests/abort.mk`, containing result and request records, serial logs, screenshots, and packet capture. |
| Downloads and keys | A fixed download-keys-guest subdirectory beneath `BUILD`, constructed by `tests/qmp/download_keys_guest.py`. It contains the QEMU command record, result JSON, logs, screenshots, and writable disk copy. |
| Web API, live-page, and video harnesses | Temporary directories with prefixes `qmp_webapi_`, `qmp_live_`, and `qmp_video_`. Their harnesses retain serial logs and captures; the video directory also contains captured audio. |
| Site scoreboard | A directory named by the run label beneath `tests/scoreboard`, plus aggregate JSON and Markdown snapshots alongside it. `tests/qmp/sites_run.py` constructs these locations. |

Record the command, source revision, host environment, image identity, control outcome, and artifact location with a reported result. `TRANSPARENCY.md` ties capability claims to the environment and behavior exercised.

## Limitations

- `test-wpt` runs its two runner controls, but its prerequisites omit `test-wpt-link` and `test-wpt-harness`. Run those checks when changing runner membership or execution.
- The WPT runner exits successfully after reporting an absent corpus. Its host text measurement uses a monospace approximation, and its layout resource adapter returns failure. Check corpus availability and use guest rendering for observations that depend on loaded images or font advances. See `tests/unit/wpt_test.c`.
- The JavaScript semantics probes exercise the engine; browser API availability also depends on page installation. `tests/jssem.mk` documents this distinction.
- The wiring audit covers a reviewed set of source patterns and linked symbols. Runtime and rendering evidence comes from behavioral harnesses. See `tools/browser_wiring_audit.py`.
- Some controls are standalone targets, including `test-webapi-page-control` and `test-stream-control` in `Makefile`. Run them alongside the corresponding positive targets. Sanitizer and leak detection settings vary by recipe.
- Live-site verdicts are measurement categories. A `PAINTED` record captures what the harness observed, and the site driver records browser failures while exiting successfully. Inspect the records and screenshots for visual correctness, and use `scoreboard-regress` for comparison gating. See `tests/qmp/qmp_site.py` and `tests/qmp/sites_run.py`.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Browser source groups, host sweep, application packaging, and core guest targets. |
| `tests/browser_expansion.mk` | Aggregate of browser integration gates. |
| `tests/css_layout.mk` | Focused geometry tests, fault controls, and sanitizer target. |
| `tests/fetch_body.mk` | Tests for request bodies and controls for the failure set. |
| `tests/wpt.mk` | WPT source derivation, link check, filtering, baseline gate, and controls. |
| `tests/unit/wpt_test.c` | Corpus execution, isolation in child processes, self-checks, adapters, and result accounting. |
| `tools/wpt_fetch.sh` | Corpus acquisition and subset selection. |
| `tests/browser_wiring.mk` | Application audit and control prerequisites. |
| `tools/browser_wiring_audit.py` | Source registrations, object membership, include ownership, and ELF symbol checks. |
| `tests/unit/browser_wiring_audit_test.py` | Control that removes a registration, using a source overlay. |
| `tools/mk_wired.py` | Recursive check of test fragment reachability. |
| `tests/tools/make/wired.mk` | Regression prerequisite for the reachability check. |
| `tests/tools/make/wired.py` | Controls for a missing include and fixtures for Make parsing. |
| `tests/css_guest.mk` | CSS guest targets, host prerequisites, and evidence directories. |
| `tests/qmp/qmp_css_components.py` | Assertions about external resources, geometry, and scanout. |
| `tests/qmp/qmp_css_background.py` | Background, gradient, and layer oracle comparisons. |
| `tests/qmp/qmp_ui.py` | QMP input, browser launch, coordinate mapping, and screen capture. |
| `tests/html_priority.mk` | Page and Worker guest gates with packaged controls. |
| `tests/abort.mk` | Cancellation host and guest gates with ordering controls. |
| `tests/qmp/download_keys_guest.py` | Download actions, artifact hashes, saved byte checks, and signature verification. |
| `tests/qmp/qmp_video_page.py` | Checks for presented frames, completion, and captured audio. |
| `tests/reftest.mk` | Commands for reference rendering, controls, baseline, and image outputs. |
| `tests/unit/refhost/refrender.c` | Host rendering through CSS, layout, and paint. |
| `tests/sites.mk` | Targets for measuring and comparing live sites. |
| `tests/qmp/qmp_site.py` | Navigation, diagnostics, screenshots, and verdict records for each site. |
| `tests/qmp/sites_run.py` | Aggregation across repeated runs and snapshot comparison. |
| `TRANSPARENCY.md` | Rules for capability claims and evidence. |

## See also

- [Browser architecture](overview.md)
- [Layout: block, flex, grid and text](layout.md)
- [Painting and hit testing](paint.md)
- [Testing](../architecture/testing.md)
- [Writing tests](../contributing/writing-tests.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
