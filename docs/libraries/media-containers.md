# Media containers and playback clock

The media library converts container bytes into track metadata, codec configuration and timestamped samples. Its common interface covers MP4/MOV, Matroska/WebM, AVI, MPEG transport streams, MPEG program streams and FLV. Preview and the browser use these samples with the video and audio decoders, sound streams and a shared playback clock. The public interfaces are in `c/lib/media/media.h`; WebVTT and SRT parsing is defined in `c/lib/media/subs.h`.

## Layer and ownership

Container parsing runs in application address spaces. The `Makefile` filters `c/lib/media/` out of the kernel sources. `tests/demux.mk` compiles the library with userland flags and connects it to Preview; `tests/mse.mk` adds the same objects to the browser. This keeps container parsing and its sample-index allocations in the consuming process.

Opening a container builds a sample index for each track. Each entry stores a payload offset, size, decode timestamp, presentation timestamp and keyframe flag. Format parsers populate this shared representation, and `c/lib/media/demux.c` implements reading, seeking and video framing over it.

The public lifetime contract requires the caller’s input buffer to remain valid and unmodified until the demuxer closes. Payload storage depends on the format:

| Formats | Storage used by samples | Cleanup through the generic API |
|---|---|---|
| MP4, Matroska, AVI, FLV | Payloads and codec configuration refer into the caller’s buffer. | `media_close()` frees the indexes and demuxer. |
| MPEG TS and PS | Parsers copy and accumulate elementary-stream payloads into an allocated buffer. | `media_open()` sets `owns_data` after successful parsing; `media_close()` also frees that buffer. |

For TS and PS, `media_sample.file_off` is an offset into the accumulated payload buffer. For the other formats, it is relative to the container’s parsing origin. These ownership rules follow `c/lib/media/media_int.h`, `c/lib/media/demux.c`, `c/lib/media/ts.c` and `c/lib/media/ps.c`.

The direct entry points remain paired APIs: `ts_open()` with `ts_close()`, and `ps_open()` with `ps_close()`. The generic pair, `media_open()` and `media_close()`, handles all six formats.

## Common demux interface

`media_sniff()` identifies containers by their contents. `media_open()` probes the buffer’s beginning, then searches within its initial 4096 bytes when the first probe returns an unknown format. Finding a container behind a prefix moves the parsing origin before indexing begins, so embedded container offsets retain their meaning.

The public interface operates on an opaque `mdemux`:

| Operation | Interface and behavior |
|---|---|
| Open and close | `media_open()` returns a demuxer, or a null pointer with an optional error code. `media_close()` releases its storage. |
| Inspect tracks | `media_track_count()` and `media_track_info()` expose metadata. `media_find_track()` locates the first track of a requested type. |
| Read sequentially | `media_read()` chooses among track cursors by lowest decode timestamp, with ties resolved by payload offset. It returns 1 for a sample, 0 at the end, or a negative error. |
| Select a track | `media_select()` restricts sequential reading to one track. A negative selection enables all tracks. |
| Read by index | `media_get_sample()` returns an indexed sample while preserving sequential cursor positions. |
| Seek | `media_seek()` scans keyframes through the requested presentation time, defaults to the first sample, and resets track cursors around the chosen decode time. It returns the chosen presentation timestamp. |
| Inspect duration | `media_duration_ns()` takes the greatest available movie or track duration, using a track’s final decode timestamp when its duration is unavailable. |

A `media_track` contains the codec identifier, video geometry or audio format, timestamp timescale, sample count and codec configuration. A `media_sample` provides a payload view, timestamps in track ticks and nanoseconds, and a keyframe flag.

The shared `br` helpers in `c/lib/media/media_int.h` check read lengths and create bounded windows for nested structures. Reader failure remains set across later operations. `media_get_sample()` checks indexed payload ranges again before returning a pointer. Allocation limits include `MEDIA_MAX_TRACKS` at 16 and `MEDIA_MAX_SAMPLES` at four million entries per track.

## Container parsers

The opening comment in `c/lib/media/media.h` lists “Two containers.” As checked on 2026-11-02, its enumeration and the dispatch in `c/lib/media/demux.c` cover these six:

