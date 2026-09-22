# Source directory reorganization — 2026-09-22

## Delivered structure

229 existing files were relocated in the original working tree. No compatibility
copies, symlink source aliases or alternate implementation tree were introduced.

- 14 diagnostic programs moved into `tests/guest/{audio,media,video,net,sched,exec}`.
- 10 service files moved into `c/apps/services/{http,ssh,syslog}`.
- Clock's four application files joined `c/apps/gui/clock/`.
- Studio's 28 engine/presentation-support files moved into `c/apps/gui/studio/core/`.
- Browser's 173 files moved from the owner's intermediate
  `c/apps/gui/browser/` location into subsystem directories, with redundant
  filename prefixes removed. JS has eleven further subsystem directories.

The [source map](../SOURCE_LAYOUT.md) explains ownership. The
[Browser map](../../c/apps/gui/browser/README.md) lists every old Browser
filename and its new location.

All 229 destinations exist and are unique; all original source paths are absent.
The ten dirty files present at the start retain the owner's modifications:
seven are byte-identical to their initial hashes; the other three become
byte-identical after reversing path-only substitutions in comments.

## Build contracts

Source relocation is independent of guest installation. The command and desktop
application names, disk mappings, link addresses and ordinary `make run` entry
point are retained. `CLI_SOURCE_*` selects service/probe sources without forcing
them back into coreutils. Browser headers use root-qualified subsystem paths;
private Browser directories are excluded from the flat `INCDIRS` namespace.

`BROWSER_FILES` recursively discovers the Browser tree once; JS sources,
headers and implementation fragments derive from it. Editor paths, data
generators, host include flags, source overlays, negative-control object filters
and the application catalog were updated with the moves.

After mapping old paths to new paths, Make's expanded inventories agree:

| Inventory | Before | After |
| --- | ---: | ---: |
| Kernel C sources | 320 | 320 |
| Browser pipeline sources | 40 | 40 |
| Browser JS sources including application entry | 34 | 34 |
| Browser pipeline objects | 92 | 92 |
| CSS objects | 326 | 326 |
| WPT test sources | 118 | 118 |
| WPT JS sources | 33 | 33 |
| CLI names | 45 | 45 |
| Base disk specifications | 8 | 8 |

Sets were compared, not just counts. The desktop app and CLI packaging lists
also agree. The saved comparison is `build-layout-browser/source-contract.json`.

Excluding include directives and comments, 172 of 173 Browser files have
identical source text. The remaining difference updates a source-location path
inside the existing ServiceWorker diagnostic string. No functions were split,
APIs renamed or browser features implemented.

## Validation and test apparatus corrections

The fresh isolated build completed both ISO and ring-3 disk targets:

```sh
make BUILD=build-layout-browser -j6 all build-layout-browser/disk.img
```

**Incremental-build correction, 2026-09-22:** the fresh build above did not test
an existing default `build/`. The owner's subsequent build failed with
`No rule to make target c/apps/clock/main.c, needed by build/apps/clock/main.o`.
The imported old `.d` retained its primary source dependency; `-MP` only gives
removed headers fallback rules. Clock, Studio, relocated CLI programs and the
poll/scheduler/thread probes now use object paths aligned with their moved source
directories. Dropping `.d` files alone could silently reuse older objects, so
the fix leaves all old artifacts in place and builds distinct current objects.

`make -j6 all build/disk.img` then completed against the existing default tree,
with no clean or dependency-file deletion. The disk packer retained 26 user-state
inodes. Its log is `build-relocation/default-build.log`. The permanent
`test-build-relocation` gate seeds old dependency files and newer old objects,
checks real Makefile plans, and requires a failing legacy-object control first.
It passed for 35 moved objects and confirmed retained header dependencies still
trigger rebuilding. The control printed `legacy Clock object rejects removed
main.c`; results are saved in `build-relocation/regression.log`.
The final rebuild also passed (`build-relocation/final-build.log`); artifact
hashes are in `build-relocation/artifacts.json`. With the new regression fragment,
`test-mk-wired` reports 378 fragments, 377 reachable and one declared wrapper.

`test-mk-wired` passes: 377 fragments, 376 reachable and one explicitly declared
wrapper. Its broken-include control was observed failing. `test-wpt-link`
passes with 64 shared translation units and seven named exclusions.

The Clock host checks, HTTP semantics (17 checks), SFTP integration, Studio
renderer (33 checks) and Studio engine (217 checks) pass. Their existing
negative controls remain prerequisites and were observed rejecting mutants.
Browser wiring passes for 14 reviewed features and 58 implementation-fragment
owners; its unchanged-ELF/removed-call control fails at the intended reflow
registration. Frame-base discipline and its positive/negative fixtures pass.
Browser storage persistence, process-restart persistence and their negative
controls also pass.

Two stale test expectations were corrected while validating:

- The wiring audit looked for `&mo_subscription`, although the pre-move source
  already stores the subscription as a pointer. Its pattern now names the
  actual call; the old assumption remains documented beside the correction.
- The media-attribute control expected a print link to win after media guards
  were disabled. DOM order already places the narrow inline style last.
  Recompiling saved pre-move source reproduces the same result: two boxes,
  screen=0, print=0, narrow=2. The control now requires the screen and narrow-style
  failures, while still checking successful box generation and overall failure.
  This combined fixture does not independently isolate the print-link branch.

The broad `test-browser` target reaches a pre-existing Darwin compilation
failure in `http_dechunk_test`: byte-order macros conflict with `net.h`, and
fortified `memcpy` conflicts with the declaration in `http.c`. Compiling with
the exact pre-move `HOST_INCDIRS` reproduces it; all four relevant source files
match their original SHA-256. This unrelated network code was not changed.
Evidence is in `build-layout-browser/http-baseline/`. Before that blocker,
loader, DOM/API, variable substitution, CSS variables (54 checks) and parser
fuzz checks pass; their completion does not mean the entire aggregate passed.

The additional `test-layout-box` run reports 51 checks and two failures. A
separate compile of saved pre-move Browser and test sources reproduces exactly
the same values: 12 elements, eight `IT_RECT` entries and ten box records. Its
two old assertions assume only background-painted elements emit `IT_RECT`,
although the existing implementation also emits transparent hit regions. This
unrelated expectation drift was recorded rather than changing layout behavior;
see `build-layout-browser/layout-baseline/`.

`test-reflect-table` explicitly skips because the optional WPT reflection
corpus is absent. Generated source is retained; no regeneration claim is made.

## Guest evidence

The same full-system harness was run against the image built before Browser's
internal migration and against the reorganized build. Both pass 27 assertions:
desktop startup, all ten installed GUI applications, shell pipelines, local
HTTP bytes, Browser CSS/text/PNG pixels, link navigation and subsequent
TextEdit input/save. A deliberately empty Dock click launches nothing.

Browser's fixture produces the same measured text ink count (4,012 pixels),
colour-block bounds and PNG bounds before and after. This is a QEMU fixture
regression check, not a claim about all websites or real hardware.

Logs and screenshots are under `build-layout/guest-baseline/` and
`build-layout-browser/guest-final/` (the final rebuilt image also passes all
27 assertions). Build outputs and scratch evidence
remain ignored by Git. Historical reports and scoreboard outputs retain their
original paths and hashes; the migration maps resolve their old names.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
