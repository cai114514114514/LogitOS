# "Fluid" — the experience release: responsive, Mac-like, usable

Written 2026-09-24 against HEAD `5fdffda06`. Every "current state" line below
was re-verified at that commit, because the CLAUDE.md sections it would
otherwise be quoted from are dated 2026-08-28 and several are stale.

## Why

The ask was one release that moves the machine's *felt* quality a whole
class. Three axes, one slice of each:

1. **Responsive** — dragging, typing and scrolling follow the hand.
2. **Mac-like** — the desktop contract an app expects: its own menus, a
   switcher you can hold, windows that snap, Spotlight.
3. **Usable** — the Terminal is a place work can happen: CJK text wraps
   correctly, scripts run by `#!`, and the text tools exist.

Decided up front: 1920×1200 stays the default (1280×800 remains a make
option); Cmd+Space becomes Spotlight, with an "Ask Assistant" row so the
assistant stays one keystroke away; multi-window-per-process is deferred to
the next release; the text tools are written from scratch, not ported.

## Current state (verified at `5fdffda06`)

| area | state | evidence |
|---|---|---|
| flush rectangle | **exists** — `SYS_GUI_FLUSH_RECT` 188, aui uses it | `wm.c:2761`, `aui.c:373` |
| BKL | gone (2026-09-10); pixel pass outside `wm_lock` | `docs/BKL_REMOVAL_2026-09-10.md`, `78fff329d` |
| composition | **single thread**; only the final present is band-parallel | `render_region`, `smp.c:253 smp_present_par` |
| frame clock | none: composites whenever damage exists | `wm_run` |
| last frame times | 94–104 ms for large drag / keystroke / terminal scroll, ~95% idle | CLAUDE.md, 2026-08-28 — **predates four optimisations; re-measure** |
| menus | kernel constants `{"LogitOS","File","View"}`, fixed dropdowns | `wm.c:3489`, `wm.c:3942` |
| key release | none; releases dropped in the driver | `keyboard.c:149,187` |
| Cmd+Tab | switches instantly, no panel — *because* there is no key-up | `wm.c:5489` |
| snapping / close animation | none | — |
| Terminal wrap | on BYTES against a width in CELLS | `terminal.c:641` |
| `#!` | not honoured by `execve` | `c/kernel/exec/load/exec.c` |
| `statvfs` / `df` | `ENOSYS` / absent | `libc/src/fsmisc.c` |
| text tools | no grep sed sort uniq tail cut tr find diff kill ln chmod | `c/apps/coreutils/` |

Syscall numbers 190–200 are spread over `aex_agent.h`, `pty.h` and
`fs_ref.h`. New calls start at **201**; grep every `include/abi/*.h` before
claiming one.

## Shape of the release

Two tracks. Track A lives almost entirely in `wm.c` and is serial; track B is
userland and can run beside it.

- **A:** P0 baseline → P1 parallel composition + frame clock → P2 key-up +
  switcher → P3 snapping + close animation → P4 Spotlight → P5 app menus
- **B:** T1 terminal wrap + `#!` + statvfs/df/du → T2 regex + grep + sed →
  T3 sort uniq tail cut tr find diff kill ln chmod

Every phase lands on its own, with a make gate and a negative control that
has been **watched failing**. A gate that cannot run on the macOS host skips
loudly, naming what is missing.

## P0 — re-baseline, add instruments

The 94 ms figure predates the cover-skip, the flush rectangle, the pixel pass
leaving `wm_lock` and the glass rate halving. Nothing in P1 is planned against
it until it is measured again.

- `wm_perf_report` gains an **input-to-present latency**: stamp a pointer
  event when it is queued; at present time record the latency of the newest
  pointer that frame applied (p50, max). Plus `parframes`/`serialfb` and
  per-band time, which read zero until P1.
- `qmp_repaint.py` gains `--assert-fps`, `--assert-speedup`, `--compare-json`.
- Gate `test-repaint-baseline` writes the JSON. Control: `-smp 1` must show
  the latency counter rising.

## P1 — band-parallel composition and a frame clock

Three of four cores are idle while a frame takes ~100 ms. The lever is to use
them.

**Split vertically, and never through a glass panel.** A pure splitter
(`c/kernel/gui/wm_bands.h`, host-testable) takes the damage rectangle and a
list of no-cut row ranges — each glass panel's full *sample* extent, from the
same `ol_display_glass_sample_halo` that `rect_or_glass_input` uses. It
weights rows by cost (glass ≈3.5×), cuts at equal-cost points snapped to legal
rows, and refuses bands under 32 rows. Because every panel and everything it
samples lie inside one band, each band renders exactly what a serial frame
would — **no barrier is needed**, and the property is checkable pixel for
pixel.

