---
title: Aqua Files — mature file manager
status: spec
date: 2026-06-06
---

## Aqua Files -- a mature ring-3 file manager

### Goals
Add a real, Dock-launchable, windowed file-manager app (`files.aex`) to Aqua OS, plus the kernel/FS/input primitives it needs, plus matching `cp`/`mv` CLI tools. The existing in-kernel Finder (wm.c WK_FINDER) is kept untouched as the lightweight desktop browser -- this work is purely additive.

Locked scope: right-click context menu only (NO drag-drop; no mouse-move / button-up delivery). Copy is done in userland via the fd API (no kernel copy primitive). Move/rename is a single new FS primitive (`vfs_rename`) that re-links a directory entry.

### The Files app (src/apps/gui/files.c) UX
A single window (`gui_create("Files", ...)`), drawn with the aui toolkit + raw `gui_*` syscalls:

- **Path/breadcrumb bar** at top showing the current directory (`cwd`, starts at "/") + an **Up** control (navigates to parent; no-op at "/").
- **Toolbar row** of aui buttons: `New Folder`, `Rename`, `Delete`, `Copy`, `Cut`, `Paste`.
- **Scrollable list view**: one row per entry from `dir_count(cwd)` / `dir_name(cwd,i,name)`; each row shows a type marker (folder vs file), the name, and the size in bytes (or `--` for a directory, since `dir_name` returns `-2` for dirs). Scroll via PageUp/PageDown keys (KEY_PGUP/KEY_PGDN already delivered as EV_KEY) and/or list paging when entries exceed the visible rows.
- **Selection model** (multi-select): plain left-click selects exactly one row; Shift+left-click extends a contiguous range from the anchor to the clicked row; Ctrl+left-click toggles the clicked row in/out of the selection. (Modifier state is tracked by the app from EV_KEY shift/ctrl press/release is NOT available, so modifiers are tracked via the keyboard the same way other apps do -- see Input below; if modifier tracking proves unavailable, plain/shift/ctrl still degrade to single-select, which is acceptable.)
- **Double-click**: a file -> `sys_open_path(fullpath)` (open with the associated app, exactly like the Finder double-click); a folder -> navigate into it (append to `cwd`). Double-click is detected in-app by a two-clicks-on-same-row-within-N-frames heuristic.
- **Right-click (EV_MOUSE_R)** anywhere in the content area opens an app-drawn **context menu** at the click point with: `Open`, `New Folder`, `Rename`, `Delete`, `Copy`, `Cut`, `Paste`, `Get Info`. The menu is drawn last each frame (on top) via `gui_rect` + `gui_text_run`; it is hit-tested in the app's raw event loop on the next EV_MOUSE; a left-click outside the menu closes it.
- **In-app clipboard**: a list of selected absolute paths + a mode flag (COPY or CUT). `Copy`/`Cut` capture the current selection into the clipboard. `Paste` into `cwd`:
  - COPY mode: for each clipboard path, copy into `cwd`. Files: fd read/write loop (4 KiB chunks). Folders: `make_dir(dest)` then recurse (walk `dir_count`/`dir_name`, copy children).
  - CUT mode: for each clipboard path, `sys_rename(src, dest)` (move). On success the clipboard is cleared (a cut item moves once).
- **Delete** (toolbar / menu): recursive. For each selected path: if it's a directory, walk it depth-first deleting files then empty subdirs bottom-up, then `delete_file` the now-empty dir; if it's a file, `delete_file` it.
- **Rename / New Folder**: use an aui textfield shown inline (a small input row) when `rename_mode` / `newfolder_mode` is active. Rename calls `sys_rename(old, cwd/newname)`. New Folder calls `make_dir(cwd/newname)`. Enter commits, the field hides.
- **Get Info**: shows name / type (file or folder) / size; for a directory also the immediate item count (`dir_count`). Drawn as a small info panel.

After every handled event the app re-runs `frame()` (immediate-mode), matching the widgets.c loop.