| Container | Parsing and index construction |
|---|---|
| MP4/MOV | `c/lib/media/mp4.c` walks boxes. Progressive files combine sample sizes, chunk mappings, chunk offsets, duration runs, composition offsets and sync-sample tables. Fragmented files use fragment headers and sample runs, with defaults from initialization metadata. |
| Matroska/WebM | `c/lib/media/mkv.c` reads EBML elements, gathers track descriptions, then indexes clusters. It handles SimpleBlock, BlockGroup and Xiph, fixed and EBML lacing. |
| AVI | `c/lib/media/avi.c` reads RIFF stream headers and format descriptions. It prefers the legacy index, then OpenDML indexes, then a scan of movie chunks. |
| MPEG transport stream | `c/lib/media/ts.c` recognizes 188-byte TS packets and 192-byte M2TS packets. It follows the Program Association Table to the first program’s Program Map Table, creates tracks by packet identifier and accumulates PES payloads between payload-start indicators. |
| MPEG program stream | `c/lib/media/ps.c` walks pack headers, program stream maps and PES packets. Pack versions select MPEG-1 or MPEG-2 PES header parsing. Video packets without timestamps extend an active video sample. |
| FLV | `c/lib/media/flv.c` indexes AVC video and AAC audio tags. Sequence headers supply codec configuration. The parser checks previous-tag-size fields against the preceding tag’s size. |

Fragmented MP4 obtains track descriptions and defaults from initialization metadata. A bare fragment containing a movie fragment box without a movie box returns `MEDIA_ERR_NO_INIT`, identifying the missing initialization segment.

AVI advances each stream’s timeline using its scale and rate fields. With a zero fixed sample size, empty chunks advance time while producing no indexed payload. Fixed-size audio advances according to the chunk’s byte count divided by its sample size. The legacy index path also checks that an indexed offset points to the claimed chunk identifier.

PES is the packet layer shared by TS and PS. `c/lib/media/pes.c` extracts timestamps, parses both optional-header forms and maps stream types to codec identifiers. TS takes video keyframe flags from the transport adaptation field’s random-access indicator. PS scans accumulated H.264/H.265 payloads for IDR pictures. Its private-stream path initially classifies the stream as other data with an unknown codec.

## Timestamps and video framing

A **decode timestamp** places compressed input on the decoder’s timeline. A **presentation timestamp** places the resulting output on the playback timeline. MP4 and FLV composition offsets preserve this distinction. Matroska stores each indexed block timestamp as both values; AVI derives equal values from stream timing.

Each track declares ticks per second. `md_ticks_to_ns()` in `c/lib/media/demux.c` converts ticks by splitting them into whole seconds and a remainder before multiplication. It checks large whole-second values before producing nanoseconds.

Timing adjustments occur during indexing:

| Format | Timestamp treatment |
|---|---|
| Progressive MP4 | Applies the first media edit and preceding empty edits; composition offsets adjust presentation time. |
| Matroska | Combines cluster time with a signed block offset, subtracts codec delay and spaces laced frames using the declared default duration. |
| TS and PS | Retains PES timestamps in a 90 kHz timescale. |
| FLV | Uses millisecond decode timestamps and adds the signed video composition offset for presentation time. |

H.264 and H.265 samples can carry length-prefixed Network Abstraction Layer units, with parameter sets stored in codec configuration. The common interface prepares the Annex B streams consumed by the video decoders:

1. `media_annexb_headers()` emits stored parameter sets with start codes.
2. `media_to_annexb()` replaces sample length prefixes with start codes.

Both functions accept a null output pointer to query the required size. For tracks with raw framing, `media_to_annexb()` copies the sample bytes. Configuration detection and conversion are implemented in `c/lib/media/demux.c`.

## Playback clock

`avclock` maps presentation timestamps onto playback progress. The caller supplies monotonic time to `avclock_frame()` and reports audio progress through `avclock_audio()`. Supplying time as an argument lets `tests/unit/avsync_test.c` exercise the policy with simulated playback.

The header describes the audio master as the card’s play cursor. The current implementation reports source frames consumed by the mixer: `snd_stream_state()` exports `frames_consumed` as `frames_played`, and `mix_one()` advances that counter while consuming source frames into a device period. Players convert this counter to media time using the source sample rate. This distinction follows `c/kernel/audio/mixer.c`.

With audio progress available, the clock uses that position plus a timeline offset. Otherwise, it advances from the first picture’s presentation timestamp using monotonic time. The first picture establishes the anchor and receives a show decision.

`c/lib/media/avclock.c` applies these defaults:

| Condition | Decision |
|---|---|
| Picture is early | Return `AV_WAIT` and a suggested wait. Retries of the same presentation timestamp share a 250 ms budget. |
| Early-picture wait budget expires | Rebase the clock and return `AV_SHOW`. |
| Picture is over one second late | Rebase the clock and return `AV_SHOW`. |
| Picture is over 50 ms late | Return `AV_DROP`, up to four consecutive drops. |
| Remaining cases | Return `AV_SHOW`. |

