# GUI events, clipboard and notifications

LogitOS delivers keyboard and pointer input through the window manager into application event queues. The kernel also owns the clipboard, displays notification cards, and manages pinyin composition and its candidate popup. `c/kernel/gui/wm.c` connects these services by choosing input recipients, delivering committed characters, and composing system UI above application windows.

## Input delivery

Input travels through two queues: a raw device queue and a window event queue. Interrupt handlers record input; the desktop thread performs routing, window changes, and application launch. The input-deferral comment in `c/kernel/gui/wm.c` records the reason: handling these operations inside interrupts raced with rendering and could enter allocation or disk operations while interrupted code held their locks.

The entry points `wm_key()`, `wm_key_up()`, `wm_mods_changed()` and `wm_mouse_event()` append input with its current modifier mask to `struct input_queue`, defined in `c/kernel/gui/input/input_queue.h`. Pointer reports carry absolute screen coordinates, button levels, and wheel movement. The queue’s kernel lock saves and restores interrupt state while serializing producers and the consumer.

Adjacent motion reports with matching button levels merge into the latest position. Keys, wheel reports, and button transitions form boundaries between motion runs. Under pressure, the queue can remove an older motion report to admit one of those events while preserving the order of the remaining entries. Button tracking follows the latest accepted report, allowing later hardware reports to retry a release whose first submission encountered a full queue. These policies are implemented in `c/kernel/gui/input/input_queue.c`.

`wm_drain_input()` processes at most `INPUTQ_DRAIN_BUDGET`, currently 16 events, per desktop pass. A backlog starts another pass after resize, network, animation, and rendering work have had an opportunity to run. This bounds the input batch ahead of a frame during continuous pointer activity.

After the batch, the desktop loop acquires the graphics lock followed by the WM state lock and calls `wm_pointer_sync()`. This publishes the latest cursor position and software overlay shape. A display cursor plane or a supported software overlay lets cursor movement proceed without desktop recomposition. The software path presents the old and new cursor rectangles from the desktop buffer, using `c/kernel/gui/fb/fb.c` and `c/kernel/gui/fb/fb_overlay.c`.

## Routing and application events

Keyboard dispatch in `c/kernel/gui/wm.c` handles active system interfaces and system shortcuts first, followed by application menu shortcuts, IME processing, and delivery to the focused application. `wm_keyboard_focus()` supplies the recipient. `wm_sync_focus()` publishes ownership changes through `EV_WINDOW_FOCUS`, with `a` equal to one for gained focus and zero for lost focus. Synchronization runs between raw events so that a window switch takes effect before the next queued key.

Application content presses establish pointer capture. Subsequent application motion, scrolling, and releases use that target until all buttons are up. Coordinates can extend beyond the content bounds during a drag. Without capture, motion reaches the content under the pointer, supporting hover feedback in an unfocused window. Left and right content presses raise their recipient; middle presses act on the content under the pointer.

Applications receive `struct logit_event` from `include/abi/logit_abi.h`:

| Field | Meaning |
|---|---|
| `type` | Event kind |
| `a`, `b` | Payload, such as key identity, coordinates, or dimensions |
| `mods` | Modifier mask associated with the input |
| `button` | Button identity for pointer presses and releases |
| `wheel` | Scroll movement in notches |

Application pointer coordinates use points relative to the content area. `EV_MOUSE` represents left or middle presses, distinguished by `button`; `EV_MOUSE_R` represents a right press. `EV_MOUSE_UP`, `EV_MOUSE_MOVE` and `EV_WHEEL` carry releases, motion, and scrolling. Window services also enqueue close, theme, resize, focus, and menu events.

The WM routes key releases and modifier edges to windows that enable `WINS_WANT_KEYUP`. `EV_KEY_UP` carries the released key’s unshifted identity and the modifiers held after release. Modifier presses arrive as `EV_KEY`. The ABI is in `include/abi/logit_abi.h`, and the PS/2 event producer is in `c/drivers/char/keyboard.c`.

## Window queues and waiting

Each application window contains a `struct evq` and a wait queue, declared in `c/kernel/gui/wm_internal.h`. The ring implementation is in `c/kernel/gui/input/evq.c`.

Consecutive unread `EV_MOUSE_MOVE` events merge by updating the pending position and modifiers. This keeps the application’s pending pointer position current while it renders. When a full ring receives another event type, it can evict an older motion entry while preserving the remaining order.

A lock protects each ring. After an event appends a slot, `wm_enqueue()` or `enqueue_input()` releases the ring lock and wakes one waiter. Each appended slot gets a wake opportunity, including when earlier events already make the queue readable. This supports multiple threads waiting on the same window. A coalesced motion update reuses an existing slot.

The application interfaces in `c/apps/logit.h` are:

