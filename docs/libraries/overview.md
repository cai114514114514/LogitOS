# Libraries

LogitOS shares image decoding, media handling, rendering, text processing, inference, and agent support through `c/lib/` and the Rust crate in `rust/`. Kernel code and applications link these components with the runtime that supplies their allocation and system interfaces. This page identifies each library’s purpose, main entry points, consumers, and ownership contracts.

## Organization and linkage

`Makefile` selects graphics, font and shaping code, image codecs, the pinyin engine, WebAssembly, and `c/lib/string.c` for kernel compilation. Applications compile shared sources with their user runtime. Audio, video, container parsing, inference, agent support, CSS support, and software 3D have application build rules.

The media build comments explain this placement through allocation and execution costs. Video retains reference pictures, audio retains decoder state, and demuxers allocate sample indexes. Application processes provide the allocation interfaces and execution context for that continuous work. See `Makefile`.

Libraries reach consumers through object groups and archives. `Makefile` packages line breaking and paragraph layout into a text archive and compiles the CSS support libraries with LibCSS. `tests/openlogit_sdk.mk` assembles the graphics SDK archive, and `tests/agent.mk` builds the agent SDK archive. `rust/Cargo.toml` defines a static library whose exported functions provide C interfaces.

The application C library lives in `c/apps/libc/`; see [The C library](../userland/libc.md). Source selection and linkage are covered in [Build system](../architecture/build-system.md).

## Images and the Rust crate

`c/lib/image/` provides format registration, decoding dispatch, image ownership, JPEG, GIF, SVG, and EXIF orientation processing. Its public interface is `c/lib/image/img.h`.

| Entry point | Purpose |
|---|---|
| `img_init()` | Registers the linked format handlers. |
| `img_decode()` | Detects the format and returns straight RGBA pixels with eight bits per channel. |
| `img_decode_anim()` | Returns complete frame canvases, delays, and loop information. |
| `img_free()` | Releases a decoded image. |
| `img_anim_free()` | Releases animation frames and their pixels. |

Animation frames contain the results of blending and disposal, so consumers display complete canvases. Still formats also work through the animation interface, returning one frame. `c/lib/image/img.c` applies EXIF orientation after decoding, including frames returned through the animation interface.

The `logit_rust` crate uses `#![no_std]` and supplies PNG and APNG decoding, BMP, ICO/CUR, WebP, DEFLATE/zlib decompression, and PNG encoding. Its module selection is in `rust/src/lib.rs`; the image codecs register with the C dispatcher.

`rust/src/imgbuf.rs` defines structures matching the C image types and wraps temporary allocations in objects that free their buffers on drop. Publishing a result transfers ownership to the caller. Allocation passes through `kmalloc()` and `kfree()`. The browser and Preview map those functions to application allocation in `c/apps/gui/browser/app/runtime.c` and `c/apps/gui/preview/preview.c`.

Two interfaces serve consumers beyond image display:

- `inflate_raw()` and `zlib_decompress()` are declared in `c/lib/image/inflate.h` and implemented in `rust/src/inflate.rs`. The HTTP client calls them in `c/net/http/http1.c`.
- `png_encode_rgba()` and `png_encode_free()` are declared in `c/lib/image/img.h` and implemented in `rust/src/pngenc.rs`. Browser canvas serialization uses them in `c/apps/gui/browser/js/graphics/canvas.c`.

Image consumers include Preview, browser layout and canvas, kernel wallpaper, and application icons. Kernel callers use `kernel_img_decode()` in `c/kernel/exec/syscall.c`, which serializes decoding because JPEG uses shared scratch storage. Its callers include `c/kernel/gui/wm_wallpaper.c` and `c/kernel/gui/app_icons.c`. Application decoding appears in `c/apps/gui/preview/preview.c` and `c/apps/gui/browser/layout/block.c`.

## Audio

`c/lib/audio/` provides audio decoding, packet handling, and supporting mathematics. The common interface in `c/lib/audio/audio.h` detects WAV, FLAC, MP3, AAC in ADTS, and Vorbis in Ogg. Its common output consists of interleaved signed samples with 16 bits per sample.

`audio_decode()` returns a complete `apcm` buffer, released with `audio_pcm_free()`. The pull interface uses `adec_open()`, `adec_read()`, and `adec_close()`. Callers retain the input bytes for the decoder’s lifetime and provide the output buffer for each read. Playback can request the number of sample frames it can queue while the decoder retains its codec state.