The drop limit preserves periodic presentation when sustained lateness would otherwise suppress every picture. Rebasing adjusts the relationship between the master and the media timeline. Counters record show and drop decisions, rebases and drift; positive drift means video is ahead of the master.

Both players decode pictures before deciding whether to present them, preserving decoder reference state. Audio feeding proceeds in blocks toward video progress plus a 150 ms lead target. This couples sound-stream progress to the rate at which video advances.

`avclock_audio_end()` transfers the video tail to monotonic time, anchored at the last reported audio position plus the clock correction. The browser calls it once the source has ended, the audio sample cursor has reached the end and pending sound writes have completed.

## Preview

Preview reads a file into memory and dispatches by content: container, image, audio, then Annex B video. Container playback opens the generic demuxer and selects the first video and audio tracks in `c/apps/gui/preview/preview.c`.

Its container video path selects H.264 or H.265, feeds stored parameter sets and retrieves samples through `media_get_sample()`. Each sample passes through `media_to_annexb()` before decoding. Preview queues input presentation timestamps in a FIFO and associates them with emitted pictures. A show decision from the clock leads to color conversion and drawing.

Container audio is assembled into input for the common audio decoder interface. Preview concatenates MP3 payloads, copies FLAC configuration before payloads, reconstructs ADTS headers for AAC and adds a WAV header for PCM. It swaps big-endian PCM bytes during assembly. The audio selector covers MP3, FLAC, AAC and 16-bit PCM.

Sound feeding follows picture progress, and `snd_state()` supplies audio-master updates. The status display reports shown pictures, dropped pictures and drift; the completion log also includes rebases and aggregate drift.

## Browser

The browser divides media integration between two files:

| File | Responsibility |
|---|---|
| `c/apps/gui/browser/js/media/media.c` | JavaScript objects, events, resource fetching, timers, operating-system callbacks and subtitle drawing. |
| `c/apps/gui/browser/js/media/media_src.c` | Buffered bytes, demuxers, decoder state, sample cursors and playback clock. |

For a resource URL assigned to a media element, the JavaScript binding fetches the response body as an array buffer and passes it to `mel_load_bytes()`. The engine creates an owned media source and a progressive buffer, then opens the complete bytes through the generic demuxer. Content sniffing selects the container.

Media Source Extensions accumulate appended bytes in a growing buffer. A box walk identifies the longest prefix ending at a complete top-level MP4 box. Opening that prefix produces the same demuxer representation used for complete files. Segment mode reparses on demand; sequence mode reparses during append to establish timestamp placement. Buffer reallocation closes the previous demuxer because its borrowed pointers refer into the earlier allocation.

The engine applies timestamp-offset checkpoints above the demuxer and maintains audio and video sample cursors. Its video selector covers H.264 and H.265; its audio selector covers AAC and MP3. H.264 timestamps enter through `h264_decode_pts()` and return with the corresponding output picture, including after picture reordering. The HEVC path associates timestamps through a FIFO.

The browser converts and stores each emitted picture before asking the clock for a decision. An early picture remains held for a later pump iteration. The timer chain requests work at 16 ms intervals, while the clock determines presentation timing.

Sound streams use nonblocking writes. The engine retains partially written audio blocks and finishes them before decoding another block. Muting writes silence, keeping sound-stream progress available to the playback clock.

## Subtitle files

`c/lib/media/subs.c` parses WebVTT and SRT into a common `subs_track`. Cue identifiers and text use owned allocations, allowing the caller to release the input after parsing.

Cues are sorted by start time, preserving file order for ties. `subs_active_at()` finds cues satisfying the interval `start_ms <= time < end_ms`, using binary searches followed by a candidate scan to handle overlaps.

The browser fetches subtitle resources, attaches them through `subs_parse()` and queries active cues using the element’s media position. Its drawing integration redraws subtitle text after video presentation. These connections are in `c/apps/gui/browser/js/media/media.c` and `c/apps/gui/browser/js/media/media_src.c`.

## Validation hooks

The repository defines these targets for the container and playback paths:

| Target | Checks described by its sources |
|---|---|
| `test-demux` | Parser units, Matroska lacing, MP4/Matroska differential checks, clock simulation, fuzzing and negative controls in `tests/demux.mk`. |
| `test-containers` | AVI/TS/PS/FLV units, AVI/FLV differential checks, generic-dispatch comparisons, fuzzing and controls in `tests/containers.mk`. |
| `test-avsync` | Simulated decoding speeds, audio lead, presentation under sustained lateness and the wait budget for a stalled audio master in `tests/unit/avsync_test.c`. |
| `test-mse` | Browser media-engine assertions for segmented and complete input, with controls defined in `tests/mse.mk`. |
| `test-containers-diff-report` | Reports comparisons across all four additional container fixtures, including recorded TS/PS sample-grouping differences. |

