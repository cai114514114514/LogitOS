# Studio

Code Studio is the desktop editor for UTF-8 files and project directories. It combines document tabs, recovery drafts, AetherScript checks and completion, and captured program output. Its application entry point is `c/apps/gui/studio/studio.c`. The repository also contains OpenLogit Scene Studio, an interactive graphics example under `examples/openlogit/studio/` that adjusts a generated scene.

## What Studio edits

Code Studio presents a project tree beside document tabs, a source editor and a problems or output panel. Editing includes selection, clipboard operations, undo and redo, search and replacement, indentation, delimiter pairing and syntax highlighting. Language assistance uses AetherScript facilities, while document storage accepts UTF-8 text regardless of filename extension.

The project tree enumerates the project root and expanded directories. Expansion state follows directory paths, keeping folders associated with their children across sorting and refreshes. Creation uses the selected directory, the selected file’s parent, or the project root. Deletion passes through a confirmation dialog. The engine operations are in `c/apps/gui/studio/core/project.c`; the chooser and dialogs are in `c/apps/gui/studio/core/studio_project.inc`.

Scene Studio provides Scene, Motion and Material inspector pages. Users select a sphere, torus or skinned tube, adjust lighting and pose blending, toggle shadows, texture and toon shading, move the camera and seek through animation. Its controls are implemented in `examples/openlogit/studio/ui.c`.

## Editor model

The application engine owns documents, project state, recovery scheduling, compiler jobs and language results. The presentation adapter owns window events, layout, text measurements, pointer placement and clipboard integration. This division lets editing commands and history serve multiple input paths through one engine interface.

| Type | Responsibility |
|---|---|
| `StEngine` | Owns engine state, host callbacks, work deadlines and the completion runner. |
| `StState` | Provides the frontend’s view of documents, project entries, output, problems and completion candidates. |
| `StDocument` | Holds current text, the saved baseline, selection, viewport, revisions and undo records. |
| `StHost` | Supplies the clock, settings callbacks and compiler or module locations. |
| `StRunner` | Holds child-process state, pipe endpoints, captured sources and collected output. |

These contracts are defined in `c/apps/gui/studio/core/engine.h`, `c/apps/gui/studio/core/document.h` and `c/apps/gui/studio/core/runner.h`. Private engine state lives in `c/apps/gui/studio/core/internal.h`.

The frontend reads a borrowed view through `st_engine_state()`. Engine commands can replace text buffers, so the renderer obtains current pointers after commands. The interface requires calls to run on the owning event loop because the language service shares scratch storage.

A document keeps its editable bytes and a baseline representing the saved contents and recorded file existence. Comparing these determines dirty state, including when undo returns to the saved text. Selection positions are byte offsets checked at UTF-8 boundaries. Each edit advances a revision used to validate asynchronous results.

Undo records retain removed bytes, inserted bytes and the previous selection. `c/apps/gui/studio/core/document.c` allocates history storage and reserves text capacity before changing contents. This ordering preserves usable text, selection and history when allocation fails.

## Files, recovery and sessions

Code Studio reads regular files and saves the current document bytes to their existing path. Persistence and compiler communication use the following representations:

| Representation | Contents and purpose |
|---|---|
| UTF-8 text file | The document’s editable bytes. |
| Binary recovery draft | A versioned `StDraft` header, baseline bytes and edited bytes. |
| Settings entries | Project path, tab paths and active tab. |
| Compiler snapshot | Length-framed paths and source bytes for open documents. |
| JSON response | Compiler diagnostics or completion candidates with source metadata. |

`c/apps/gui/studio/core/storage.c` stores recovery drafts in a `.studio` directory beside each document. A path hash names two alternating slots. Each draft records the full path, generation, selection, viewport, baseline, edited contents, file-existence state and checksum. Recovery validates both slots and selects the valid draft with the greatest generation. Full paths identify ownership, allowing the storage code to detect a collision with another valid draft.

The saved baseline makes conflict detection possible after recovery. Before saving, the storage layer reads the destination and compares its existence and contents with that baseline. A matching destination proceeds through a recovery checkpoint, a complete write, synchronization and baseline replacement. A conflict returns an error, and the edited buffer keeps its contents.

A clean checkpoint acts as a selection bookmark: recovery restores its selection when the current file matches its bytes. A dirty draft restores edited text and its baseline, including when the source file has disappeared. Confirmed deletion removes matching recovery slots before removing the source, preventing those drafts from reopening the deleted document.