Codec headers provide entry points for payloads extracted from containers. Browser media opens AAC with container configuration through `aac_open_asc()`. Opus exposes packet parsing and decoder operations through `opus_packet_parse()`, `opus_decoder_create()`, `opus_decode()`, and `opus_decoder_destroy()` in `c/lib/audio/opus.h`.

Preview and Terminal consume the common pull interface in `c/apps/gui/preview/preview.c` and `c/apps/gui/terminal/terminal.c`. Browser media calls codec interfaces in `c/apps/gui/browser/js/media/media_src.c`. The Opus vector harness is `tests/unit/opus_vec.c`.

## Video

`c/lib/video/` contains decoder contexts, picture reconstruction, transforms, prediction, and filtering. Stateful decoders retain reference pictures and other stream state across calls.

| Component | Main entry points | Consumers |
|---|---|---|
| H.264 | `h264_open()`, `h264_decode()`, `h264_decode_pts()`, `h264_flush()`, `h264_close()` | Preview, Terminal, and browser media. |
| H.265 | `h265_open()`, `h265_decode()`, `h265_flush()`, `h265_close()` | Preview, Terminal, and browser media. |
| MPEG-1/2 | `mpeg12_open()`, `mpeg12_decode_pts()`, `mpeg12_flush()`, `mpeg12_close()` | `tests/unit/mpeg12_test.c`. |
| Motion JPEG | `mjpeg_open()`, `mjpeg_next_frame()`, `mjpeg_decode_frame()`, `mjpeg_close()` | `tests/unit/mjpeg_test.c`. |
| Cinepak, MS Video 1, RPZA, QuickTime Animation | `legacy_cinepak_decode()`, `legacy_msvideo1_decode()`, `legacy_rpza_decode()`, `legacy_qtrle_decode()` | `tests/unit/legacy_test.c`. |
| MPEG-4 Part 2/H.263 components | Header processing, `m4_idct_put()`, `m4_idct_add()`, and `m4_mc()` | Component harnesses listed in `tests/mpeg4.mk`. |

The H.264, H.265, and MPEG-1/2 interfaces return decoded image planes and provide flushing operations for delayed pictures. Their contracts are in `c/lib/video/h264.h`, `c/lib/video/h265.h`, and `c/lib/video/mpeg12.h`.

Motion JPEG locates frame boundaries and supplies default Huffman tables for frames using that convention. It then calls `img_decode()` for JPEG reconstruction, sharing the still image decoder. See `c/lib/video/mjpeg.c`.

Application playback integrations are in `c/apps/gui/preview/preview.c`, `c/apps/gui/terminal/terminal.c`, and `c/apps/gui/browser/js/media/media_src.c`.

## Containers, subtitles, and timing

`c/lib/media/` identifies containers from their contents and exposes tracks and timestamped samples. The dispatch in `c/lib/media/demux.c` handles MP4, Matroska/WebM, AVI, MPEG transport streams, MPEG program streams, and FLV.

The interface in `c/lib/media/media.h` uses `media_open()`, `media_track_info()`, `media_read()`, `media_seek()`, and `media_close()`. Reads choose the next sample across the selected tracks by decode timestamp. `media_annexb_headers()` emits stored H.264/H.265 parameter sets, and `media_to_annexb()` converts sample framing into Annex B byte streams for the decoders.

The caller retains the source buffer until closing the demuxer. MP4, Matroska, AVI, and FLV samples refer into that buffer. Transport and program stream parsing reassembles payloads into storage owned by the demuxer; `media_close()` releases that storage. This distinction is implemented in `c/lib/media/demux.c`.

Presentation timing uses `avclock_init()`, `avclock_audio()`, and `avclock_frame()` in `c/lib/media/avclock.c`. The clock relates picture timestamps to reported audio playback progress or monotonic time supplied by the caller, then returns a decision to show, wait, or drop presentation. Players continue decoding pictures whose presentation is dropped so later pictures retain their references.

Subtitle parsing uses `subs_parse()`, `subs_active_at()`, and `subs_close()` from `c/lib/media/subs.h`. WebVTT and SRT become tracks of timed cues. The parser copies cue data into track storage, allowing the caller to release the source bytes after parsing.