**Per-CPU drawing state.** `fb.c`'s target `T`, the clip (held on the shared
`screen` surface), the one `sdk_display` context and the dirty flag become
per-CPU. The glass tables in `openlogit_glass.c:66-70` are global and rebuilt
whenever panel geometry changes, so they move into `struct ol_display` (or
are prebuilt on the WM thread before dispatch). Lazily filled mask caches are
prewarmed or locked. Perf counters become per-CPU and are summed at frame end.

**Dispatch by IPI.** A woken thread waits up to a tick for an idle core, so
workers are driven the way `smp_present_par` already is. The WM thread
publishes the bands and a claim counter, sends the IPI, and claims bands
itself. A core takes work only if it interrupted ring 3 or idle — so it holds
no kernel lock. Unclaimed bands are done by the WM thread; there is no
timeout. `fb_present_rect` stays single-caller.

**Serial frames stay serial**: the locked greeter, any open / minimise /
Exposé / close animation (shared `anim_scratch`), and rectangles under 128
rows. A `wm.par_bands` setting switches the path at runtime so one build can
be A/B'd.

**Frame clock.** Render only when damage is pending and ≥16 ms have passed
since the last present; otherwise block until the deadline. The input queue
already coalesces pointer motion, so the newest position wins.

**P1b, only if P1 misses 30 fps:** split the menu-bar and dock glass into
parallel row-blur, column-blur and column-slice render passes.

Gates: `test-wm-bands` (host: exact cover, no overlap, no cut inside a panel;
control `-DWM_BANDS_NEGCTL_IGNORE_GLASS`); `test-wm-par` (guest, a
`WM_PAR_VERIFY` build re-renders each parallel rectangle serially and counts
mismatches, which must be zero with `parframes>0`; the cut-ignoring build must
mismatch; `qmp_damage.py` re-run with bands on); `test-wm-par-perf` (`par_bands`
1 vs 4 ≥2.5×, ≥30 fps on drag/type/scroll — reference host only, skips loudly
without `LOGIT_PERF_HOST`; control: workers always decline); `test-frameclock`
(an app flushing every 1 ms yields ≤62 composites/s; control: clock off).

## P2 — key-up, and a switcher you can hold

- PS/2: `modk()` reports modifier edges via `wm_mods_changed()`; releases of
  mapped keys go to `wm_key_up()` instead of being dropped. USB HID diffs
  consecutive reports.
- Input queue: `INPUTQ_KEYUP`, `INPUTQ_MODS` — never merged, and they wake the
  WM loop.
- ABI: `EV_KEY_UP = 11`, modifier key codes `KEY_SHIFT/CTRL/ALT/CMD`
  (0x115–0x118), `WINS_WANT_KEYUP = 8`. Only windows that opt in see key-up
  and modifier-down, so no existing app changes behaviour. `abi_layout.inc`
  follows; `check-abi` passes.
- Switcher: an MRU list kept in `wm_sync_focus`; Cmd+Tab opens a panel on the
  second entry, Tab/Shift+Tab step, Cmd release commits, Esc cancels, a tap
  under 150 ms commits without drawing. While open, every WM pass rechecks
  `kbd_mods()` so a lost release can never strand it.
- Gates `test-keyup`, `test-switcher`; control `KEYUPNEGCTL=1` (no release
  edge ⇒ no commit ⇒ named FAIL).

## P3 — snapping and a close animation

- Drag zones: left edge → left half, right edge → right half, top → maximise;
  a non-glass preview whose damage is only its own boxes. Half frames derive
  from `zoom_rect` so they stay clear of the dock; top reuses
  `win_set_zoom`. `snapped` sits beside `zoomed`, and dragging a snapped
  window away restores its saved size.
- Close: `reap` hands the surface to a small ghost array instead of freeing
  it; Exposé's scaled-and-faded drawing becomes `draw_window_animated()`,
  shared by both. 150 ms, scale 1.0→0.92, alpha 255→0. Skipped under reduce
  motion, for minimised windows and on the lock screen.
- Gates `test-snap` (control: release does not apply) and `test-close-anim`
  (control: last frame under-reports damage, so the round trip fails).

## P4 — Spotlight

A ring-3 app, `/sbin/spotlight.aex`, not a kernel overlay: the file walk
stays off the compositor thread, the per-window IME makes CJK filenames work
for free, `aui_textfield` is reused, and a crash does not take the desktop.

- Kernel: `WINS_SET_PANEL = 9` (no titlebar, centred at H/4, on top, excluded
  from Cmd+Tab/Exposé/cascade; `TBH` uses become `win_tbh(w)`); focus loss
  hides rather than closes; Cmd+Space shows or launches it;
  `SYS_OPEN_PATH_ARG` passes one argument through `launch_for_ext`.