### FS / VFS changes (src/fs)
- **`vfs_rename(old, new)`** -- new primitive. Re-links a directory entry to move/rename a file OR a directory, within the single AquaFS volume (same-dir rename or cross-dir move). Implemented as `aquafs_rename` in aquafs.c using existing helpers (`resolve`, `resolve_parent`, `dir_add`, `dir_remove`, `iget`, `flush_inode`, `flush_bitmap`). Exposed through `struct filesystem.rename` (new fn pointer in vfs.h) + `vfs_rename` wrapper in vfs.c.
  - Resolve everything first (src ino, dest collision, both parents + leaves) into locals, THEN mutate, to avoid the shared static `blk_buf`/`ind_buf` hazard.
  - Reject: src == NOINO, src == root, dest already exists, parent not a dir, and (if src is a dir) dest being under src (cycle guard via absolute-path prefix test).
  - Order: `dir_add(new_parent, new_leaf, src_ino)` then `dir_remove(old_parent, old_leaf)`; roll back the add if the remove fails. Flush old_parent, new_parent, bitmap.
- **Directory deletion**: already supported. `aquafs_delete` (aquafs.c:481) deletes an EMPTY directory and rejects non-empty; `vfs_delete` (SYS_DELETE_FILE) is the rmdir path. No new primitive; recursive delete is driven by the caller (app/CLI).
- **Copy**: NOT a kernel primitive. Callers use the fd API.

### Syscall / ABI changes
In `include/abi/aqua_abi.h` (next free numbers after SYS_SETNB=64):
- `#define SYS_RENAME    65   /* (old_path, new_path) -> 0, or -1 */`
- `#define SYS_OPEN_PATH 66   /* (path) -> 0; open file with its associated app (GUI only) */`
- `#define EV_MOUSE_R    4    /* a = x, b = y (window-local), right-button down */` (after EV_CLOSE=3)

Kernel dispatch:
- `SYS_RENAME` -> a new `case` in `syscall_dispatch` (syscall.c, before `default:` at line 254). Copies both user strings, `proc_resolve`s each against the proc cwd, calls `vfs_rename`. Placed here (not wm) so CLI `mv` works.
- `SYS_OPEN_PATH` -> a new `case` inside `wm_gui_syscall` (wm.c, before line 525). Copies the path; if it ends in `.aex` `wm_launch(path,"")`, else `launch_for_ext(ext_of(path), path)`. No syscall.c change (the `default:` arm already forwards unknown numbers to wm_gui_syscall). GUI-only by design (`cur_app()==NULL` guard returns -1 for CLI).

Userland wrappers in `src/apps/aqua.h`:
- `sys_rename(const char *old, const char *new)` -> `_sys(SYS_RENAME, old, new, 0)`
- `sys_open_path(const char *path)` -> `_sys(SYS_OPEN_PATH, path, 0, 0)`

