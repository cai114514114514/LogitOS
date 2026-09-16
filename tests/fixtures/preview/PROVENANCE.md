# Provenance: tests/fixtures/preview

## Classification: C — project-authored

All seven `.as` scripts (`open-{audio,flac,image,mkv,mp4,wav,webm}.as`) are
short, hand-written AetherScript test drivers, each issuing two syscalls
(`SYS_GUI_CREATE`, `SYS_OPEN_PATH`) against a fixed `/media/sample.*` path —
see the shared header comment in each file: "The association, exercised the
way the Finder exercises it ... tests/qmp/qmp_preview.py --assoc runs
these." No third-party content; the `/media/sample.*` paths they open point
at the class-C fixtures in `tests/fixtures/media` and `tests/fixtures/audio`
(see those directories' own `PROVENANCE.md`), not at anything captured here.

The A3 migration keeps the same seven media paths and moves the two checked
ABI calls into the project-authored `association.as` helper. Each entry point
is compiled into a native AEX; the helper checks both failure results instead
of allowing a refused association to exit successfully. No media was added or
replaced by that language migration.

## Consuming gates

`tests/preview.mk` (`PREVIEW_AS`), `tests/qmp/qmp_preview.py`,
`tests/unit/as_preview_launcher_test.py`, and `tests/boot/run-as-preview.py`.
The earlier provenance pass did not edit these class-C files; the later A3
migration changes their language/runtime and tests, not their classification.

## History

Added alongside the Preview/Files-association work in `tests/preview.mk`.
