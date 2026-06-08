---
title: Aether Files — implementation plan
status: plan
date: 2026-06-06
---

Implement bottom-up; each step is independently buildable + has a gating check before the next. Build commands: `make` (kernel/ISO), `make build/disk.img` (apps/CLI/disk), `make test`, `make test-shell`, `make test-as*`. All paths absolute.

---
### STEP 1 -- FS primitive: vfs_rename (no syscall yet)
Files: `/Users/wangzhe/ststem/src/fs/vfs.h`, `/Users/wangzhe/ststem/src/fs/vfs.c`, `/Users/wangzhe/ststem/src/fs/aetherfs.c`.

1a. vfs.h: add `int (*rename)(const char *old, const char *new_path);` to `struct filesystem` (after `mkdir`, line 19). Add declaration after vfs_mkdir (line 37): `int vfs_rename(const char *old_path, const char *new_path);`.

1b. vfs.c: append wrapper after vfs_mkdir (line 62):
`int vfs_rename(const char *old_path, const char *new_path) { return (root && root->rename) ? root->rename(old_path, new_path) : -1; }`

1c. aetherfs.c: add `static int aetherfs_rename(const char *old_path, const char *new_path)` immediately before the `struct filesystem aetherfs` initializer (line 547). Algorithm (resolve-all-first, then mutate):
  - `uint32_t src = resolve(old_path); if (src == NOINO || src == sb.root_ino) return -1;`
  - `if (resolve(new_path) != NOINO) return -1;` (no clobber)
  - `char old_leaf[NAME_MAX], new_leaf[NAME_MAX];`
  - `uint32_t op = resolve_parent(old_path, old_leaf); if (op == NOINO) return -1;`
  - `uint32_t np = resolve_parent(new_path, new_leaf); if (np == NOINO) return -1;`
  - `struct dinode *npd = iget(np); if (!npd || npd->type != T_DIR) return -1;`
  - Cycle guard: `struct dinode *si = iget(src); if (si && si->type == T_DIR && path_under(old_path, new_path)) return -1;` where `path_under(a,b)` returns 1 iff normalized `b` equals `a` or starts with `a` followed by '/'. Implement `path_under` as a small static string helper in aetherfs.c (operates on the already-absolute kernel paths).
  - Mutate: `if (dir_add(np, new_leaf, src) < 0) return -1;`
  - `if (dir_remove(op, old_leaf) < 0) { dir_remove(np, new_leaf); return -1; }` (rollback)
  - `if (flush_inode(op) || flush_inode(np) || flush_bitmap()) return -1; return 0;`
  - NOTE: do not call resolve/dir_lookup/dir_nth between dir_add and dir_remove (shared blk_buf hazard) -- all resolution is done above.

1d. aetherfs.c: add `.rename = aetherfs_rename,` to the `struct filesystem aetherfs` initializer (lines 547-560).

GATE 1: `make` compiles cleanly (kernel links; aetherfs_rename referenced by the struct so no unused-static warning). No behavior change yet (nothing calls it).

---
### STEP 2 -- Syscalls + ABI + userland wrappers
Files: `/Users/wangzhe/ststem/include/abi/aether_abi.h`, `/Users/wangzhe/ststem/src/kernel/exec/syscall.c`, `/Users/wangzhe/ststem/src/kernel/gui/wm.c`, `/Users/wangzhe/ststem/src/apps/aether.h`, `/Users/wangzhe/ststem/src/apps/clib.h`.

2a. aether_abi.h: after SYS_SETNB (line 58) add `#define SYS_RENAME 65` and `#define SYS_OPEN_PATH 66`. After EV_CLOSE (line 89) add `#define EV_MOUSE_R 4`.

