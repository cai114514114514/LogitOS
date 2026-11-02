# Preview and Files

Preview displays local images and plays audio and video, with decoding performed in its ring 3 process. Files provides directory browsing, file operations and a project pane connected to the agent service. Files passes document paths to the window manager, which selects the application to open them.

## Application structure

Files is packaged as `files.aex` with the display name Finder. Its implementation uses the application toolkit in `c/apps/gui/aui.c`. Preview links the media libraries into its process and sends drawing and sound requests to kernel services. This keeps continuous media decoding in userland and lets Preview consume complete animations through the shared image interface.

| File | Responsibility |
| --- | --- |
| `c/apps/gui/files/files.c` | Directory snapshots, selection, navigation, file operations, menus and the event loop. |
| `c/apps/gui/files/files_project.inc` | Project queries, task creation and task controls. |
| `c/apps/gui/preview/preview.c` | File loading, format dispatch, decoding, playback and resource cleanup. |
| `c/apps/gui/preview/picker.inc` | Media collection, classification and selection. |
| `c/apps/gui/preview/view.inc` | Window presentation, image placement, zoom, navigation and messages. |
| `c/apps/gui/preview/audio_view.inc` | Audio position, duration and output level meters. |

The Preview link rule in `Makefile` includes image, video, audio and container objects, the Rust library, mini-libc, mathematical routines and OpenLogit drawing support. Its allocation adapters connect allocations from the image library to the application heap.

## Opening files

Files opens directories within its window and passes files to `sys_open_path()`. In `c/kernel/gui/wm.c`, AEX paths launch applications directly. Document routing checks registered extension handlers, then Preview’s media association, then the Terminal fallback.

Preview reads its launch path through `get_arg()`. When launched with no path, it opens the media collection. A collection item returns to the collection through Escape, Backspace or the Library button. Escape and the Close button close a viewer launched with a document path. These transitions are implemented in `c/apps/gui/preview/preview.c` and `c/apps/gui/preview/view.inc`.

The collection scans the guest directory `/media` and one level of subdirectories. It classifies entries by content as images, audio, video, containers or other files, and displays format and size information. `c/apps/gui/preview/picker.inc` supports opening by click or by keyboard selection followed by Enter.

## Preview formats and libraries

`open_path()` reads the complete file and dispatches by content in this order: containers, images, audio, then Annex B video. Images precede audio because JPEG data can resemble MP3 synchronization patterns. ICO and CUR receive an additional header check because their opening bytes resemble an Annex B start code. The dispatch and these checks are in `c/apps/gui/preview/preview.c`.

| Input | Implementation used by Preview |
| --- | --- |
| PNG and APNG | `c/lib/image/img.c` registers the decoder and animation interface implemented in `rust/src/png.rs`. |
| GIF | `c/lib/image/gif.c` decodes still images and animations. |
| JPEG | `c/lib/image/jpeg.c` handles baseline and progressive images. |
| BMP, ICO and CUR | `rust/src/bmp.rs` and `rust/src/ico.rs`. |
| WebP | `rust/src/webp.rs` handles lossless VP8L images and routes lossy images to VP8 key-frame decoding. |
| SVG | `c/lib/image/svg.c`. |
| WAV, FLAC, MP3, ADTS AAC and Ogg Vorbis | Recognition and pull decoding through `c/lib/audio/audio.c`. |
| MP4, including fragmented MP4; Matroska/WebM; AVI; MPEG transport and program streams; FLV | Recognition and parser dispatch through `c/lib/media/demux.c`. |
| H.264 and H.265/HEVC Annex B streams | `c/lib/video/h264.c` and `c/lib/video/h265.c`. |

For a container, Preview selects the first video track and first audio track. Its video branch accepts H.264 and H.265. Its audio branch accepts MP3, FLAC, AAC and signed 16-bit PCM in either byte order. Container parsing and codec selection are successive steps in `play_container()` in `c/apps/gui/preview/preview.c`.

Container audio is assembled into an input accepted by the shared audio interface. Preview prepends FLAC configuration, adds ADTS headers to AAC samples, and wraps PCM in a WAV header. It swaps big endian PCM samples during assembly. File audio and container audio then use the same playback pump and meters.