- App: sources are apps (`*.aex`, named as `scan_apps` names them), files (a
  background index: ≤20k entries, depth ≤8, skipping `/proc` and `/dev`), and
  settings (a static table — toggles act directly, panes open Settings on
  `pane=<id>`). Ranking is a pure fuzzy scorer. The last row is always "Ask
  Assistant: <query>", which starts the assistant with its goal prefilled.
- Gates `test-spotlight-rank` (host, mutation control) and `test-spotlight`
  (QMP: `term`⏎ launches Terminal; a fixture filename opens; the assistant
  row reaches the assistant; control drops the file source).

## P5 — app menus

- `SYS_GUI_MENU_SET = 201`, an `inbuf` blob: versioned header, ≤6 menus of
  ≤24 items `{id, SEP/DISABLED/CHECKED, shortcut hint, label}`, ≤4 KiB.
  Invalid blobs are refused with a serial reason.
- The fixed tables become accessors over the focused app's copy, and every
  reader — layout, draw, hit test, enable, fire, dropdown, mouse — goes
  through them. "LogitOS" stays kernel-owned; an app that publishes nothing
  falls back to File/View. Choosing an app item sends `EV_MENU = 12`.
- aui: `aui_menu_publish`, `aui_menu_pick`. Files, TextEdit, Terminal and
  Browser migrate, each item calling the function its existing shortcut
  calls. Shortcut text is a hint only; no kernel-side routing.
- Gates `test-menu-blob` (host parser over a malformed corpus, asserting the
  return code — never an ASan abort) and `test-menus` (TextEdit → Edit →
  Select All takes effect; control drops the event).

## T1 — terminal wrap, `#!`, statvfs/df/du

- Terminal's line builder moves to a host-testable header and wraps on
  cells, at character boundaries; backspace removes one code point; tabs
  count cells. Gate `test-term-wrap`; control: byte wrap.
- `execve` reads the first 127 bytes after the `MAY_EXEC` check; `#!` yields
  interpreter + one optional argument + path + original argv[1..]. One level;
  the interpreter must be a binary. Gate `test-shebang`; control
  `EXEC_NEGCTL_NO_SHEBANG`.
- `statfs` appended at the END of the op tables (positional initialisers);
  logitfs counts its bitmap under its lock, ramfs reports its budget, synthetic
  filesystems report zero and read-only. `SYS_STATVFS = 202`, libc stub
  replaced, `/proc/mounts`, `df`, `du`. Gate `test-statvfs` (`df /` equals the
  mkfs geometry); control: the `ENOSYS` build.

## T2 — a regex engine, grep, sed

- `c/apps/coreutils/text/lre.{h,c}`: a non-recursive Pike VM, linear in the
  input, BRE and ERE, leftmost-longest overall match, `\1`–`\9` in sed
  replacements, pattern backreferences refused by name. libc's `regex.c`
  recurses with line length; it is left alone for now, and **the two engines
  are a rule-3 hazard to converge later** — recorded here so it is not
  discovered.
- grep: `-E -F -i -v -c -n -l -o -w -r -q -e`. sed: `s///gpNI`, addresses and
  ranges, `d p q a i c y =`, `-n -e -f -E -i`.
- Build: `tests/textutils.mk` with a rule that links `LIBC_OBJS` (plain
  `CLI_RULE` does not), wired into the root Makefile and proved by
  `test-mk-wired`.
- Gates: `test-textutils-host` (native build of the same sources, `LC_ALL=C`,
  case files tagged posix/gnu/bsd; GNU-only cases skip loudly without
  `gsed`/`ggrep`) and `test-textutils` (the same corpus in the guest against
  host-made goldens). Control: a non-greedy `lre` must fail both.

## T3 — sort uniq tail cut tr find diff kill ln chmod

POSIX first, GNU where POSIX is silent (`uniq -c` is `%7d`). `diff` is Myers
and is checked by applying its output with host `patch` — byte equality would
reject legal hunk choices. `find` output is sorted before comparison. `kill`,
`ln`, `chmod` wrap existing syscalls. Control: an unstable `sort` must fail
the `-k` stability cases.

## Out of scope

Multi-window per process, `pwrite` / writable file mappings, italic, full-
screen TUIs (vi, less), a pty/VT100 Terminal, FAT, drag-and-drop between
apps, a notification history.

## Done means

Track A's end re-runs `test-repaint` into a before/after table (fps, input
latency, full frames) with drag/type/scroll ≥30 fps at 1920×1200, and a
`make run` walkthrough: snap a large window, hold Cmd+Tab, Cmd+Space `term`⏎,
Edit → Select All in TextEdit, close a window; in the Terminal, a long CJK
line, `grep -rn foo / | sort | uniq -c`, `df -h`, and a `#!` script.
CLAUDE.md's stale paragraphs are corrected beside the old claims, dated.
