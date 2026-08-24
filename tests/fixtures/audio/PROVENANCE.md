# Provenance: tests/fixtures/audio

## Classification: C — project-generated

`sample.aac`, `sample.flac`, `sample.mp3`, `sample.ogg`, `sample.wav`
carry no third-party content. All five were added in the same commit as
`tests/unit/audio_gen.sh` (commit `815b1d8bf`, "audio: WAV, FLAC and MP3
decoders -- the one rescued line with a real result", 2026-08-07) and
`43b2373bd`/`9015061e3` for AAC/Vorbis the following day — the generator
script's own header states the method and the reason nothing here is
third-party:

> "tests/unit/audio_gen.sh -- build the audio test corpus with ffmpeg.
> Everything here is generated, never committed [in that script's own
> working copy]: the point of a differential test is that the reference and
> the input come from a tool that has no knowledge of our decoder ...
> A stamp keeps re-runs cheap; delete the directory to force regeneration."

The five files under `tests/fixtures/audio/` are a small, fixed, COMMITTED
subset of that generated corpus (kept so `test-audio-codec-fuzz` and the
on-device `/media/sample.*` fixtures — see `Makefile:1189-1193` — mean
something on a machine with no `ffmpeg` installed, the same reasoning
`tests/fixtures/video/README`-equivalent gives for `sample.h264`). Sources
are synthetic (`ffmpeg -f lavfi -i sine=...`/tone generators), not captured
from any third party.

## Consuming gates

`test-audio-codec-fuzz`, `test-audio-codec-fuzz-deep`,
`test-audio-codec-fuzz-negctl` (`tests/audio_codec.mk`) read this directory
directly; `Makefile:1189-1193` packs `sample.mp3`/`sample.flac`/`sample.wav`
onto the LogitFS disk image at `/media/`. Not touched by this pass — class C,
outside the class-A removal mandate.

## History

First added: commit `815b1d8bf`, 2026-08-07 (WAV/FLAC/MP3); AAC and Vorbis
samples added `43b2373bd`/`9015061e3`, 2026-08-08.
