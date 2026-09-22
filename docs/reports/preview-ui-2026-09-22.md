# Preview UI — 2026-09-22

The old fixed dark list is now a native media library with five categories,
format and size labels, selection, scrolling and paging. File classification
uses headers and the player's codec libraries, not extensions. The viewer has
a toolbar, a neutral image stage, a readable footer, Fit / 100% controls, and
a visible Library action. Audio gets a separate playback card and real PCM
meters; long decoder messages wrap within a readable panel.

Rendering uses the existing OpenLogit transport, including its rounded shapes.
The private views are split into `view.inc`, `picker.inc` and `audio_view.inc`.
All normal and measurement/control entrypoints depend on these includes.
The pre-existing mono/stereo buffer-safety fix in `preview.c` was preserved.

## Build and guest evidence

The isolated `build-preview/logit.iso` and `build-preview/disk.img` build
successfully. The original application was captured before the change at
`build-preview/before/00-list.png`. New library, image, audio and refusal
screenshots are under `build-preview/screenshots` and `build-preview/navigation`.

`build-preview/ui.log` records a passing 10-format guest run: PNG, BMP, WebP,
ICO, EXIF JPEG, animated GIF, WAV, AAC in MP4, H.264/MP3 in MP4, and a named
VP9/Opus refusal. Still images matched 36/36 sampled reference pixels; where
appropriate the independent PIL reference matched as well. The GIF showed all
four composited frames and measured 1491 ms for its declared 1490 ms loop.
The MP4 displayed 30 frames with zero dropped and mean A/V drift of -10 ms.
Category filtering, 100% pixels and the Fit keyboard shortcut also passed.

`build-preview/navigation.log` passes Escape from a still, a playing fragmented
MP4 and a missing-init media segment. Each return is checked against a fresh
picker listing, and the still return also against its visible window anchor.
The missing-init refusal is checked by its actual decoder message.

The missing-delay control was built and watched fail in the guest:

- GIF: declared 1490 ms, measured 67 ms.
- APNG: declared 600 ms, measured 66 ms.

`build-preview/control.log` records acceptance of those failures. The positive
pixel/timing make targets now require this control. Its old shell wrapper
accepted any nonzero exit; the replacement requires an actual early-loop
measurement, so a failed mouse launch cannot count as successful sabotage.
The private disk recipe reader also replaces the old text substitution and
keeps production packaging authoritative.

`make BUILD=build-preview test-mk-wired` passes: 386 fragments, 385 reachable,
one declared wrapper exception. Python syntax and scoped `git diff --check`
also pass.

After the final metadata-label and completed-meter refinements,
`build-preview/final-ui.log` passes category filtering, desktop theme changes,
Fit / 100%, the Library toolbar button, reopening an image, APNG and WAV.
The APNG measured 607 ms against a declared 600 ms. Final screenshots are in
`build-preview/final-screenshots`.

The packed `/preview.aex` was compared byte-for-byte against the local build:
2768048 bytes, SHA-256
`e8f0967f4d7acef169b6e8635fd1313f336801d7ea8e6808ccb39410ac3bf558`.
Source/artifact hashes are saved in `build-preview/artifacts.json`.

## Boundaries

This is a UI refactor, not new codec support. The existing 48-entry `/media`
collection limit remains. Actual-size images are center-cropped if larger than
the stage; there is no pan tool. Playback progress is display-only, without
seeking. Resize handling is implemented, but the recorded interaction runs
use the default 760 x 560 window at 100% display scale. No physical-hardware
claim is made.

One QMP startup attempt failed to settle the pointer before launching Preview.
Its log is retained as `build-preview/ui-startup-failure.log`; the retry passed.
That failure was not treated as evidence of an application failure or accepted
as a successful negative control.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