Review basis: source inspection on 2026-11-02. Builds and tests were not run for this review.

## Limitations

- The demuxer indexes resident input. Complete browser resources arrive before playback, and each browser media buffer has a 48 MiB limit. Reopening growing prefixes can accumulate quadratic parsing work. See `c/apps/gui/browser/js/media/media_src.c`.
- MP4 uses the first sample description and the initial media edit with preceding empty edits. AVI reads the first RIFF body and omits later AVIX continuations. See `c/lib/media/mp4.c` and `c/lib/media/avi.c`.
- TS handles PAT/PMT sections from individual packets and selects the first program. TS/PS timestamps retain their 33-bit values across wraparound. See `c/lib/media/ts.c` and `c/lib/media/pes.c`.
- TS/PS sample grouping can combine several encoded audio frames. PS’s continuation heuristic can also merge complete pictures from packets without timestamps. These differences are recorded in `tests/containers.mk`.
- Both players’ container AAC paths require configuration bytes; TS/PS derive AAC rate and channels from ADTS without constructing that configuration. Preview’s AAC assembly accepts AAC-LC. See `c/lib/media/ts.c`, `c/lib/media/ps.c`, `c/apps/gui/preview/preview.c` and `c/apps/gui/browser/js/media/media_src.c`.
- Preview’s timestamp FIFO and the browser’s HEVC FIFO assume matching decode and display order. Reordered output can receive the wrong timestamp through these paths.
- Audio feeding can produce gaps when decoding falls behind. The kernel’s exported audio position measures mixer consumption, with device buffering between that point and audible output. See `c/lib/media/avclock.c` and `c/kernel/audio/mixer.c`.
- Subtitle parsing preserves raw text markup and positioning settings. The browser draws up to four active cues as text, with up to three displayed lines at the default bottom-center position. See `c/lib/media/subs.h` and the browser media files.

## Source map

| File | What it contains |
|---|---|
| `c/lib/media/media.h` | Public container, track, sample, framing and clock interfaces. |
| `c/lib/media/media_int.h` | Reader helpers, sample indexes and demuxer ownership state. |
| `c/lib/media/demux.c` | Sniffing, dispatch, reading, seeking, timestamp conversion and Annex B adaptation. |
| `c/lib/media/mp4.c` | Progressive and fragmented MP4 parsing. |
| `c/lib/media/mkv.c` | Matroska/WebM tracks, clusters, lacing and timing. |
| `c/lib/media/avi.c` | RIFF streams, indexes, chunk scanning and timing. |
| `c/lib/media/ts.h`, `c/lib/media/ts.c` | Transport packets, program tables, PES accumulation and direct-entry lifetime rules. |
| `c/lib/media/ps.h`, `c/lib/media/ps.c` | Program stream packs, stream maps, PES accumulation and direct-entry lifetime rules. |
| `c/lib/media/pes.h`, `c/lib/media/pes.c` | Shared PES headers, timestamps, stream classification and IDR scans. |
| `c/lib/media/flv.c` | AVC/AAC tags, configuration and composition timestamps. |
| `c/lib/media/avclock.c` | Presentation decisions, drift accounting and audio-tail transition. |
| `c/lib/media/subs.h`, `c/lib/media/subs.c` | Subtitle model, ownership, parsing and active-cue lookup. |
| `c/lib/video/h264.h` | Picture-associated timestamp contract. |
| `c/apps/gui/preview/preview.c` | File dispatch, container playback, timestamp FIFO and audio assembly. |
| `c/apps/gui/browser/js/media/media_src.c` | Buffered media engine, decoders and clock integration. |
| `c/apps/gui/browser/js/media/media.c` | JavaScript integration, fetching, timers, sound callbacks and subtitle drawing. |
| `c/kernel/audio/mixer.c` | Source-frame consumption and exported playback counters. |
| `Makefile`, `tests/demux.mk`, `tests/mse.mk` | Application linkage and media test targets. |
| `tests/containers.mk` | Additional container checks and recorded grouping differences. |
| `tests/unit/avsync_test.c`, `tests/unit/mse_test.c` | Clock simulations and browser media-engine assertions. |

## See also

- [Libraries](overview.md)
- [H.264 video decoding](video-h264.md)
- [Audio codecs and processing](audio-codecs.md)
- [Storage, crypto, text and media APIs](../browser/storage-crypto-media.md)
- [Preview and Files](../userland/preview-and-files.md)