Preview and browser media consume the demuxer and presentation clock. Browser subtitle integration is in `c/apps/gui/browser/js/media/media_src.c`. The guest demuxer harness is `tests/guest/media/demuxcheck.c`.

## Graphics, text, and input

These libraries supply drawing, text, and composition services across the desktop and applications.

| Library | Purpose and main interface | Consumers |
|---|---|---|
| `c/lib/gfx/` | Shared 2D rendering and OpenLogit interfaces. Start with `c/lib/gfx/include/gfx.h` and `c/lib/gfx/include/openlogit.h`; `ol_device_create()` initializes a device in storage supplied by the caller. | Kernel desktop rendering, the GUI toolkit, browser painting, and SDK examples. |
| `c/lib/gfx3d/` | Software 3D rendering with shader, scene, and material support. Main rendering calls include `ol3d_create()`, `ol3d_draw_indexed()`, and `ol3d_end()` in `c/lib/gfx3d/include/openlogit_3d.h`. | Examples including `examples/openlogit/island.c` and `examples/openlogit/studio/scene.c`. |
| `c/lib/text/` | Font reading, Unicode processing, shaping, glyph rasterization, and paragraph layout. Main interfaces include `shape_line()`, `shape_run()`, and `tl_para_layout()`. | Kernel text drawing, browser canvas text, and GUI paragraph layout. |
| `c/lib/ime/` | Pinyin composition, dictionary lookup, candidate ranking, and selection through `ime_open()`, `ime_reset()`, `ime_feed()`, `ime_candidates()`, and `ime_accept()`. | Kernel IME integration in `c/kernel/gui/ime/ime_ui.c`. |

The text system shares shaping between measurement and drawing. Kernel text and browser canvas call `shape_line()`, while GUI paragraph layout supplies a measurement callback to `tl_para_layout()`. The contracts are in `c/lib/text/shape.h` and `c/lib/text/para.h`.

The pinyin engine reads dictionary bytes in place and keeps composition state in a `struct ime_state` owned by the caller. Fixed storage and integer operations support its use in the window manager. The engine lives in `c/lib/ime/pinyin.c`; input event handling and candidate presentation live in `c/kernel/gui/ime/ime_ui.c`.

See [Graphics and windowing](../architecture/graphics-and-windowing.md) and [Text and input](../architecture/text-and-input.md) for their system roles.

## CSS support

`c/lib/logitwapcaplet/` implements the interned string interface declared in `c/lib/logitwapcaplet/include/libwapcaplet/libwapcaplet.h`. `lwc_intern_string()` gives equal byte strings a shared object; `lwc_string_ref()` and `lwc_string_unref()` manage references.

The index stores pointers to allocated string objects, so resizing preserves addresses retained by DOM and CSS objects. `c/lib/logitwapcaplet/pool.c` manages storage and references, while `c/lib/logitwapcaplet/string.c` supplies equality and ASCII case folding. Browser DOM code in `c/apps/gui/browser/dom/tree.c` and LibCSS consume the interface.

`c/lib/logitparserutils/` supplies buffers, vectors, stacks, Unicode helpers, charset codecs, and incremental input streams. The CSS input path uses `parserutils_inputstream_create()`, `parserutils_inputstream_append()`, `parserutils_inputstream_peek()`, and `parserutils_inputstream_advance()`.

The stream keeps pending source bytes and decoded UTF-8 storage. Replaced decoded windows remain allocated until their unread characters have been consumed, preserving borrowed pointers across refill and growth. This lifetime handling is implemented in `c/lib/logitparserutils/stream.c` and `c/lib/logitparserutils/window.c`.

LibCSS creates and appends to these streams in `third_party/css/libcss/src/parse/parse.c`; its lexer reads them in `third_party/css/libcss/src/lex/lex.c`. `Makefile` compiles both support libraries with the CSS sources.

## Neural network inference

`c/lib/nn/` provides tensor storage, arithmetic for floating point and quantized values, model loading, and transformer inference.

