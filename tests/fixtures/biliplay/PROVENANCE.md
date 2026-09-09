# Provenance: tests/fixtures/biliplay

## Classification: B — third-party media content, captured for interoperability testing

The bytes here are from bilibili video BV1GJ411x7h7 ("【官方 MV】Never Gonna
Give You Up - Rick Astley", cid 137649199), the owner's designated decoding
specimen, fetched host-side once on 2026-08-30 via the html5-fallback playurl
API (`platform=html5&high_quality=1`), which serves a progressive MP4. The
capture is the harness downloading public files, not the browser identifying
itself: the browser's UA is unchanged and honest everywhere else.

## What each file is

- `progressive.mp4` — the first 24 s of that download. Video track
  STREAM-COPIED (bit-exact bilibili H.264 High 640x360 25 fps, level 3.0);
  audio RE-ENCODED to AAC-LC because the source track is HE-AAC
  (`mp4a.40.5`), which this browser refuses BY DESIGN (js_media.h: "decoding
  the core alone is the right samples at half the rate"). Keeping the HE-AAC
  bytes would make the audio half of every gate a documented refusal, which
  is the live site's story to tell, not the fixture's.
- `init-video.mp4`, `video-N.m4s` — the same video, DASH-shaped: ffmpeg's
  dash muxer, 4 s segments, stream-copied video. The shape bilibili's real
  player feeds MediaSource (init segment + numbered .m4s), built from the
  real encode so the timescales, SPS/PPS and slice shapes are the CDN's own.
- `init-audio.mp4`, `audio-N.m4s` — AAC-LC (re-encoded as above), same shape.
  `audio-7.m4s` (191 B) is the muxer's final partial segment, kept on
  purpose: a near-empty trailing segment is a real DASH occurrence and a
  different code path than a full one.
- `type-video.txt` / `type-audio.txt` — the MIME strings the offline gate
  asks `addSourceBuffer` for, i.e. the codec question the site's manifest
  family asks: `avc1.64001E` (High@3.0 -- what this encode actually is) and
  `mp4a.40.2`.

## Why the real DASH segments are NOT here

The DASH manifest (`fnval=16`) is only served by the WBI-signed endpoint,
which answered the capture session with HTTP 412 (bilibili risk control)
regardless of UA or signature -- measured 2026-08-30, three attempts with
valid `w_rid` signatures and warm `buvid3`/`buvid4` cookies. The unsigned
endpoint serves the html5 fallback only. So the DASH-shaped files here are
locally re-muxed from the specimen's own video bytes; when the WBI endpoint
someday answers, they should be replaced with the CDN's own segments.

## Consuming gates

tests/biliplay.mk (`test-biliplay-offline`, `test-biliplay-live`), driven by
tests/qmp/qmp_biliplay_page.py.