2b. syscall.c: add a `case SYS_RENAME:` before `default:` (line 254), mirroring SYS_MKDIR (lines 145-151):
```
case SYS_RENAME: {
    char o[128], n[128], ao[128], an[128];
    struct proc *p = proc_current();
    if (!p || user_copy_string(o, sizeof o, (const char *)r->rdi) < 0) { r->rax = (uint64_t)-1; return; }
    if (user_copy_string(n, sizeof n, (const char *)r->rsi) < 0) { r->rax = (uint64_t)-1; return; }
    proc_resolve(p, o, ao, sizeof ao);
    proc_resolve(p, n, an, sizeof an);
    r->rax = (uint64_t)vfs_rename(ao, an);
    return;
}
```
(proc.h, vfs.h, usercopy.h already #included at top of syscall.c.)

2c. wm.c: add a `case SYS_OPEN_PATH:` inside the `wm_gui_syscall` switch, before the closing `}` at line 525 (after SYS_GUI_CLIP, line 524):
```
case SYS_OPEN_PATH: {
    char path[USER_PATH_MAX];
    if (user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
    if (ends_aex(path)) wm_launch(path, "");
    else launch_for_ext(ext_of(path), path);
    return 0;
}
```
(ends_aex line 123, ext_of line 127, launch_for_ext line 242, wm_launch line 160 -- all visible here; USER_PATH_MAX=128 wm.c:29. The `if(!ap) return -1` guard at 297-298 keeps it GUI-only. No change to syscall.c for this -- the default arm forwards it.)

2d. aether.h: after make_dir (line 66) add:
`static inline int sys_rename(const char *old, const char *new_path){ return (int)_sys(SYS_RENAME,(long)old,(long)new_path,0); }`
`static inline int sys_open_path(const char *path){ return (int)_sys(SYS_OPEN_PATH,(long)path,0,0); }`

2e. clib.h: add a `path_join` static inline (after c_atoi, ~line 18):
```
static inline void path_join(char *dst, const char *dir, const char *name, int max){
    int i=0; for(; i<max-2 && dir[i]; i++) dst[i]=dir[i];
    if(i>0 && dst[i-1]!='/') dst[i++]='/';
    for(int j=0; i<max-1 && name[j]; i++,j++) dst[i]=name[j];
    dst[i]=0;
}
```

GATE 2: `make` compiles (kernel). Quick syscall sanity: build + `make test-shell` still passes (no new shell commands yet -- just verifies the ABI append didn't break boot/dispatch).

---
### STEP 3 -- Right-button input -> EV_MOUSE_R
Files: `/Users/wangzhe/ststem/src/drivers/char/mouse.c`, `/Users/wangzhe/ststem/src/kernel/gui/wm.h`, `/Users/wangzhe/ststem/src/kernel/gui/wm.c`.

3a. mouse.c: at line 87 add `int right = packet[0] & 0x02;`. At line 104 change call to `wm_mouse_event(mx, my, left, right);`.

3b. wm.h:10: change to `void wm_mouse_event(int x, int y, int left, int right);`.

3c. wm.c:69: change `static int mx, my, mleft;` to `static int mx, my, mleft, mright;`.

3d. wm.c:846: change signature to `void wm_mouse_event(int x, int y, int left, int right)`. After the existing `if (left && !mleft) { ... }` block (ends line 903) and BEFORE `if (!left) dragging = -1;` (line 904) add a parallel right-button block:
```
if (right && !mright) {
    for (int i = norder - 1; i >= 0; i--) {
        struct win *w = &wins[order[i]];
        if (!w->used || !in_rect(x, y, w->x, w->y, w->w, w->h)) continue;
        int cx = x - w->x, cy = y - w->y;
        if (cy >= TITLEBAR_H && w->kind == WK_APP) enqueue(w, EV_MOUSE_R, cx, cy - TITLEBAR_H);
        content = 1;
        break;
    }
}
```
Add `mright = right;` next to `mleft = left;` (line 906). (No raise/drag/close on right; WK_FINDER + Dock untouched.)

GATE 3: `make` compiles (kernel + mouse driver). `make test` still passes (boot OK). Right-click has no visible effect until an app handles EV_MOUSE_R.

---
### STEP 4 -- The Files app (src/apps/gui/files.c) + Makefile wiring
Files: NEW `/Users/wangzhe/ststem/src/apps/gui/files.c`, `/Users/wangzhe/ststem/Makefile`.

4a. files.c: `#include "aui.h"`. State: `char cwd[128]="/"; int sel_count; char sel[N][128]; int anchor; int scroll; int rename_mode,newfolder_mode; char editbuf[64]; int menu_open,menu_x,menu_y; char clip[N][128]; int clip_count,clip_cut; int shift_down,ctrl_down; int last_click_row,last_click_frame,frame_no;`. Pick N (e.g. 64) selection/clipboard cap.
  - `frame()`: `aui_begin(AUI_BG)`; draw breadcrumb (cwd) + Up button; toolbar row (New Folder/Rename/Delete/Copy/Cut/Paste aui_buttons -> set modes / run actions); list view rows from `dir_count(cwd)`/`dir_name(cwd,i,nm)` (folder marker + name + size-or-`--`), selected rows drawn with AUI_ACCENT background; the inline rename/newfolder aui_textfield when a mode is active; Get-Info panel when requested; finally the context menu (if menu_open) drawn last via gui_rect+gui_text_run; `aui_end()`.
  - Helpers: `copy_file(src,dst)` (sys_open O_RDONLY / O_WRONLY|O_CREAT|O_TRUNC, 4 KiB loop, close); `copy_tree(src,dst)` (make_dir + walk + recurse); `delete_tree(path)` (walk, delete files, recurse into dirs, then delete_file the empty dir); `is_dir(path)` via dir_count>=0 or dir_name parent lookup; `path_join` (reuse from clib.h is CLI-only -- define a local one here or include clib.h; simplest: a local static path_join in files.c since GUI apps use aether.h not clib.h). Use `sys_open_path` for double-click-file open; `sys_rename` for move/rename; `make_dir` for New Folder.
  - Event loop (modeled on widgets.c lines 50-62): `gui_create("Files", WINW, WINH); frame();` then loop poll_event:
    - EV_CLOSE -> app_exit(0).
    - EV_KEY: track shift/ctrl if the keyboard delivers them as keys (if not delivered, modifiers stay 0 -> single-select fallback, acceptable); PageUp/PageDown adjust `scroll`; feed to aui (for the active textfield) then frame.
    - EV_MOUSE_R -> set menu_open=1, menu_x=e.a, menu_y=e.b; frame; continue (do NOT aui_feed -- right-click is app-only).
    - EV_MOUSE -> if menu_open: hit-test menu items (run action or close); else hit-test list rows for selection (plain/shift/ctrl) + double-click detection (same row within a few frames -> open file via sys_open_path or navigate into folder); then aui_feed/frame/aui_feed_done for toolbar buttons + textfield.
  - frame_no++ each loop for the double-click heuristic.

4b. Makefile: add after line 104 (widgets): `$(eval $(call APP_RULE,files, 0x47000000,Files,-,F,120,190,140))`. Change line 108 to `APPS := clock textedit monitor terminal widgets files`.

GATE 4: `make build/disk.img` builds files.aex and packs it. Boot (`make run` manual, or proceed to STEP 7's qmp test) -> Files appears in the Dock and launches; basic list view renders cwd. (Functional verification finalized by STEP 7.)

---
### STEP 5 -- CLI: cp + mv
Files: NEW `/Users/wangzhe/ststem/src/apps/coreutils/cp.c`, NEW `/Users/wangzhe/ststem/src/apps/coreutils/mv.c`, `/Users/wangzhe/ststem/Makefile`.

5a. mv.c (`#include "clib.h"`): `main(argc,argv)`: require argc==3; `if (sys_rename(argv[1],argv[2])<0){ errs("mv: cannot rename\n"); return 1; } return 0;`.

5b. cp.c (`#include "clib.h"`): parse optional `-r`; require src+dst. `copy_file(src,dst)`: sys_open(src,O_RDONLY), sys_open(dst,O_WRONLY|O_CREAT|O_TRUNC), 4 KiB `char buf[4096]` read/write loop, close both. `do_cp(src,dst,recursive)`: `int n=dir_count(src); if(n>=0){ if(!recursive){errs("cp: -r required for directory\n");return 1;} make_dir(dst); for i in 0..n: dir_name(src,i,nm); path_join(cs,src,nm); path_join(cd,dst,nm); do_cp(cs,cd,1);} else copy_file(src,dst);`. Uses clib.h `path_join` from STEP 2e. Return nonzero on open failure.

5c. Makefile line 123: append `cp mv` -> `CLI := sh echo ls cat pwd wc head true false sleep mkdir rm touch clear uname net cp mv`. (CLI_RULE + disk packing line 216 auto-handle them; base 0x50000000.)

GATE 5: `make build/disk.img` builds cp.aex + mv.aex packed under /bin. Manual sanity optional; formal check is STEP 6.

---
### STEP 6 -- Shell round-trip test (cp/mv/mkdir/rm)
File: `/Users/wangzhe/ststem/scripts/run-shell-test.sh`.

6a. Extend the command line fed to the shell (line 18) to add, before `exit`, a deterministic round-trip in a self-created dir (no /tmp assumption):
`mkdir /cptest; echo cpmvprobe > /cptest/a.txt; cp /cptest/a.txt /cptest/b.txt; cat /cptest/b.txt; mv /cptest/b.txt /cptest/c.txt; ls /cptest; rm /cptest/a.txt; rm /cptest/c.txt; rm /cptest`. Keep existing commands. (sh supports `>` redirect: sh.c:73.)

6b. Add an assertion in the poll loop (line 25): also require `grep -aq "cpmvprobe"` (proves cp produced b.txt and cat read it back) in addition to the existing markers, OR assert on a final `echo cp-mv-ok` appended after the sequence -- choose the `cpmvprobe`-from-cat assertion (it actually proves cp+cat data flow). Keep PASS/FAIL prints. -snapshot already set (line 19) for determinism.

GATE 6: `make test-shell` prints PASS and the new cpmvprobe assertion holds. This independently validates STEP 1 (vfs_rename via mv), STEP 2 (SYS_RENAME), STEP 5 (cp/mv) end-to-end on the real process stack.

---
### STEP 7 -- QMP screenshot test for the Files app
File: NEW `/Users/wangzhe/ststem/tools/qmp_files.py` (model on `/Users/wangzhe/ststem/tools/qmp_fs.py`).

7a. Boot ISO+disk headless with QMP socket + serial file; wait for AETHER_BOOT_OK (qmp_fs.py:31-49 pattern). Connect QMP, qmp_capabilities.

7b. Launch Files from the Dock: `goto(<files dock icon x>, 720); click()` -- compute the icon x from dock layout (Files is the 6th scanned app; if exact x is uncertain, reuse the qmp_fs.py goto+click helpers and target the Files icon by stepping along the dock row; acceptable to screenshot the whole desktop and assert the window appears).

7c. Drive: New Folder (click toolbar button) -> type a name (reuse the `send`/`key`/KMAP helpers) -> Enter; Rename (select the new folder row, click Rename) -> type new name -> Enter; Delete (click Delete). Optionally inject a right button to pop the context menu: `input-send-event` with `{"type":"btn","data":{"button":"right","down":true/false}}` (same shape as the left btn in qmp_fs.py:74-77) and screenshot.

7d. `screendump` to the out .ppm; `quit`; print `PASS: files app new/rename/delete + screenshot` (or FAIL). Mirror qmp_fs.py exit semantics. (This test is screenshot/launch oriented -- the deterministic data correctness is already covered by STEP 6.)

GATE 7: `python3 tools/qmp_files.py build/aether.iso build/disk.img build/files_smoke.ppm` runs to completion and produces the screenshot (PASS). Inspect the .ppm visually if needed.

---
### STEP 8 -- Full regression
8a. `make` (clean kernel build) -> AETHER_BOOT_OK path intact.
8b. `make test` -> PASS (boot/serial assertion).
8c. `make test-shell` -> PASS (incl. new cp/mv/mkdir/rm round-trip).
8d. `make test-as` + `make test-as-gcstress` + `make test-as-os` -> PASS (ABI additions are pure appends; must be unaffected).
8e. `make build/disk.img` -> all .aex (files, cp, mv) packed; MAXWIN=8 not exceeded (7 Dock apps).

GATE 8 (final): every command above passes. Deliverables: vfs_rename primitive, SYS_RENAME + SYS_OPEN_PATH + EV_MOUSE_R, files.c app, cp/mv coreutils, extended shell test + qmp_files.py, all regressions green.