| Interface | Behavior |
|---|---|
| `poll_event()` | Attempts to consume one event immediately |
| `wait_event_ms()` | Consumes an event, sleeping when the queue is empty |
| `wait_idle()` | Waits for availability and leaves the event queued |

These wrap `SYS_POLL_EVENT` and `SYS_WAIT_EVENT`. A positive timeout bounds the wait; zero requests a wait without a timeout. Waiting on the window queue lets an idle application sleep until input arrives.

The backend in `c/kernel/gui/wm.c` also wakes consumers during application shutdown. An event syscall holds a reference to its window slot, and teardown waits for those references before recycling the slot.

## Clipboard

The clipboard is a kernel store shared by processes. A successful set copies the payload into kernel memory, allowing another process to paste it after the source process exits. `CLIP_Q_OWNER` reports the last setter’s PID as metadata. This lifetime contract is documented in `include/abi/logit_abi.h` and implemented in `c/kernel/gui/clipboard.c`.

Four flavour identifiers describe representations of a selection:

| Flavour | Intended representation |
|---|---|
| `CLIP_F_TEXT` | UTF-8 plain text |
| `CLIP_F_HTML` | UTF-8 HTML fragment |
| `CLIP_F_URI` | UTF-8 absolute URL |
| `CLIP_F_PATH` | UTF-8 absolute filesystem path |

A set replaces the selection’s representations. `CLIP_SET_ADD` adds or replaces one flavour alongside the current representations, allowing an application to publish several forms of the same selection. Each successful set increments the serial number and records the setter’s PID. A zero-length payload counts as a present flavour.

For `CLIP_F_TEXT`, validation examines the copy in kernel memory. This checks the bytes that will be published even when another thread changes the caller’s buffer during the operation. Text reads return a prefix ending at a UTF-8 character boundary. Getters return a byte count; callers needing a C string append its terminator. Reading an absent flavour returns `CLIP_E_EMPTY`.

Published payloads are immutable and reference counted. A getter retains the selected payload under the store lock, releases the lock, and copies the bytes to the caller. Replacement can publish new content while an existing reader finishes copying the previous payload. Allocation and access to user memory happen outside the store lock.

`SYS_CLIP_SET`, `SYS_CLIP_GET` and `SYS_CLIP_INFO` reach the store through `c/kernel/exec/syscall.c`, allowing graphical and command-line processes to use it. Wrappers in `c/apps/logit.h` expose set, add, get, flavour discovery, length, serial, owner, and capacity queries. TextEdit uses these interfaces for copy, cut, and paste in `c/apps/gui/textedit/textedit.c`.

## Notifications

`SYS_NOTIFY` accepts a title, body, and severity. Applications call `notify()` from `c/apps/logit.h`; kernel callers use `notify_post()` from `c/kernel/gui/notify.h`. Both paths copy strings into the notification store. A successful post returns a positive ID, and keyboard focus stays with its current recipient.

`c/kernel/gui/notify.c` maintains a notification pool and an array of visible slots. Up to `NOTIFY_VISIBLE`, currently three cards, appear below the menu bar at the right edge. A queued card receives its `NOTIFY_MS_DEFAULT` lifetime of four seconds when promoted to a visible slot. A left press on a card closes it, moves lower cards upward, and promotes queued cards. The window manager consumes the dismissal press before application content handling.

The presentation comment in `include/abi/logit_abi.h` says “newest at the top.” The implementation displays oldest first: `promote()` selects the pending notification with the lowest ID, and `close_slot()` compacts the visible stack upward.

A private GUI mutex protects notification state. `notify_tick()` performs expiry during the desktop loop, and `notify_compose()` draws the cards. Changes damage the whole notification column, including shadows, because removing one card can reposition several others.

The default presentation uses appearance and disappearance. A comment in `c/kernel/gui/notify.c` records the comparison behind this choice: on 2026-08-08, QEMU/TCG at 1920×1200 and scale 150 measured five additional composites for three notifications through expiry, versus 26 with the optional slide and fade entrance. Those historical figures subtract a control that launches the corresponding commands.

## System overlays and the hardware HUD

Notification dismissal and IME popup selection take priority over application content presses. In builds made with `make HWHUD=1`, the hardware HUD also handles presses on its title row, between notification dismissal and IME selection.

`hwhud_click()` in `c/kernel/diag/hwhud.c` toggles the HUD between its full panel and title row, then requests repaint of the previous rectangle. The WM consumes subsequent pointer reports through the left button release, keeping that gesture within the HUD.

The HUD yields to notification cards and the IME candidate popup. `wm_system_overlay_visible()` in `c/kernel/gui/wm.c` also recognizes About and power overlays, open menus, the window switcher, Exposé, and visible application panel windows. The desktop loop samples this state under the WM state lock and calls `hwhud_tick()` after releasing it. The policy in `c/kernel/diag/hwhud_panel.h` requests repaint of the HUD’s previous rectangle when system UI appears and restores the panel after that UI closes.

