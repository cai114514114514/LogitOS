# Code Studio application engine

The engine lives under `c/apps/studio/`. It is part of the Studio application,
compiled into `studio-engine.a` and linked into the existing `studio.aex`.
`c/apps/gui/studio.c` and the `studio_*.inc` files here own the window, layout,
glyph measurements, pointer hit testing, clipboard integration and shortcuts.

## Ownership

| Module | Responsibility |
| --- | --- |
| `engine.h`, `engine.c` | Opaque engine lifetime, read-only state view, scheduling, tabs and shutdown |
| `document.c`, `edit.c`, `navigation.c` | UTF-8 text, selections, undo/redo, search/replace, movement and indentation |
| `project.c`, `session.c` | Lazy folder tree, expansion, native chooser listings, file/folder creation, confirmed deletion and session restore |
| `studio_*.inc` | Studio presentation, drawing, dialogs and input adapters (included only by the GUI entry point and renderer tests) |
| `pairs.c`, `highlight.c` | Typed delimiter pairs, selection wrapping and tolerant syntax colours |
| `storage.c` | Full file I/O, two-slot draft recovery and external-edit conflict detection |
| `jobs.c`, `runner.c` | Captured source, nonblocking child I/O, cancellation and actual wait status |
| `language.c`, `diagnostics.c` | Completion providers, result validation and revision-bound problem locations |

Frontends call `st_engine_*` commands and read `const StState` from
`st_engine_state()`. They must not mutate that view or retain its text buffers
across commands. Each engine owns its project, documents and completion context.
The original AetherScript completion implementation is reused through
`as_complete_with()`; it is not copied into the GUI or replaced by a second
compiler. Its shared scratch buffers require serialized calls.

`StHost` supplies the clock, optional settings persistence and compiler/module
paths. No engine module includes the window API or calls a GUI syscall. Storage
and processes currently use the POSIX interfaces supported by LogitOS mini-libc
and the host. Callers ignore SIGPIPE before polling child input, as Studio does.
`st_engine_shutdown()` checkpoints documents and can fail; only destroy the
engine after handling that result. `destroy()` itself does not imply a save.

## Validation

```sh
make BUILD=build-studio-refactor test-studio-core test-as-check test-complete
make BUILD=build-studio-refactor build-studio-refactor/studio.aex
```

`test-studio-core` links the application engine without any GUI object and runs
with AddressSanitizer and UndefinedBehaviorSanitizer. It exercises Unicode
selection and history, recovery, external edits, independent project completion,
unsaved module content, compiler diagnostics, stale result rejection, actual
child execution/cancellation and session restoration. Its prerequisite builds
six private source mutants and requires failures specifically at UTF-8
boundaries, stale diagnostics, automatic pairing, missing-file opening,
deleted-draft recovery and closing a clean tab after its parent disappears.

## Current limits

This extraction is an architectural change to the existing application. Checks
currently diagnose syntax with the original compiler; completion still has its
existing independent semantic approximation. Static typing, a unified semantic
frontend, LLVM native builds and native debugging remain outstanding work.
Correction during A3 migration: the existing Run action now routes an explicit
3.0 file through the native CLI. `runner.c` preserves the compiler's real path
in argv[0], so the host compiler finds its adjacent runtime after Studio enters
the project directory. `test-studio-core` executes that actual path, checks
native diagnostics and verifies the program's output and exit 7. The engine
test now constructs a native echo/cat pipeline and transfers its captured
bytes through scoped pipe endpoints before writing to the runner output.
That exercises command lowering through the actual Studio launcher as well.
The shipped guest checks A3 without any A2 compiler caches, but still requires the host
LLVM connection for building. Completion, multi-document snapshot handoff and
native debugging remain unconverted; host engine execution is not guest UI
acceptance or a completed build bridge.
The host tests and native link alone do not establish the guest GUI workflow.
`test-studio-ui` drives the real picker, creation, collapse, saved bytes,
paired typing and confirmed/cancelled deletion via QMP. It also verifies stale
session cleanup and compares partial/full rendered editor pixels.
`test-studio-persistence-os` additionally rebuilds the private image using the
production preservation flags, reboots it and verifies unsaved draft recovery.
Saves preserve recovery drafts but do not provide an atomic filesystem commit
or remove the race between checking external bytes and writing the file.

## Paint and rebuild invariants

- The editor measures and paints bounded UTF-8 runs, caching their exact widths.
  Input-only frames retain unchanged rows. Every retained row clips to its own
  24 px band; with the former 21 px pitch, repainting the next row erased the
  previous row's descenders (30 pixel differences in the guest reproduction).
- Full frames call `aui_end_rect` with the canvas extent. `aui_end` alone only
  knows toolkit draw calls and missed raw `gui_*` code/selection changes.
- Completion waits for a 180 ms typing pause; Ctrl+Space stays immediate.
- Typing pairs differs from paste. Pair insert/wrap/delete is one undo step;
  comments, string contents and escaped quotes suppress automatic openers.
- Studio's default `/docs` is user data. The production disk recipe must use
  `--preserve-merge /docs`: old user bytes and metadata win over packaged
  defaults, including `.studio` drafts, while new packaged siblings are added.
  The missing flag previously deleted projects on application rebuild even
  though the preserved settings still pointed at the missing tabs.
- A missing source may reopen only with a valid unsaved recovery draft. Clean
  checkpoints are cursor bookmarks, not copies to resurrect deleted files.
  Session restoration prunes unavailable tabs and remaps the active tab.
- Closing a clean tab does not depend on writing a bookmark. A missing parent
  previously made that tab impossible to close; dirty buffers still require a
  successful checkpoint so a close cannot silently discard edits.
- Project deletion is explicit and clears matching recovery slots and the open
  tab. Directories must be empty; recursive deletion is not implemented.