`src/apps/clib.h` includes aqua.h, so both wrappers reach the coreutils automatically. Add a `path_join(dst, dir, name, max)` static-inline helper to clib.h (mirrors wm.c's path_join) for cp/mv and reuse the same logic in files.c.

### Input changes (right-click only)
- `src/drivers/char/mouse.c`: extract `int right = packet[0] & 0x02;` (line ~87, currently discarded) and pass it: `wm_mouse_event(mx, my, left, right);` (line 104).
- `src/kernel/gui/wm.h:10`: `void wm_mouse_event(int x, int y, int left, int right);`
- `src/kernel/gui/wm.c`: add `static int mright;` beside `mleft` (line 69); extend `wm_mouse_event` (line 846) with a `right` param; add a `right && !mright` edge block (mirroring the `left && !mleft` block) that hit-tests windows in z-order and, for the topmost WK_APP hit in the content area, `enqueue(w, EV_MOUSE_R, cx, cy - TITLEBAR_H)`; set `mright = right;` beside `mleft = left;` (line 906). The right block does NOT raise/drag/close and ignores WK_FINDER and the Dock (the kernel Finder stays unchanged). NO mouse-move / button-up delivery is added.

EV_MOUSE_R surfaces through the existing `SYS_POLL_EVENT` ring-buffer pop (wm.c:363) unchanged. struct aqua_event is unchanged. Existing apps ignore type=4.

### CLI changes (src/apps/coreutils)
- **`mv.c`**: `mv SRC DST` -> `sys_rename(SRC, DST)`; nonzero exit + stderr on failure. Works for files and dirs (kernel re-links the dirent).
- **`cp.c`**: `cp [-r] SRC DST`. File: fd read/write loop (4 KiB stack buffer). With `-r` and SRC a directory: `make_dir(DST)` then walk `dir_count`/`dir_name`, `path_join` child paths, recurse. Without `-r` on a directory: error. Uses only existing fd + dir syscalls + the new nothing (cp needs no new syscall).
- Makefile: append `cp mv` to the `CLI` list (line 123). `CLI_RULE` + the `$(foreach c,$(CLI),...:/bin/$(c))` disk-packing (line 216) pick them up automatically; both link at the common CLI base 0x50000000.

### App registration (Makefile)
- New `APP_RULE` line: `files` at base `0x47000000` (next free after widgets 0x46000000; stack top entry+0x2800000 = 0x6F800000 stays inside PDPT[1] 0x40000000-0x7FFFFFFF), display "Files", no ext (`-`), icon `F`, color (120,190,140).
- Append `files` to `APPS` (line 108). Disk packing `$(foreach a,$(APPS),...)` (line 215) and Dock `scan_apps` (wm.c:914) pick it up.

### Scope & explicit deferrals
- IN: vfs_rename FS primitive; SYS_RENAME + SYS_OPEN_PATH syscalls + wrappers; EV_MOUSE_R right-click delivery; the Files app (list, multi-select, context menu, in-app clipboard copy/cut/paste, recursive delete, rename, new folder, get info, double-click open/navigate); cp/mv CLI; tests.
- DEFERRED / CUT (do NOT implement): drag-and-drop; mouse-move and button-up event delivery; middle button; multi-volume / cross-FS move; replacing/overwriting an existing destination on rename; in-place same-block dirent edit optimization (use the kmalloc+inode_write path like dir_add/dir_remove). The in-kernel Finder is NOT modified or removed.

### Test plan
1. **Build**: `make` (kernel ISO) and `make build/disk.img` (apps/CLI/fsroot) both succeed; `files.aex`, `cp.aex`, `mv.aex` packed.
2. **make test**: existing AQUA_BOOT_OK serial assertion still passes (regression).
3. **make test-shell** (extended): the serial-shell round-trip in scripts/run-shell-test.sh gains `mkdir /cptest; echo MARKER > /cptest/a.txt; cp /cptest/a.txt /cptest/b.txt; cat /cptest/b.txt; mv /cptest/b.txt /cptest/c.txt; ls /cptest; rm /cptest/a.txt; rm /cptest/c.txt; rm /cptest` and asserts a deterministic marker (e.g. `cp-mv-ok` echoed after the sequence, or the MARKER text from `cat`) appears -- exercising cp (fd copy), mv (SYS_RENAME), mkdir, and rm (incl. empty-dir removal). Uses -snapshot (already present) for determinism.
4. **make test-aqs / test-aqs-os / test-aqs-gcstress**: unchanged, must still pass (regression -- the ABI additions are pure appends).
5. **QMP screenshot test** (new tools/qmp_files.py, modeled on qmp_fs.py): wait for AQUA_BOOT_OK, launch Files from the Dock, then drive: New Folder (toolbar) -> type a name -> Enter; Rename it -> type new name -> Enter; Delete it; screendump to a .ppm. Optionally inject a `right` button (`input-send-event` btn right) to pop the context menu and screenshot it. Asserts the app launched and the screendump was produced (PASS/FAIL print like the other qmp_* scripts).
6. **Full regression**: make / make test / make test-shell / make test-aqs*.