`c/apps/gui/studio/core/session.c` stores the project, tab paths and active tab through `StHost`. Startup accepts a launch path or restores saved settings, pruning unavailable tabs. Window closure checkpoints documents before destroying the engine. Failure to checkpoint a dirty document keeps the window open.

The disk packaging rule in `Makefile` preserves the guest document directory `/docs` with `--preserve-merge /docs`. Existing user contents take precedence over packaged defaults, including recovery drafts, while newly packaged files can be added.

## Checks, completion and execution

An edit schedules recovery after a 500 ms pause and checking after an 850 ms pause. Further edits move those deadlines. The frontend drains queued input before servicing scheduled work, allowing a queued keystroke to postpone a check. These paths are in `c/apps/gui/studio/core/engine.c` and `c/apps/gui/studio/studio.c`.

Checks capture owned copies of all open documents. `c/apps/gui/studio/core/source_snapshot.c` serializes them using the framing defined in `c/apps/as/include/snapshot.h`: a protocol marker and source count, followed by lengths and raw path and source bytes. Sending this snapshot through a pipe lets the compiler inspect unsaved buffers, and each file keeps its saved state.

The configured compiler defaults to the guest path `/bin/as`. The runner starts it in the project directory. Check requests ask for JSON diagnostics and provide the snapshot; completion requests also provide the caret position. Run first saves dirty tabs, then invokes the compiler with the active file’s path. Job preparation is in `c/apps/gui/studio/core/jobs.c`, and child execution is in `c/apps/gui/studio/core/runner.c`.

Standard output and standard error share a capture pipe. The parent uses nonblocking I/O and bounds the work performed during each poll, keeping output collection compatible with the GUI event loop. Child completion follows its wait status. Stop sends a termination signal to the runner’s child.

Diagnostics are validated before attachment to the editor. The engine checks the captured document revision and bytes, reported source lengths and checksums, and UTF-8 boundaries around diagnostic ranges. Selecting a problem checks the source again before moving the selection, including when the problem belongs to an imported module. These checks prevent an earlier result from navigating into changed text. The implementation is in `c/apps/gui/studio/core/diagnostics.c` and `c/apps/gui/studio/core/language.c`.

Completion combines the existing editor provider with compiler queries for member expressions. A dedicated completion runner lets those queries progress while the main runner handles a check or program. Request generations, source checks and caret checks reject stale responses. The frontend schedules automatic completion after a 180 ms typing pause; Ctrl+Space requests it immediately. The integration is in `c/apps/gui/studio/core/language.c`, `c/apps/gui/studio/core/completion_job.c` and `c/apps/gui/studio/core/studio_editor.inc`.

## Drawing with OpenLogit

Code Studio uses the AUI widget layer in `c/apps/gui/aui.c`, which uses shared OpenLogit graphics facilities and GUI services. The editor also draws text, selections and other regions through GUI primitives.

The presentation code measures bounded UTF-8 runs and caches their widths. Painting, selection geometry and pointer placement use those measurements, accounting for differing glyph advances. Editor updates can reuse rows that match the previous frame. Full interface frames call `aui_end_rect()` with the canvas extent because direct editor drawing contributes pixels outside AUI’s widget tracking. These mechanisms are in `c/apps/gui/studio/core/studio_render.inc`, `c/apps/gui/studio/core/studio_code.inc` and `c/apps/gui/studio/core/studio_view.inc`.

Scene Studio combines OpenLogit 2D surfaces and controls with the software 3D renderer. Its `struct studio`, defined in `examples/openlogit/studio/studio.h`, owns graphics resources, generated meshes, camera state, animation and inspector values. The scene includes the three selectable objects and a ground plane; the tube uses a pose blend with two joints. Shader source is embedded in `examples/openlogit/studio/shaders.h`.

The 3D renderer produces a scene surface that the 2D interface composites into the viewport. Scene changes trigger rendering; interface animation can reuse the cached scene image. The event loop waits when idle and suspends drawing while the window is hidden. The reduced-motion setting pauses scene playback and changes interface animation sampling. These paths are implemented in `examples/openlogit/studio/main.c`, `examples/openlogit/studio/scene.c` and `examples/openlogit/studio/ui.c`.

## Code organisation

`Makefile` compiles the Code Studio engine’s C sources into an archive and links it with the presentation adapter, AUI, graphics code and the editor completion provider. Presentation fragments are included by `c/apps/gui/studio/studio.c`.

Scene Studio divides window lifetime and events, scene resources and rendering, and interface controls among three C files. `tools/mkopenlogit_sdk.py` installs its executable at the guest path `/bin/scene-studio` and copies example sources beneath `/usr/share/openlogit`.