| Interface | Purpose and main entry points |
|---|---|
| `c/lib/nn/nn.h` | Tensor allocation through `nn_new()` and `nn_free()`, matrix operations, normalization, rotary position operations, and activations. |
| `c/lib/nn/quant4.h` | Quantization and arithmetic for packed weights using four bits per value, with scale and minimum metadata. |
| `c/lib/nn/model.h` | LOGITLM model loading through `lm_open()` and cleanup through `lm_close()`. |
| `c/lib/nn/infer.h` | State allocation through `lm_state_new()` and `lm_state_free()`, and token evaluation through `lm_forward()`. |
| `c/lib/nn/kvcache.h` | Cache allocation through `kv_cache_new()` and memory calculations through `lm_budget_compute()`. |

`lm_open()` validates the model header and payload length, allocates layer descriptors, and builds tensor views into the supplied blob. The caller retains that blob for the model’s lifetime. See `c/lib/nn/model.c`.

`lm_state_new()` computes the working storage layout before allocating its arena. The same layout code counts the bytes and assigns buffer addresses, keeping the allocation size aligned with the buffers used by `lm_forward()`. The forward pass produces token logits using that state. See `c/lib/nn/infer.c`.

The application consumer, `c/apps/lm/lm.c`, loads a model, creates inference state, and calls `lm_forward()` during prompt processing and generation.

## Agent support

`c/lib/agent/` provides messaging, application context publication, task state, review proposals, file bindings, persistent memory, and model requests.

| Interface | Responsibility |
|---|---|
| `c/lib/agent/sdk.h` | Connections and messages through `ag_connect()` and `ag_call()`, context publication, and worker activation. |
| `c/lib/agent/task.h` | Task phases, call budgets, object rights, document revisions, and operation receipts. |
| `c/lib/agent/review.h` | Candidate proposal storage and validation. |
| `c/lib/agent/files.h` | File identities and task bindings. |
| `c/lib/agent/memory.h` | Application memory records with revision checks. |
| `c/lib/agent/model.h` | Model configuration and the `ag_model_start()`, `ag_model_pump()`, and `ag_model_close()` request lifecycle. |

`ag_task_commit()` consults receipts when an operation ID is retried and checks document revisions before applying a new operation. Receipts record the resulting revision, payload length, and checksum. `c/lib/agent/store.c` writes checksummed task and document records into alternating slots and selects a valid generation during recovery. Keeping receipts and document contents in the same record ties an operation’s recorded completion to its effect.

Service communication uses a Unix socket. `ag_connect()` compares the peer identity supplied by the kernel with the installed service’s application ID and image hash. Model requests use HTTP through the socket API and advance through polling in `c/lib/agent/model.c`.

Consumers include `c/apps/agent/agentd.c`, Assistant, Files, TextEdit, and `c/apps/coreutils/agentctl.c`. Their service workflow is covered in [Agent runtime](../userland/agent-runtime.md).

## WebAssembly

`c/lib/wasm/` provides a WebAssembly binary decoder, validator, and interpreter. `wasm_load()` decodes and validates a module; `wasm_instantiate()` resolves supplied imports and creates instance state; `wasm_invoke()` executes a function.

The embedding supplies a `struct wasm_arena` for metadata and execution storage. Module structures retain references to the input bytes, so the embedding also controls their lifetime. The contracts are in `c/lib/wasm/wasm.h` and `c/lib/wasm/wasm_exec.h`.

The browser adapter in `c/apps/gui/browser/js/wasm/wasm.c` includes the library implementation sources. It converts JavaScript values and imports, manages memory views, and translates library errors and traps into JavaScript outcomes.

## Memory and string primitives

`c/lib/string.c` supplies `memset()`, `memcpy()`, `memmove()`, `memcmp()`, and `strcmp()` for kernel links. Freestanding compiler output can call memory primitives when implementing structure and array operations, making these definitions part of the kernel runtime. Applications obtain their implementations from `c/apps/libc/`.

## Limitations

- Kernel image links use the empty SVG registration hook in `c/lib/image/img.c`. SVG decoding requires an application link containing `c/lib/image/svg.c`.
- Opus decoding implements CELT and rejects SILK and hybrid modes. Its packet interface sits outside the common audio dispatcher, whose formats are WAV, FLAC, MP3, AAC, and Vorbis. See `c/lib/audio/opus.h` and `c/lib/audio/audio.c`.
- The public decoder operations declared in `c/lib/video/mpeg4.h` have no implementations in this tree. Available components cover headers, transforms, and motion compensation; `tests/mpeg4.mk` records the missing orchestration and macroblock layer.
- `c/lib/video/h265.h` records reconstruction differences for B pictures and scaling lists with nonuniform entries.
- VP8 interframe decoding requires the `vp8-interframe` Cargo feature. The default feature set in `rust/Cargo.toml` is empty.
- WebAssembly support targets MVP features. The interpreter’s default linear memory cap is 512 pages, or 32 MiB, in `c/lib/wasm/wasm_exec.c`.