When a container has playable audio and an unsupported video codec, Preview presents the audio interface with a diagnostic identifying the video codec. A supported video track can play with a diagnostic explaining unavailable audio.

## Images and playback timing

The image interface supplies complete RGBA canvases. GIF and APNG decoders apply blending and disposal before returning frames, so Preview displays each canvas directly. `img_decode_anim()` represents still images as one frame, and the shared layer applies EXIF orientation. These contracts are defined in `c/lib/image/img.h` and implemented in `c/lib/image/img.c`.

Fit preserves aspect ratio within the image stage. The 100% control uses source dimensions; the keyboard shortcuts are `0` for Fit and `1` for 100%. Resize, theme and zoom events request repainting of held images and messages through `c/apps/gui/preview/view.inc`.

Animation playback accumulates frame delays into deadlines measured from the beginning of each loop. This accounts for painting time within the requested interval. Preview logs declared duration, elapsed duration and painting time for each completed loop in `c/apps/gui/preview/preview.c`.

Audio decoding produces interleaved signed 16-bit PCM. Preview checks the capacity of the sound buffer before writing, measures peaks from outgoing samples, and obtains playback position from the sound stream’s counter of played frames. The audio interface displays position, available duration and decaying level meters.

For container video, `c/lib/media/avclock.c` chooses whether to wait, show or drop a picture. Preview feeds played audio position to the clock when a sound stream is open and uses monotonic time otherwise. Its audio pump targets 150 ms ahead of the decoded video position, writing in blocks of up to 1,024 sample frames.

Late pictures still pass through decoding to preserve prediction references. A drop decision skips conversion and painting. Preview converts planar YUV to RGBA, using the HEVC decoder’s display planes at eight bits. Raw Annex B playback loops through the stream at the rate decoding and display can sustain.

## Files browsing and selection

Files starts at the guest directory `/docs`. Its sidebar provides document and font directories, the filesystem root and an Applications view that filters root entries to AEX files. Visibility rules come from `c/apps/hidden.h`; Ctrl+H toggles hidden entries for the session.

Each rendered frame builds a directory snapshot using `dir_count()` and `dir_name()`. Drawing, hit testing and commands consume that snapshot. The event handler matches a click against the listing already displayed, so the click uses the row from the frame the user saw even if the directory changes.

Selection stores entry names. Adding or removing another entry can change row positions while the selected name continues to identify the same directory entry. Plain click selects one item, Ctrl-click toggles membership, and Shift-click selects a range. Navigation clears selection, and double-click opens an entry. These mechanisms live in `c/apps/gui/files/files.c`.

List and icon views share `item_rect()` for drawing and hit testing. Switching views converts the first visible item into the new row layout to preserve the approximate reading position. The event loop refreshes project state during idle periods and waits for events between checks.

Files also tracks changes to the listing and sidebar drawing inputs. It reports their damage rectangle alongside the toolkit’s widget damage, allowing both kinds of drawing to reach the compositor.

## File operations

The context menu and application menu bind commands to their handlers in `c/apps/gui/files/files.c`.

| Operation | Behavior |
| --- | --- |
| New project | Validates the entered name and creates a directory in the current location. |
| Rename | Renames one selected entry and updates selection to its new name. |
| Copy | Stores selected absolute paths in the application clipboard; Paste copies files and directory trees. |
| Cut | Stores selected paths; Paste attempts each move through `sys_rename()`. |
| Delete | Recursively removes selected paths. |
| Get Info | Displays the name, type, and file size or directory entry count. |

File copying transfers data through a 4 KiB buffer. Directory copying creates destination directories and visits their children. Paste checks the source and destination relationship to prevent copying a directory into itself or a descendant.

## Project work

The project pane treats the current directory as the project. `project_refresh()` queries the agent service for its identity and tasks, polling every two seconds until the directory changes. When a project identity is available, the pane can resolve its current path after a move or rename. The application code is in `c/apps/gui/files/files_project.inc`; project lookup in `c/apps/agent/agentd.c` uses stored identity bindings, with a path fallback for tasks lacking those bindings.

Starting work publishes selected paths, or the current directory when selection is empty, then submits the goal with the current directory as the output location. The pane displays task phases, goals and reasons, provides pause, resume and cancellation controls, and opens a reported artifact through `sys_open_path()`.

