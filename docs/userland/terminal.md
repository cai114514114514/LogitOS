# Terminal

Terminal is a graphical frontend for `/bin/sh`. It displays text and structured output, maintains scrollback and selection, and groups output by command. The shell owns command editing, history, completion and foreground pipelines. Pipes carry text and input; the LRT/1 protocol carries command metadata, input snapshots and rich content (`c/apps/gui/terminal/terminal.c`, `c/apps/coreutils/sh.c`, `c/apps/coreutils/logit_rich.h`).

## Application structure

`c/apps/gui/terminal/terminal.c` contains the application lifecycle, shell connection, scrollback records, protocol handlers, input dispatch and painting. `c/apps/gui/terminal/term_line.h` provides the UTF-8 assembly and the operations that build lines.

`app_main()` loads the theme, measures text geometry, creates the window, initializes the protocol parser and launches the shell. Its event loop processes window events, flushes queued input, drains shell output, advances media playback and paints requested updates. `wait_idle(16)` waits for an event or timeout. The timeout gives the application another opportunity to read its pipes, whose activity reaches Terminal through polling.

The `Makefile` links Terminal with application startup code, libc, drawing support and media libraries, then packages it as an AEX application. `tests/agent.mk` augments its link through `tools/agent_link.py`, using Terminal’s entry in `c/apps/agent/catalog.json` to include the agent runtime and GUI hook.

## Shell connection

`spawn_shell()` creates five pipes, forks and executes the guest program `/bin/sh`. The shell receives this descriptor layout:

| Descriptor | Connection |
|---|---|
| 0 | Input pipe from Terminal. |
| 1 | Standard output pipe to Terminal. |
| 2 | Standard error pipe to Terminal. |
| 3 | LRT/1 output from the shell and eligible child programs. |
| 4 | Control frames from Terminal to the shell. |

Terminal makes its retained pipe endpoints nonblocking. Keyboard text travels through the control pipe as `RT_C_TEXT`. At the prompt, the shell applies those bytes to its editor. During a foreground job, it forwards them to the job’s input pipe. Interrupt requests and requests to end input use their own frame types, so the shell can process them while a child is reading input (`c/apps/gui/terminal/terminal.c`, `c/apps/coreutils/sh.c`).

The launch environment supplies `LOGIT_TERM`, `LOGIT_RICH` and `LOGIT_CTL` for protocol discovery. `LOGIT_COLS`, `LOGIT_ROWS` and `LOGIT_CELL` describe the initial geometry. Terminal sends later grid changes through `RT_C_SIZE`; the shell updates those environment entries for subsequent launches.

In `start_pipeline()`, the shell grants the rich channel to the final stage of a foreground pipeline whose stdout reaches Terminal. For other stages, `env_build()` removes `LOGIT_RICH` and `LOGIT_CTL`, and child setup closes the rich descriptor. Every pipeline child closes the shell’s control descriptor. These decisions use the pipeline and redirection information already held by the shell.

## Line buffer and screen model

Scrollback is a ring of `struct tline` records. Each record stores UTF-8 bytes, a byte length, a stream identity, command metadata and a count of display rows. A record for a rich object also stores an object descriptor index and generation (`c/apps/gui/terminal/terminal.c`).

Text records occupy one display row. Rich objects reserve several rows according to their stored height. Scrolling, painting and hit testing walk the records and accumulate their `vrows` values.

Each stream has its own unfinished line and UTF-8 assembler, so stdout and stderr can arrive in interleaved reads while each builds a separate record. `open_line` stores absolute positions; `cur_line()` translates them into the current ring. This preserves the association between a stream and its unfinished record as older entries are evicted.

`RT_T_CMD_BEGIN` creates command metadata and an echoed command record. New lines capture the current command slot and ID. `RT_T_CMD_END` records status and interruption state. `cmd_of()` checks the stored ID against the slot’s current ID before using its metadata. That check allows command slots to be recycled while older text still occupies scrollback.

