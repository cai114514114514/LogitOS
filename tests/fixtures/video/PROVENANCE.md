# Provenance: tests/fixtures/video

## Classification: C — project-generated

`sample.h264` and `sample.crc32` are produced by this project's
`tools/genvideo.sh` (the H.264 equivalent of `tools/genvideo265.sh`, see
`tests/fixtures/video265/README`'s explicit cross-reference: "Same reasoning
and same arrangement as tests/fixtures/video for H.264."). Encoded from a
synthetic `ffmpeg`/`libx264` source at deterministic settings; `sample.crc32`
is a checksum this project computed and pinned against ffmpeg's own H.264
decode. No third-party content.

## Consuming gates

`tests/h264.mk` (`test-h264`, wired broadly), `$(DISK)`
(`Makefile:1189`, packs `sample.h264` onto the disk at `/media/`). Not
touched by this pass — class C, outside the class-A removal mandate.

## History

Committed alongside the H.264 decoder work; `tools/genvideo.sh` is the
generator of record for the wider (uncommitted) matrix this one fixture is
drawn from.
