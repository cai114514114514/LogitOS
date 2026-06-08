# Code Studio — an AetherScript IDE (MVP)

## Why

AetherScript (`/bin/as`, `.as`) is edited today in TextEdit (a 90-line plain-text
canvas) and run only from the Terminal. The point of an IDE is the tight loop:
**edit -> run -> see output/errors -> jump to the error -> fix**, all in one
window. This is the first AS/JS IDE (the planned [[ide-as-editor]]); JS is deferred
until `/bin/js` is revived.

## Scope (MVP)

IN: a standalone GUI app **Code Studio** for `.as` files — code editor (line
numbers, vertical scroll), AetherScript syntax highlighting, Run (fork+exec
`/bin/as`) with a bottom output pane, and click-the-error -> jump-to-line.

OUT (deferred, stated): `.js` running (needs `/bin/js`), a file-tree sidebar,
tabs/multi-file, autocomplete. One file open at a time.

TextEdit stays as-is (the simple `.txt` editor); Code Studio is a separate app and
`.as` associates to it.

## Architecture

New app `src/apps/gui/studio.c`, a ring-3 GUI process built like the other GUI apps
(links `aui` + `crt0`, uses the `gui_*` drawing primitives and `sys_*` syscalls).
Makefile: `APP_RULE(studio, 0x49000000, "Code Studio", as, {, color)`, add `studio`
to `APPS`. The `as` ext makes Finder open `.as` files in it.

Window ~780x620. Three horizontal bands:
- **Toolbar** (top, ~26px): file name + a `Run` button (also Ctrl+R). Shows
  "running…/saved/modified" status.
- **Editor** (middle): a left gutter with line numbers, then highlighted code with
  a caret; scrolls vertically when the file exceeds the visible rows.
- **Output** (bottom, ~150px): the captured stdout+stderr of the last run.

### Components (each independently understandable)

1. **Edit buffer** — `char text[MAXT]` (64 KiB) + `int len` + `int caret` (byte
   index) + `int top_line` (scroll). Operations: insert char, backspace, newline,
   move caret (←→↑↓, Home/End). Cursor row/col + line starts are computed by a
   small helper that scans `text`. (TextEdit's edit core, extended with arrows +
   scroll-to-caret.)
2. **Highlighter** — `hl_line(const char *line, int n, draw_run cb)`: a lightweight
   scanner that splits ONE line into colored runs: keyword / identifier / number /
   "string"/'string' / `#comment` / operator / plain. Keyword set = the
   AetherScript keywords. Called only for VISIBLE lines each frame, so cost is
   bounded by the viewport. Colors: a small fixed palette (keyword, string,
   comment, number, default).
3. **Runner** — `run_file()`: `write_file(path)` (save), `sys_pipe(outpipe)`,
   `sys_fork`; child `dup2(outpipe[1])->1,2`, `execve("/bin/as", {"as", path, 0})`;
   parent keeps `outpipe[0]`, `sys_set_nonblock`s it, records the child pid, sets
   `running=1`. The event loop, while `running`, `sys_read`s the pipe (non-blocking)
   into the output buffer and `sys_waitpid(pid, WNOHANG)`s; on child exit it drains
   the last bytes and clears `running`. Non-blocking so an infinite-loop script
   doesn't freeze the IDE; on EV_CLOSE a still-running child is killed/closed.
4. **Error locator** — after each read, scan new output for the pattern `line <N>`
   (every `/bin/as` lexer/compiler/runtime error carries it). Store `err_line`;
   the editor renders that line with an error background; clicking the output pane
   (or pressing Enter on it) jumps the caret to `err_line` and scrolls to it.
5. **App loop** — `gui_create`, then per WM event (EV_KEY / EV_MOUSE / EV_CLOSE):
   edit on keys, Run on Ctrl+R or the toolbar button, save on Ctrl+S; each frame
   re-render the three bands; poll the run pipe when active. Opened on a path
   (Finder association / arg) -> load that file; else a scratch buffer.

## Data flow

WM event -> dispatch (key->edit / Ctrl+R->run / mouse->caret-or-error-jump) ->
mark dirty -> frame: draw toolbar + gutter + highlighted visible lines + caret +
output pane. A run adds a side channel: the pipe fd is polled every loop iteration
until the child exits.

## Error handling

- `/bin/as` not found / fork fail -> show "could not run" in the output pane.
- Output buffer is capped (e.g. 16 KiB); overflow truncates with a notice.
- File too large for `text[]` -> load truncated + a status warning.
- Save failure -> status shows "save failed".

## Build sequence (each stage builds + is screenshot-verified in QEMU)

1. **Skeleton**: app + Makefile wiring + window + edit buffer + gutter/line-numbers
   + caret + scroll + Ctrl+S save. (Looks like an editor; `.as` opens in it.)
2. **Highlighter**: colorized visible lines.
3. **Runner**: Run button/Ctrl+R -> fork+exec /bin/as -> output pane (non-blocking).
4. **Error jump**: parse `line N`, flag + click-to-jump.

## Verify

- `make build/disk.img` builds `studio.aex`; boot Aether, open a `.as` file (Finder
  double-click or Dock), confirm: highlighted text, edit + Ctrl+S, Run shows
  `print` output, a deliberate syntax error shows the message + jumps to the line.
  Captured via `tools/qmp_*.py` screenshots.
- No regression: `make test`, `make test-shell`, `make test-as-os` stay green.
