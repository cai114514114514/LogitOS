# Settings redesign — 2026-09-22

Settings now has eight native pages and 21 editable preferences, including
eleven newly exposed preferences. OpenLogit/GFX renders the cached appearance
previews; AUI supplies the native controls, theme and keyboard navigation.

The additional controls cover Dock magnification, launch bounce, running-app
indicators, the overview hot corner, four clock display options, scroll
direction, title-bar double-click timing, and Browser session restoration.
The System page reads display, memory and network information from the OS.
There are no fabricated hardware controls or placeholder values.

The generated, transparent PNG at `fsroot/icons/settings.png` is packaged as
`/icons/settings.png`. Settings is the first consumer of the generic raster
app-icon resource path. Dock, Finder and Settings use that path; other apps
retain their existing fallback icons. The source image and generation prompt
are preserved, separately from the UI concept image.

## Persistence and lifecycle

Drafts remain local until Apply, which writes only edited fields. Network
addresses and masks are validated before writing. Revert reads the current
live store. The inspector searches both keys and values. Unsaved close and
destructive reset have confirmation dialogs. The final guest check exposed
an Escape dismissal that changed state without repainting the old scrim;
Settings now renders a fresh frame after dismissing a modal.

The preference ABI has no multi-key live transaction or compare-and-swap.
The generation check detects intervening changes, but cannot guarantee an
atomic transaction against simultaneous writers. A failed write/commit is
reported and the draft is retained; earlier writes may already be live in RAM.
Dark mode remains an explicitly labelled immediate system action.

## Reproduction and evidence

Work is in the original checkout. Builds and runtime artifacts use the isolated
`build-settings` directory; no shared disk was repacked for testing.

```sh
make BUILD=build-settings build-settings/logit.iso build-settings/disk.img
make BUILD=build-settings test-settings-draft test-app-icons test-settings-host test-mk-wired
python3 -u tests/qmp/qmp_settings.py --iso build-settings/logit.iso --disk build-settings/disk.img --out build-settings/settings.png --keep
```

Host checks (`build-settings/checks-final.log`):

- Draft validation and edited-field writes: zero failures under ASan/UBSan.
  The mandatory stale-snapshot mutation failed three assertions before the
  unchanged implementation passed.
- Real raster-loader source: zero failures. Removing its actual blit failed
  two assertions, demonstrating that decoding alone cannot pass the gate.
- Existing settings-store gate: 107 checks, zero failures. Its four mandatory
  controls failed 5, 9, 1 and 1 checks respectively.
- Make wiring: 385 fragments, 384 reachable and one declared wrapper exception.

The final QEMU run (`build-settings/ui-final.log`) passed: all eight pages,
ordered/clamped keyboard navigation, invalid-IP rejection, Revert, Apply,
search/clear, menu-clock preferences, and cancellation of an unsaved close.
Window background luminance changed from 240 to 32; the independently drawn
menu bar changed from 196 to 80. Disabling seconds/date/weekday reduced the
clock-region ink count from 383 to 106. The Dock's Settings tile contained
639 blue and 502 metallic-tone pixels, alongside the guest raster-decode log.
Before the modal fix, the check printed `FAIL Escape cancels closing an edited
Settings window`; the same check and the scrim-removal comparison now pass.

Packed `/settings.aex`, `/files.aex` and `/icons/settings.png` were byte-compared
with their local build/source files after the final build. All matched; hashes
are recorded in `build-settings/artifacts.json`. The final Settings executable
is 352336 bytes, SHA-256
`47affdf6c4e8b284dddef30c36e801795252e2a077493ec639bddcfd8c290b4b`.

Runtime screenshots are under `build-settings/settings-*.png`. The original
app was also exercised and captured as `build-settings/before-*.png` before
replacement. The generated `docs/design/settings/concept.png` is a design
reference, not runtime evidence.

Proof boundaries: QEMU and host checks do not establish physical-hardware
behavior. The Dock and Settings raster icon are checked in the guest; Finder
integration is built, with its two rendering paths reviewed. This task does
not claim a guest interaction check of every new Dock/input preference, or a
new reboot-persistence test for each key. Existing store serialization gates
cover the shared persistence mechanism. Network and wallpaper UI explicitly
state that their changes require restart.

One initial QMP run failed to settle the guest pointer during startup. The
desktop and raster decode were visible; a subsequent boot reached every page.
This is recorded as a harness/startup failure, not proof of an application
hang. The search-clear test also needed to wait before capturing: the helper's
`screendump(settle=...)` sleeps after taking the screenshot.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
