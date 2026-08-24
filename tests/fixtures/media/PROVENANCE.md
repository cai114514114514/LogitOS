# Provenance: tests/fixtures/media

## Classification: C — project-generated

Every container file (`h264-mp3.mp4`, `h264-mp3-nobf.mp4`, `h264-mp3.mkv`,
`h264-flac.mkv`, `h265.mp4`, `frag.mp4`, `frag-everyframe.mp4`, `mp3.mka`,
`pcm.mov`, `vp9-opus.webm`, `laced-*.mkv`, `aac.m4a`) is produced by
`tests/unit/gen_media.sh`, whose header states:

> "build the container fixture matrix with ffmpeg. The files this writes are
> COMMITTED under tests/fixtures/media, for the same reason
> tests/fixtures/video/sample.h264 is: the gate has to mean something on a
> machine with no encoder installed ... Deterministic, generated sources: no
> input file, same bytes on any machine with the same ffmpeg. testsrc2 moves
> (so P frames are not all skip)."

All video/audio content is synthetic (`ffmpeg -f lavfi -i testsrc2=...`/
`sine=...`), not captured or derived from any third party. `laced.expect`,
`expected-guest.txt` are project-computed expectation files (checksums and
per-track metadata this project's own tools derived from the generated
containers), not external data.

## Consuming gates

`tests/demux.mk`, `tests/preview.mk` (`PREVIEW_FX`, packs several of these
onto the disk at `/media/`), `tests/h265.mk` (`h265.mp4`). Not touched by
this pass — class C, outside the class-A removal mandate.

## History

Generated/committed alongside the demuxer work; `tests/unit/gen_media.sh` is
the generator of record (delete the directory and re-run to regenerate,
"expect every pinned checksum in the suite to move" per its own header).
