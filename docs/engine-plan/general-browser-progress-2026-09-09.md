# General browser compatibility — integration record, 2026-09-09

This wave treats frameworks and sites as specimens, never production branches.
The requested scope is HTML elements/attributes, DOM and Web APIs, component
lifecycle and interaction, CSS layout/paint, and scheduling. iframe is one
example of that scope, not its definition.

## What the integration found

The earlier inventory's five zero-consumer gaps were not merely missing APIs.
Connecting them revealed defects in the application caller: an inner scroll
position could not move pixels; specified height behaved like min-height;
Popover invalidation compared equal CSS and skipped layout; native button
clicks never invoked the script-only command default; animation completion jobs
waited forever without another timer. These are general defects and their
negative controls restore the old behavior at those seams.

The component extension adds an auto-height/max-height clamp with final overflow
clipping, parser-frame adoption before bootstrap, uniform refusal of unsupported
sandbox forms, and MessagePort send-time cloning/queued-delivery ownership.
The large remaining architecture gaps are listed in the linked audits; a
script-visible contentWindow is not evidence of an independently painted,
interactive child browsing context.

## Framework evidence must distinguish mount, input and CSS

The retained seven-app corpus covers Angular, Next, React, Svelte, Vite, Vue and
Webpack. It packages real emitted JS and lazy chunks, but deliberately drops
CSS/images/fonts. Therefore its runtime and counter results do not certify a
framework's full visual compatibility.

The guest driver now retains serial/JSON/screenshots and clicks the real
counter using QMP. Initial measurement: all seven mounted and lazy content
loaded; six counters changed 0 to 1, while one stayed 0. That exposed a general
DOM wrapper/expando lifetime defect, being investigated independently of bundle
names. The negative control stops the trusted click before the component's
handler: React still mounts, but the click gate reports 0/1 and exits 1. This
makes prerendered HTML or an observer's own event insufficient for a pass.

## Challenge UI and server acceptance

An interactive verification widget needs working script, child-context,
communication, layout and input paths. Fixing those paths does not guarantee a
third party accepts a custom engine. Cloudflare documents limited support for
custom/heavily modified and embedded engines, and directs automated integration
tests to test keys; no identity spoof or challenge bypass was implemented.
[Official supported-browser documentation](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/)

## Audits and remaining scope

- [CSS production-chain audit](css-compat-audit-2026-09-09.md): auto max-height,
  percentage heights, column-flex shrink, grid rem units and content alignment.
- [Runtime/component audit](framework-runtime-audit-2026-09-09.md): parser frame
  startup, real child Window/paint/input, shadow flat-tree consumption,
  MessagePort cloning and custom-element reaction/scan paths.
- [WAAPI scope and proof](wiring-waapi-2026-09-09.md): accepted properties,
  explicit refusals, ownership and completion checkpoint.

## Verified integration artifact

Correction beside the initial 6/7 result above: after connecting the main DOM
wrapper graph to QuickJS GC, **all seven specimens mount, load their lazy
content, and increment their counters through native QMP mouse input**. The
Svelte bundle was not changed. The ordinary regression uses Symbol handlers,
class-instance expandos and composedPath without naming any framework.

`make -j6 test-browser-expansion` and a forced browser ELF relink followed by
`make -j6 build/disk.img` exit 0. The aggregate includes the observed-failing
component controls; `test-mk-wired` passes for 201 fragments (200 reachable,
one declared standalone wrapper). The final
wrapper-specific gate/ASan has 24 checks: 300 detached parent/child cycles give
6 live wrappers before, 606 before GC and 6 after GC. Its missing-gc-mark
control leaves 609 alive and fails. MessagePort and WAAPI ASan pass 21 and 39
checks respectively. These host results do not certify guest memory behavior.

The actual 1 GiB guest browser passes seven component pages in one navigation
sequence: element scroll (5 checks plus native hit/wheel), Popover (8 checks
plus native open/outside-close/reopen/Escape), WAAPI (4 checks plus native
cancel), text formatting, max-height plus wheel, parser/dynamic child script
execution exactly twice with four sandbox refusals, and MessagePort (10
checks). Inspected screenshots show the scrolled content clipped in its panel,
the Popover above the document, the animation endpoint and restored start
position after cancel, and transformed/spaced/indented text.

Commands, exit codes, serial logs, screenshots, JSON and SHA256 hashes are in
[the retained evidence directory](evidence/general-browser-2026-09-09/commands.json).
The framework corpus omits CSS/images/fonts; 7/7 counter interaction remains
a runtime/input result, not full framework rendering conformance.

An earlier intermediate guest launch crashed before navigation; its raw log is
retained at `build/wiring-next/guest-waapi-final/serial.txt`. The clean linked
artifact passed both complete guest sequences above. The earlier address alone
did not establish a cause, so this report does not label that crash fixed.

## Shared-workspace handoff

The user confirmed another team is actively changing `browser.c`. Source hashes
drifted after the tested artifact was captured, while the ELF/AEX/disk/ISO hashes
were unchanged at the post-verification check. The evidence applies to those
recorded artifacts, not every later concurrent edit. We preserve the other
team's work. The late-callback queue/navigation findings are handed off as a
separate reviewable patch instead of editing their active file.

The [late-callback handoff](patches/late-callback-consumers.md) contains that
patch and two ordinary HTML fixtures. A copy of the real `app_main` is run
with an observable idle boundary: direct scroll navigation, inserted script,
and inserted-script navigation all fail in the original at the first
`wait_idle(0)` after the listener, then pass in the patched copy. A fourth
first-load case passes in both and is recorded only as a positive control.
The patch is **not applied to production and has no guest verification**;
it must be reconciled with the other team's active browser loop changes.

**Later correction (same date):** the previous sentence is the status of that
artifact, not the current source. The patch is now reconciled into production,
has a permanent prerequisite negative control, and its three late-scroll modes
ran in the guest without further input after Shift-wheel. See the correction
in the linked handoff and the new site-generalization report. Other-team edits
were retained; the earlier artifact hashes above are not reused for this build.

The DOM owner's follow-up identifies its hash drift as a comment-only change:
the captured `js_dom.c` hash is
`141269f1dcb363d4ef6d3da3a935c851a05237233e68fb40dbb03225927f44dd`,
and removing the final GC-measurement comment from the later file reproduces
that hash. The 21-to-24-check expansion changes tests, not production behavior.
The independent `browser.c` edits are still outside this artifact's evidence.

Remaining large areas include real child browsing-context paint/input and
Window identity, shadow flat-tree layout plus style scoping/slots, custom-element
connection reactions, and the CSS issues in the audit. The iframe startup fix
does not complete those larger contracts. See the
[frame/component audit](frame-widget-audit-2026-09-09.md).

No extra build tree or disk-image variant was created in this wave. Redundant
PPM screenshots were losslessly verified against retained PNGs before deletion,
removing 156,672,816 bytes of duplicate captures. Host test counts and source
audits remain separate from guest/site results.
