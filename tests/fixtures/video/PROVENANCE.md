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

## black.mp4 + captions.vtt + captions-black.vtt (2026-08-30, video wave-1)

Generated for the <video src> + <track> gates (tests/videosrc.mk):
  ffmpeg -f lavfi -i "color=c=black:s=64x48:r=15" -t 4 -c:v libx264 ...
Solid black, 60 frames, no audio track, keyint 15 so seeks land exactly.
The video is black ON PURPOSE: the guest gate counts bright pixels in the
caption band, and on black content every bright pixel is the caption's. A
textured video would make that check measure the video.

captions.vtt's boundaries (0.0/0.5/1.0/1.4) sit INSIDE h264-mp3.mp4's 2 s, so
the host gate can step the clock across every edge; captions-black.vtt's
windows are wider (0.9/0.8/0.6 s) because the guest gate screenshots over QEMU
timing, not a stepped clock.