## IME UI

The pinyin UI lives in `c/kernel/gui/ime/ime_ui.c` and uses the dictionary and candidate engine in `c/lib/ime/pinyin.c`. Integration at the WM keyboard dispatch point gives applications a common input method through their existing event queues.

`ime_ui_init()` prefers the guest dictionary `/ime/pinyin-qwen.dat` and falls back to `/ime/pinyin.dat`. Their repository assets are `fsroot/ime/pinyin-qwen.dat` and `fsroot/ime/pinyin.dat`. The loader reads the dictionary in chunks, validates its format, and retains the backing bytes for the engine’s index.

`IME_TOGGLE_MOD` and `IME_TOGGLE_NAME`, defined in `c/kernel/gui/ime/ime_ui.h`, specify Shift+Space. The header records the earlier Ctrl+Space binding and its delivery problem on macOS hosts. A clickable 中/EN indicator is implemented by `c/kernel/gui/wm_menubar.c` and the pointer dispatcher in `c/kernel/gui/wm.c`.

Enablement belongs to each window. One engine state holds the active composition; parked records retain each window’s spelling and candidate page. Focus changes save the outgoing record and reconstruct the incoming composition by replaying its spelling. Window teardown clears the associated state. Toggling the IME off clears that window’s composition.

The following controls apply while the IME is enabled, after system shortcuts have been handled:

| Input | Action |
|---|---|
| Lowercase letters | Start or extend composition |
| Uppercase letters before composition starts | Pass through to the application |
| Apostrophe during composition | Add a syllable separator |
| Space during composition | Select the first candidate on the current page, or commit raw spelling when the candidate list is empty |
| Digits 1–9 during composition | Select the corresponding candidate on the current page |
| Enter during composition | Commit raw spelling |
| Backspace during composition | Edit spelling |
| Escape during composition | Cancel composition |
| Left/Right, Page Up/Page Down, `-`/`=` during composition | Change candidate page |
| Candidate click | Select the displayed row |
| Comma, period, `!`, `?`, `:`, `;`, `(` or `)` | Commit pending composition, then emit the mapped Chinese punctuation |
| Other printable ASCII during composition | Commit raw spelling followed by that character |

Selecting a candidate can consume a prefix and leave its suffix editable. Ctrl, Alt, and Cmd combinations reaching the IME pass onward with composition preserved. The older desktop description in `CLAUDE.md` says these combinations cancel composition; the current policy and regression assertions are in `c/kernel/gui/ime/ime_ui.c` and `tests/unit/ime_ui_test.c`.

`ime_ui_key()` returns `-1` for forwarding, zero for consumed input without committed text, or a positive count of committed code points. The WM delivers committed characters as `EV_KEY`, with the code point in `a` and zero modifiers. Application text handlers must retain the integer code point until encoding it.

The popup shows spelling and up to nine numbered candidate rows. Layout and hit testing share row geometry. Changes damage both previous and current popup bounds, including shadows. `render_top()` draws the popup after application layers and before notifications.

Selections attributable to dictionary entries feed the ranking store in `c/kernel/gui/ime/ime_learn.c`. Learning updates memory during input processing. A timer queues worker activity to persist a snapshot after a quiet period; toggling the IME off and closing a window request an earlier flush.

## Diagnostics and checks

The WM exposes counts for queued events, merged motion, evicted motion, and drops. Raw input statistics distinguish motion loss from other event loss and record maximum queue depth and batches that leave a backlog.

`wm_perf_report_collect()` in `c/kernel/gui/wm.c` captures a performance sample under the WM state lock; `wm_perf_report_print()` prints it after releasing the lock. Reports include software cursor work. Cumulative lock statistics are printed for tags whose call counts have changed.

`SYS_SCHED_OBSERVE` (224) reaches `observe_syscall()` in `c/kernel/sched/observe/observe_control.c`. A caller with user ID zero can start and stop a measurement run, read status, and export scheduler traces, input records, and coalesced groups through the ABI in `include/abi/logit_sched_observe.h`. The ledger and group archive provide storage for payloads, deadlines, outcomes, and group membership. The HUD formats a status snapshot through `c/kernel/sched/observe/observe_hud.c`.

The following test sources define checks for these services:

| Check source | Coverage |
|---|---|
| `tests/unit/input_queue_test.c` | Motion floods, button release admission and retry, ordering, and bounded batches |
| `tests/unit/evq_test.c` | Window queue ordering, motion coalescing, overflow, and wraparound |
| `tests/unit/clipboard_test.c` | UTF-8 validation, cuts throughout a mixed-width string, flavours, capacity, and allocation failure |
| `tests/unit/ime_ui_test.c` | Focus restoration, shortcuts, partial commits, punctuation, and candidate clicks |
| `tests/qmp/qmp_notify.py` | Guest presentation, expiry, typing through cards, dismissal, and stale pixels |