Command metadata drives status markers, folding, copying and rerun actions. With the errors filter enabled, `line_visible()` includes stderr records and records associated with a completed command whose status is nonzero. With the filter disabled, folding hides a command’s output while the echoed command stays on screen.

## Text controls and escape handling

For bytes admitted by the binary guard, `put_char()` handles these controls before passing printable bytes to the UTF-8 assembler:

| Input | Operation |
|---|---|
| Newline | Creates a line if needed, flushes pending UTF-8 bytes and closes the line. |
| Carriage return | Discards the byte. |
| Backspace or DEL | Clears a pending partial sequence and removes the final code point from the current line. |
| Tab | Adds spaces to the next tab stop, a multiple of eight cells, bounded by line capacity. |
| Other C0 controls, including ESC | Discards the byte. |

`c/apps/gui/terminal/term_line.h` collects UTF-8 sequences across reads. `tl_fits()` checks both display width and byte capacity before a unit is appended. An interrupted sequence is emitted as its collected raw bytes. `tl_backspace()` removes the final decoded character, and `tl_tab()` measures tab spacing in cells.

`c/apps/coreutils/logit_cells.h` supplies the shared decoding and cell conversions. Its width rule assigns two cells to East Asian Wide and Fullwidth code points and one cell to other code points, including combining marks. The shell uses this rule to publish its cursor position; Terminal uses it for wrapping, selection coordinates and the input viewport.

Display requests also arrive as LRT/1 frames. For example, `c/apps/coreutils/clear.c` sends `RT_T_CLEAR`, which calls `clear_scrollback()`. That operation resets scrollback and scrolling state, pauses videos and stops audio.

## Binary output and frame ordering

`put_bytes()` passes stdout and stderr through `sniff_guard_byte()` before line construction. The guard classifies a stream as binary when it encounters a NUL or reaches its threshold for control bytes. ESC and DEL contribute control evidence; tab, newline, carriage return and backspace are exempt (`c/apps/coreutils/logit_sniff.h`).

When classification changes, Terminal appends a summary record. Later chunks update that record with a count of suppressed bytes, a format name when recognizable, and a hexadecimal prefix. The guards reset when a new command begins.

LRT/1 uses a header of 16 bytes containing magic bytes, version, type, flags, a stdout byte count and payload length. Strings carry lengths within the payload. The parser accepts fragmented reads, searches for a header after garbage and rejects payload lengths above 16 KiB (`c/apps/coreutils/logit_rich.h`).

The event loop drains rich frames before stdout and stderr, so the frame that opens a command can establish the current command and reset its guards before any text is processed.

For content frames, `seq` provides an ordering anchor. Producers advance their counter when writing through `rt_out()` or `rt_outn()`. Terminal records a stdout baseline at command begin and compares consumed bytes against that baseline plus the frame’s anchor. Eligible frames wait in a defer queue until the text catches up. Frames that mark command boundaries, frames that report input state and greeting frames are dispatched immediately (`c/apps/gui/terminal/terminal.c`).

## Input, selection and scrolling

The shell stores its editable command in `lbuf`, with `lcur` as a byte offset. Its editor handles character movement, backspace, history and completion. `publish_input()` sends `RT_T_INPUT` containing the prompt, UTF-8 buffer and cursor position in display cells (`c/apps/coreutils/sh.c`).

Terminal draws that snapshot in the fixed input strip at the bottom of the window. `lc_view()` chooses a horizontal slice at character boundaries and returns the caret position within that slice.

The main keyboard actions are:

| Input | Action |
|---|---|
| Text, including committed Unicode characters | Batch UTF-8 bytes into `RT_C_TEXT`. |
| Left, Right, Home, End, Up and Down | Send `RT_C_KEY`; the shell applies navigation at the prompt. |
| Page Up and Page Down | Scroll by approximately one viewport. |
| Shift+Up and Shift+Down | Scroll one display row. |
| Shift+Home and Shift+End | Move to the beginning or follow the bottom. |
| Ctrl+C | Copy when a selection exists; otherwise send `RT_C_INTR`. Shift also selects the copy action. |
| Ctrl+D | Send `RT_C_EOF`. |
| Ctrl+E | Toggle the errors filter. |
| Ctrl+V | Queue bytes from Terminal’s local copy buffer. |
| Ctrl+L | Invoke the agent GUI context hook in the catalogued build. |