Ask Logit publishes the same selection context and opens the assistant. Files also handles worker activation: its `main()` reads the activation identity and enters the Finder worker role when requested. The protocol interfaces are declared in `c/lib/agent/sdk.h`.

## Limitations

- Preview’s collection holds 48 entries. Playback retains the complete input file; animated images also retain decoded canvases, and container audio assembly adds another allocation. See `c/apps/gui/preview/picker.inc` and `c/apps/gui/preview/preview.c`.
- Image viewing offers Fit and centered 100% display; the image stays centered in the stage. Audio progress is display-only. Video output is limited to 1280 × 800 pixels, and container video conversion uses a 1 MiB sample buffer.
- Container codec support is the set listed above. AAC container configuration requires object type 2 and a nonzero channel configuration; playback accepts mono and stereo. Ogg Vorbis goes through the file audio path.
- Preview pairs container timestamps with pictures through a queue in decode order. The video decoders support picture reordering, so reordered streams can receive mismatched presentation times. The queue is in `c/apps/gui/preview/preview.c`; decoder output ordering is implemented in `c/lib/video/h264_dpb.c` and `c/lib/video/h265.c`.
- Preview’s image error message still lists progressive JPEG and 16-bit PNG as unsupported examples. That diagnostic is stale: `c/lib/image/jpeg.c` and `rust/src/png.rs` implement those forms.
- The window manager’s media association table differs from Preview’s dispatch. It reads files up to 4 MiB for content inspection and falls back to extensions above that size. Launching an already running Preview focuses its window and ignores the new document argument. Both behaviors are in `c/kernel/gui/wm.c`.
- Files displays up to 64 visible entries per directory and reports the omitted count. Its path buffers hold 128 bytes, new names accept 1–59 bytes, and recursive directory operations have a depth guard of 32. Copy can overwrite files and merge directories; recursive child failures are silent, cut clears its clipboard after attempted moves, and deletion happens immediately and permanently. See `c/apps/gui/files/files.c`.

## Source map

| File | What it contains |
| --- | --- |
| `c/apps/gui/preview/preview.c` | File dispatch, decoding, audio output, video presentation and cleanup. |
| `c/apps/gui/preview/picker.inc` | Media enumeration, classification and collection interaction. |
| `c/apps/gui/preview/view.inc` | Presentation, image sizing, navigation and messages. |
| `c/apps/gui/preview/audio_view.inc` | Audio progress and output level meters. |
| `c/apps/gui/files/files.c` | Directory model, selection, operations, menus and event handling. |
| `c/apps/gui/files/files_project.inc` | Project queries, task creation and task controls. |
| `c/apps/gui/aui.c` | Application toolkit used by Files. |
| `c/apps/hidden.h` | Directory-entry visibility rules. |
| `c/apps/logit.h` | Application wrappers for opening paths, launch arguments and sound services. |
| `c/kernel/gui/wm.c` | Application launching and document associations. |
| `c/lib/image/img.h`, `c/lib/image/img.c` | Image interfaces, registration, animation ownership and orientation handling. |
| `c/lib/image/gif.c`, `c/lib/image/jpeg.c`, `c/lib/image/svg.c` | GIF, JPEG and SVG decoding. |
| `rust/src/png.rs`, `rust/src/bmp.rs`, `rust/src/ico.rs`, `rust/src/webp.rs` | Rust image decoders. |
| `c/lib/audio/audio.c` | Audio recognition and pull decoding. |
| `c/lib/media/demux.c`, `c/lib/media/avclock.c` | Container dispatch, track selection and playback clock policy. |
| `c/lib/video/h264.c`, `c/lib/video/h264_dpb.c`, `c/lib/video/h265.c` | Video decoding and picture output ordering. |
| `c/lib/agent/sdk.h`, `c/apps/agent/agentd.c` | Agent protocol and project lookup. |
| `Makefile` | Application packaging and Preview library linkage. |

## See also

- [Desktop applications](desktop-apps.md)
- [Agent runtime](agent-runtime.md)
- [Image decoding](../libraries/image-decoding.md)
- [Audio codecs and processing](../libraries/audio-codecs.md)
- [Media containers and playback clock](../libraries/media-containers.md)
- [Virtual file system](../filesystems/vfs.md)
