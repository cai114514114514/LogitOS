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

## Consuming gates

`tests/preview.mk` (`PREVIEW_AS`), `tests/qmp/qmp_preview.py`. Not touched by
this pass — class C, outside the class-A removal mandate.

## History

Added alongside the Preview/Files-association work in `tests/preview.mk`.