## See also

- [Repository map](../getting-started/repository-map.md)
- [Build system](../architecture/build-system.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Text and input](../architecture/text-and-input.md)
- [The C library](../userland/libc.md)
- [Agent runtime](../userland/agent-runtime.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Kernel source selection, application object groups, CSS integration, text archive construction, and Rust linkage. |
| `tests/openlogit_sdk.mk`, `tests/agent.mk` | Graphics and agent archive construction and consumer linkage. |
| `c/lib/image/img.h`, `c/lib/image/img.c` | Image registration, decoding, orientation handling, and output ownership. |
| `c/lib/image/jpeg.c`, `c/lib/image/gif.c`, `c/lib/image/svg.c`, `c/lib/image/exif.c` | C image codecs and orientation processing. |
| `rust/Cargo.toml`, `rust/src/lib.rs` | Rust crate configuration, features, and module selection. |
| `rust/src/imgbuf.rs` | C image structures and allocation ownership. |
| `rust/src/png.rs`, `rust/src/bmp.rs`, `rust/src/ico.rs`, `rust/src/webp.rs` | Rust image decoders and registration functions. |
| `rust/src/pngenc.rs`, `rust/src/inflate.rs` | PNG encoding and DEFLATE/zlib decompression. |
| `c/lib/audio/audio.h`, `c/lib/audio/audio.c`, `c/lib/audio/opus.h` | Common audio interfaces, format dispatch, and Opus packet decoding. |
| `c/lib/video/h264.h`, `c/lib/video/h265.h`, `c/lib/video/mpeg12.h` | Stateful video decoder contracts. |
| `c/lib/video/mjpeg.c`, `c/lib/video/legacy.h` | Motion JPEG integration and legacy codec interfaces. |
| `c/lib/video/mpeg4.h`, `c/lib/video/mpeg4_int.h`, `tests/mpeg4.mk` | MPEG-4 interfaces, available components, and implementation gaps. |
| `c/lib/media/media.h`, `c/lib/media/demux.c` | Container dispatch, tracks, samples, timestamps, and ownership. |
| `c/lib/media/avclock.c`, `c/lib/media/subs.h`, `c/lib/media/subs.c` | Presentation timing and subtitle parsing. |
| `c/lib/gfx/include/gfx.h`, `c/lib/gfx/include/openlogit.h`, `c/lib/gfx3d/include/openlogit_3d.h` | Graphics geometry, device, and software 3D interfaces. |
| `c/lib/text/shape.h`, `c/lib/text/para.h` | Shaping and paragraph layout contracts. |
| `c/lib/ime/pinyin.h`, `c/lib/ime/pinyin.c`, `c/kernel/gui/ime/ime_ui.c` | Pinyin composition engine and kernel input integration. |
| `c/lib/logitwapcaplet/pool.c`, `c/lib/logitwapcaplet/string.c` | Interned string storage, references, equality, and case folding. |
| `c/lib/logitparserutils/stream.c`, `c/lib/logitparserutils/window.c` | Incremental decoding and borrowed input window lifetimes. |
| `c/lib/nn/nn.h`, `c/lib/nn/model.c`, `c/lib/nn/infer.c` | Tensor interfaces, model loading, and inference state. |
| `c/lib/nn/quant4.h`, `c/lib/nn/kvcache.h` | Packed weight arithmetic, cache storage, and memory calculations. |
| `c/lib/agent/sdk.c`, `c/lib/agent/task.c`, `c/lib/agent/store.c`, `c/lib/agent/model.c` | Agent transport, task updates, recovery records, and model requests. |
| `c/lib/wasm/wasm.h`, `c/lib/wasm/wasm_parse.c`, `c/lib/wasm/wasm_exec.h`, `c/lib/wasm/wasm_exec.c` | WebAssembly loading, embedding, execution, and resource bounds. |
| `c/lib/string.c` | Kernel memory and string primitives. |