At the prompt, an interrupt clears the shell’s edit buffer. During a foreground job, `wait_foreground()` sends `LOGIT_SIGINT` to its pipeline stages. It gives the job a bounded grace period before returning a surviving job to the background. A request to end input closes the job’s input writer; at an empty prompt it ends the shell session.

The Ctrl+L branch in Terminal’s local `on_key()` calls `clear_scrollback()`. The event loop first calls `ag_gui_event()`, and the linked agent hook consumes Ctrl+L. That hook publishes the bytes supplied by `ag_gui_context()` and opens the assistant after successful publication. Terminal’s callback supplies the current queued `typed` batch. The Edit menu’s clear action reaches the local handler through a menu event (`c/lib/agent/gui.h`, `c/lib/agent/gui.c`, `c/lib/agent/sdk.c`).

Mouse selection stores columns in display cells. `copy_selection()` converts those endpoints to byte offsets before collecting record text into `clip`. Right-clicking the text area of a record with command metadata invokes `copy_command()`. A gutter click folds or expands the command; clicking a completed command echo sends `RT_C_RERUN`.

The mouse wheel scrolls three rows per unit. Clicking the scrollbar maps its vertical position to a scroll position. The `follow` flag makes painting place the viewport at the latest output (`c/apps/gui/terminal/terminal.c`).

## Rendering and rich content

`measure_geometry()` measures a monospace “M” through `text_measure_px()` and derives line pitch from `UIPX` with additional leading. `derive_layout()` calculates the text area and grid from the window size, padding, gutter, scrollbar and input strip. `on_resize()` recalculates the layout and reports changed grid dimensions to the shell.

`paint()` clears the canvas, draws visible records, then draws the scrollbar and input strip. Stream identity selects text color. Command echoes receive gutter status markers. A clip rectangle confines rich objects to the scrollback viewport (`c/apps/gui/terminal/terminal.c`).

Drawing uses the GUI wrappers in `c/apps/logit.h` and their OpenLogit window transport. During a full paint, Terminal records drawing geometry and content hashes. `td_finish()` compares these records by call order with the previous paint and combines changed extents. `td_flush()` publishes the resulting rectangle, skips publication for an identical recording, or publishes the full canvas after initialization, resizing or recording overflow.

Rich frames create images, progress widgets, tables, charts, video and audio objects. Progress handling searches recent records of the current command for a matching widget ID and updates it in place. Table rows can carry paths that open when clicked. The `RT_T_LM_BEGIN`, `RT_T_LM_TOKEN` and `RT_T_LM_END` handlers add generated text and a closing statistics record.

Images use `img_open()`, which invokes the kernel’s `SYS_IMG_DECODE` handler. Terminal downsamples the returned pixels into a retained thumbnail (`c/apps/logit.h`, `c/kernel/exec/syscall.c`). Video and audio decoding execute within Terminal using the libraries linked by the `Makefile`.

A video object owns a display region and updates its pixels in place. After its first picture establishes geometry, playback advances one picture per turn of the event loop for each visible active clip. `present_videos()` blits the stored video regions and publishes their combined rectangle between full paints. Clicking a video toggles pause or restarts a finished clip.

Audio attaches stopped and starts on a click. Playback continues as its object scrolls out of view. `aud_step()` writes decoded samples through a nonblocking sound stream and retains samples left over after a short write.

## Limitations