## Limitations

- Documents must contain valid UTF-8 without embedded NUL bytes. Each document is limited to 1 MiB; the engine holds eight tabs and 192 project-tree entries. Document paths must fit in 128-byte buffers. See `c/apps/gui/studio/core/document.h`, `c/apps/gui/studio/core/engine.h` and `c/apps/gui/studio/core/runner.h`.
- Undo retains up to 128 edits with a rolling 512 KiB byte budget; a single large edit can exceed that budget. Each runner captures up to 128 KiB of output. The state holds 32 problems and 64 completion candidates. These bounds are defined in the same headers.
- Saving checks external contents before opening the destination for truncation. Another writer can change the file between that check and the write, and saving uses no atomic replacement. Folder deletion requires an empty directory. See `c/apps/gui/studio/core/storage.c` and `c/apps/gui/studio/core/project.c`.
- Run provides captured output without interactive terminal input: the parent closes the child’s input pipe after launch. See `c/apps/gui/studio/core/runner.c`.
- Scene Studio edits generated scene parameters in memory. Scene import, export and general model editing are outside the sample’s scope. Its software scene buffers are 320 × 224 or 160 × 112 pixels. See `examples/openlogit/studio/scene.c` and `examples/openlogit/studio/README.md`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/studio/studio.c` | Code Studio entry point, host callbacks and event loop. |
| `c/apps/gui/studio/core/engine.h` | Frontend commands, state view and host contract. |
| `c/apps/gui/studio/core/internal.h` | Private engine state and completion-worker ownership. |
| `c/apps/gui/studio/core/engine.c` | Engine lifetime, tab closure, shutdown and scheduling. |
| `c/apps/gui/studio/core/document.h` | Document and history layouts and limits. |
| `c/apps/gui/studio/core/document.c` | UTF-8 validation, text storage, dirty state and undo history. |
| `c/apps/gui/studio/core/edit.c` | Editing, selection, movement, indentation and search operations. |
| `c/apps/gui/studio/core/pairs.c` | Typed delimiter pairing and selection wrapping. |
| `c/apps/gui/studio/core/highlight.c` | Syntax highlighting. |
| `c/apps/gui/studio/core/project.c` | Project listings, folder expansion, creation, deletion and opening. |
| `c/apps/gui/studio/core/session.c` | Session settings and restoration. |
| `c/apps/gui/studio/core/storage.c` | File I/O, recovery format and save-conflict detection. |
| `c/apps/gui/studio/core/source_snapshot.c` | Capture and serialization of open documents. |
| `c/apps/gui/studio/core/jobs.c` | Check and Run preparation. |
| `c/apps/gui/studio/core/runner.h` | Runner state, captured-source layout and limits. |
| `c/apps/gui/studio/core/runner.c` | Compiler launch, pipe polling, cancellation and child status. |
| `c/apps/gui/studio/core/diagnostics.c` | Compiler-result and diagnostic-range validation. |
| `c/apps/gui/studio/core/language.c` | Completion integration and problem navigation. |
| `c/apps/gui/studio/core/completion_job.c` | Asynchronous completion and response validation. |
| `c/apps/gui/studio/core/studio_project.inc` | File chooser and project-operation dialogs. |
| `c/apps/gui/studio/core/studio_editor.inc` | Keyboard input, clipboard integration and pointer placement. |
| `c/apps/gui/studio/core/studio_render.inc` | Text measurements, width cache and highlighting cache. |
| `c/apps/gui/studio/core/studio_code.inc` | Source painting and retained editor rows. |
| `c/apps/gui/studio/core/studio_view.inc` | Interface layout and repaint paths. |
| `c/apps/gui/aui.c` | Shared widgets and graphics integration. |
| `c/apps/as/include/snapshot.h` | Compiler snapshot protocol. |
| `examples/openlogit/studio/studio.h` | Scene Studio state and resource ownership. |
| `examples/openlogit/studio/main.c` | Scene application lifetime and events. |
| `examples/openlogit/studio/scene.c` | Generated meshes, scene rendering and picking. |
| `examples/openlogit/studio/ui.c` | Inspector controls, animation and composition. |
| `examples/openlogit/studio/shaders.h` | Embedded shader source. |
| `examples/openlogit/studio/README.md` | Sample controls and scope. |
| `tools/mkopenlogit_sdk.py` | Scene executable and example installation. |
| `Makefile` | Code Studio compilation, packaging and document preservation. |

## See also

- [Desktop applications](desktop-apps.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