## Limitations

- The raw ring holds 511 pending entries and a window ring holds 255. Full rings can lose incoming events; both implementations count losses.
- The USB keyboard path in `c/drivers/usb/usb_hid.c` emits presses and updates held-key and modifier state, but lacks calls to `wm_key_up()` and `wm_mods_changed()`. Key-release and modifier-edge delivery depends on the device path.
- Clipboard payloads have a 64 KiB limit per flavour. Oversized sets return `CLIP_E_TOOBIG` and attempt to post a warning notification. UTF-8 validation and character-boundary reads apply to `CLIP_F_TEXT`; other flavours use byte storage.
- The notification ABI comment says “Nothing is dropped.” The implementation has a 12-entry pool covering visible and queued cards, and returns zero when it is full.
- Notification title and body buffers hold 48 and 120 bytes including terminators. The syscall returns `-1` for invalid user memory or strings lacking a terminator within those bounds; kernel posting truncates strings to fit.
- Special key values overlap Unicode code points. The IME commit path accepts ASCII and U+4E00–U+9FFF; its punctuation path emits additional fixed characters. General Unicode input requires resolving this ABI collision.
- Shift+Space consumes a shifted space as the IME toggle. If dictionary loading fails, the UI logs the failure and forwards the chord.
- The GUI queues lack hooks into the observation ledger and group archive. `SYS_GUI_RECEIPT` (225) is reserved in `include/abi/logit_abi.h` and falls through to unsupported WM handling. The observation facility currently lacks the GUI integration needed to connect input arrival to a displayed frame.

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_abi.h` | Event layout, event kinds, clipboard calls, notification constants, and service contracts |
| `c/drivers/char/keyboard.c`, `c/drivers/char/mouse.c` | PS/2 input delivery into the WM |
| `c/drivers/usb/usb_hid.c` | USB keyboard and pointer report handling |
| `c/kernel/gui/input/input_queue.h`, `c/kernel/gui/input/input_queue.c` | Raw input transport, coalescing, overflow handling, statistics, and batch limits |
| `c/kernel/gui/input/evq.h`, `c/kernel/gui/input/evq.c` | Window event rings, coalescing, and counters |
| `c/kernel/gui/wm_internal.h` | Window event queue, wait queue, and waiter references |
| `c/kernel/gui/wm.c` | Routing, focus, capture, waiting, diagnostics, and service integration |
| `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.c` | Software cursor presentation and overlay composition |
| `c/kernel/diag/hwhud.c`, `c/kernel/diag/hwhud_panel.h` | HUD drawing, visibility policy, and title row interaction |
| `Makefile` | `HWHUD=1` build option |
| `c/kernel/gui/gui_sync.h` | GUI spin locks and recursive mutexes |
| `c/kernel/gui/clipboard.h`, `c/kernel/gui/clipboard.c` | Shared clipboard, payload lifetime, and UTF-8 handling |
| `c/kernel/gui/notify.h`, `c/kernel/gui/notify.c` | Notification storage, promotion, expiry, drawing, damage, and recorded presentation cost |
| `c/kernel/gui/ime/ime_ui.h`, `c/kernel/gui/ime/ime_ui.c` | Toggle, dictionary loading, composition policy, focus records, and popup |
| `c/kernel/gui/ime/ime_learn.c` | Learned ranking weights and persistence work |
| `c/lib/ime/pinyin.h`, `c/lib/ime/pinyin.c` | Dictionary interface, candidate engine, and prefix acceptance |
| `c/kernel/gui/wm_menubar.c` | IME mode indicator and its hit target |
| `c/kernel/exec/syscall.c`, `c/kernel/exec/usercopy.c` | Service dispatch and copying user memory |
| `include/abi/logit_sched_observe.h` | Observation commands and exported record layouts |
| `c/kernel/sched/observe/observe_control.c` | Measurement run control, status, and paged export |
| `c/kernel/sched/observe/input_ledger.c`, `c/kernel/sched/observe/coalesced_inputs.c` | Input records and coalesced group archive |
| `c/kernel/sched/observe/observe_hud.c` | Observation status formatting for the HUD |
| `c/apps/logit.h` | Application event, clipboard, and notification wrappers |
| `c/apps/gui/textedit/textedit.c` | Clipboard use by a desktop editor |

## See also

- [Window manager](window-manager.md)
- [Compositing and effects](compositing-and-effects.md)
- [Text and input](../architecture/text-and-input.md)
- [System calls](../architecture/system-calls.md)