- The shell connection uses pipes and LRT/1 without a PTY or ANSI/VT parser. ESC is discarded while printable sequence bytes can appear as text; repeated escapes can trigger binary classification. Foreground applications requiring cursor addressing or terminal navigation sequences need additional integration (`c/apps/gui/terminal/terminal.c`, `c/apps/coreutils/sh.c`).
- Scrollback holds 600 records, wrapping is capped at 200 cells, and each text record holds 400 bytes. Command metadata and object descriptors each have 96 slots (`c/apps/gui/terminal/terminal.c`).
- Input buffers have different capacities. The shell’s command buffer is 4096 bytes; Terminal retains 511 bytes of the live input snapshot and 95 bytes of command text used for rerun. The shell consumes at most 511 text bytes from each `RT_C_TEXT` frame. Long snapshots, pastes and rerun text can be truncated. Copy operations also stop at a byte limit that can split UTF-8 (`c/apps/coreutils/sh.c`, `c/apps/gui/terminal/terminal.c`).
- The outbound control ring has 4095 usable byte positions. `cout_push()` drops bytes when full, which can lose input and truncate frames. Ordering deferral has eight slots of 1024 payload bytes; larger frames and frames arriving at a full queue run immediately. Anchors count each producer’s stdout bytes, so they provide no total order across stdout, stderr and concurrent producers (`c/apps/gui/terminal/terminal.c`, `c/apps/coreutils/logit_rich.h`).
- A resize keeps the wrapping of existing text and the stored dimensions of images and videos. The shared Unicode cell rule can disagree with the renderer’s width derived from font advances, affecting alignment and selection (`c/apps/gui/terminal/terminal.c`, `c/apps/coreutils/logit_cells.h`, `c/lib/text/shape.c`).
- Content pools hold six images, six tables, eight charts, sixteen progress widgets, two videos and two audio objects. Payload slots can be reused while older object descriptors still reference them, allowing older records to display replacement content. Video accepts up to 12 MiB of H.264 or H.265 Annex-B data; audio input is capped at 16 MiB (`c/apps/gui/terminal/terminal.c`).
- Tables store six columns and 48 rows. The handler reports truncation on screen and through a diagnostic, but it reads the clamped column count without skipping surplus column fields, so frames wider than six columns can be misparsed (`c/apps/gui/terminal/terminal.c`).

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/terminal/terminal.c` | Lifecycle, shell pipes, scrollback, input, rich objects and rendering. |
| `c/apps/gui/terminal/term_line.h` | UTF-8 assembly, wrapping, backspace and tab operations. |
| `c/apps/coreutils/sh.c` | Editor, history, completion, pipeline launch and foreground input handling. |
| `c/apps/coreutils/logit_rich.h` | LRT/1 framing, encoding, parsing, discovery and ordering counters. |
| `c/apps/coreutils/logit_cells.h` | UTF-8 decoding, cell widths, cursor boundaries and viewport calculations. |
| `c/apps/coreutils/logit_sniff.h` | Format identification and the guard for binary output. |
| `c/apps/coreutils/clear.c` | Rich clear request and escape fallback. |
| `c/apps/logit.h` | GUI, event, image decoding and text measurement wrappers. |
| `c/kernel/exec/syscall.c` | Kernel image-decoding syscall implementation. |
| `c/lib/text/shape.c` | Glyph placement and widths from font advances for the monospace path. |
| `c/lib/agent/gui.h` | Wrapper for the optional GUI event hook. |
| `c/lib/agent/gui.c` | Ctrl+L context dispatch. |
| `c/lib/agent/sdk.c` | Context publication and assistant launch. |
| `c/apps/agent/catalog.json` | Terminal’s application identity and agent catalogue entry. |
| `Makefile` | Terminal compilation, linking and AEX packaging. |
| `tests/agent.mk` | Agent runtime dependencies and linker augmentation. |
| `tools/agent_link.py` | Adds agent entry code and SDK archives to catalogued applications. |
| `tests/unit/term_wrap_test.c` | Cases for cell wrapping, fragmented UTF-8, deletion, tabs and byte capacity. |
| `tests/term_wrap.mk` | Test for the line builder and its prerequisite negative control. |

## See also

- [Command-line tools](coreutils.md)
- [Desktop applications](desktop-apps.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [GUI events, clipboard and notifications](../kernel/gui-events-and-services.md)
