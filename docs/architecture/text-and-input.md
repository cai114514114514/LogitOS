# Text and input

LogitOS routes keyboard and pointer input through the window manager, which handles desktop controls, selects recipients, and queues application events. The pinyin input method participates in keyboard dispatch and delivers committed characters through the same event ABI. The kernel text layer loads fonts, shapes and draws text, and supplies measurements and caret positions to application layout helpers. The main integration points are `c/kernel/gui/wm.c`, `c/kernel/gui/text.c`, and `c/kernel/gui/ime/ime_ui.c`.

## From devices to event queues

The PS/2 keyboard driver in `c/drivers/char/keyboard.c` decodes scancode set 1 using US QWERTY tables. It tracks held keys and modifiers, translates navigation and function keys into ABI codes, and folds Ctrl with a letter into a control character. Presses, releases, and modifier changes reach the window manager through `wm_key()`, `wm_key_up()`, and `wm_mods_changed()`.

The PS/2 mouse driver in `c/drivers/char/mouse.c` assembles packets, integrates movement into screen coordinates, and publishes button levels and wheel movement through `wm_mouse_event()`. The window manager derives button transitions because it owns the window and capture state.

USB HID uses the same keyboard and pointer entry points through `c/drivers/usb/usb_hid.c`. Parsed keyboard reports update held state and modifiers before publishing new presses. Repeated reports with the same held keys drive key repeat. Pointer decoding handles relative and absolute axes before publishing screen positions.

Two queues connect these producers to applications:

| Queue | Contents | Capacity and processing |
|---|---|---|
| Raw input queue | Pointer state, wheel movement, key presses, releases, and modifier changes | 512 slots, 511 usable; the desktop drains at most 16 events per pass |
| Event queue for each window | Application events represented by `struct logit_event` | 256 slots, 255 usable; applications retrieve events through system calls |

The raw queue is implemented in `c/kernel/gui/input/input_queue.c`; window event queues are implemented in `c/kernel/gui/input/evq.c`. Each queue has its own lock.

Deferral places window operations in desktop thread context. A click can launch a program, allocate memory, or change window geometry. The deferral comment in `c/kernel/gui/wm.c` records allocator deadlocks and races with compositing caused by doing this work from device interrupts. Producers now enqueue input, and the desktop processes it alongside its other work.

Both queues merge adjacent pointer motion samples. The raw queue preserves boundaries formed by keys, button transitions, and wheel movement; a window queue merges consecutive `EV_MOUSE_MOVE` events. When a queue is full, an incoming event other than motion can evict an older motion sample. The fixed drain budget gives rendering, animation, and scheduler work an opportunity between input batches.

## USB keyboard setup and fallback

`c/drivers/usb/usb_hid.c` uses parsed report layouts when available and selects boot protocol for the boot fallback. Parsed keyboard interfaces use the device’s default report protocol; parsed interfaces in the boot subclass that have no keyboard role receive a request for report protocol.

Before arming keyboard input, setup requests idle reports every 96 ms and sends a report with all LEDs off when a suitable output report is available. The source records LED initialization as a compatibility step for keyboards that wait for it before sending input. `c/drivers/usb/hid_report.c` finds the output report’s ID and size. The parser’s field budget is 320, accommodating keyboard descriptors that assign a variable bit to each key, as explained beside `HID_MAX_FIELDS` in `c/drivers/usb/hid_report.h`.

After three seconds of interrupt silence, `hid_service()` can query keyboard state through control transfers. A decodable response activates polling with a requested interval of 16 ms. The USB core schedules service through its timer, so the effective interval follows that timer’s ticks.

Changed reports enter the keyboard decoder immediately. Unchanged reports are fed at 96 ms intervals when their bytes are nonzero, preserving the repeat cadence. An interrupt report on the interface retires the fallback; a service pass also retires it when a sibling HID interface has delivered an interrupt report. Control transfers run on a worker, with decoder updates serialized against interrupt callbacks. These mechanisms are in `c/drivers/usb/usb_hid.c` and `c/drivers/usb/usb_core.c`.

## Focus and application delivery

Focus selection, desktop shortcuts, and application delivery meet in `c/kernel/gui/wm.c`. `wm_keyboard_focus()` selects the greeter while the desktop is locked and otherwise starts from the top visible window. Desktop menus and overlays can take keyboard ownership. Keyboard dispatch processes desktop interfaces and shortcuts before invoking the IME or delivering a key to an application.

After each raw event, the desktop synchronizes focus. Application windows receive `EV_WINDOW_FOCUS`, with `a` equal to one for focus and zero for loss of focus. Synchronization also runs before rendering, covering focus changes caused by animations or other window activity.

Pointer routing uses window geometry and capture state. Left and right presses in application content raise the recipient; middle presses preserve stacking order. Application coordinates are relative to the content area and expressed in points. Motion and wheel events reach the window under the pointer or the capture target. Capture directs button releases to its window, allowing coordinates outside the content bounds during a drag.

`struct logit_event` in `include/abi/logit_abi.h` contains the event type, payload fields `a` and `b`, modifiers, button identity, and wheel movement. For `EV_KEY`, `a` carries a character code point or a command key code. Applications must retain that value as an integer until they classify it. TextEdit uses `logit_key_is_command()` for navigation and function keys, then encodes accepted text into UTF-8 in `c/apps/gui/textedit/textedit.c`.

Applications retrieve events through `SYS_POLL_EVENT` or wait through `SYS_WAIT_EVENT`. The wrappers `poll_event()` and `wait_event_ms()` are in `c/apps/logit.h`. The kernel resolves the calling application’s window, checks its queue, and sleeps on a wait queue when requested. Appending an event wakes one waiter.

Applications enabling `WINS_WANT_KEYUP` receive releases and modifier press and release events from the PS/2 path. Printable releases carry the unshifted key identity, allowing applications to identify the released key after modifier state changes.

## Pointer presentation and diagnostics

The window manager uses a hardware cursor plane when available. For CPU framebuffer presentation, the software overlay in `c/kernel/gui/fb/fb.c` and `c/kernel/gui/fb/fb_overlay.c` restores the previous pointer box from the RAM composite and presents the new box with the sprite.

The desktop publishes the software overlay’s position and shape after input processing, acquiring the graphics lock before the window manager state lock. The comment beside cursor shape handling in `c/kernel/gui/wm.c` explains the reason: this matches GUI syscall lock order and avoids a deadlock between input processing and application drawing. Movement that changes desktop content, such as Dock hover, generates compositor damage.

HID request outcomes, report counts, decoder results, and fallback state are held in static diagnostic slots described by `c/drivers/usb/hid_diag.h`. The hardware panel enabled by `make HWHUD=1` displays them through `c/kernel/diag/hwhud.c`. Its title row accepts a left click to collapse or expand the panel; the window manager consumes the press and following release. The panel yields screen space to system interfaces, including the IME popup.

Window manager performance reporting snapshots counters under its state lock and prints after releasing it. Serial transmission in `c/drivers/char/serial.c` queues bytes in memory and drains available UART FIFO space, including from periodic timer calls.

The scheduler observation facility supplies run control and record export through `SYS_SCHED_OBSERVE`. Its storage for input outcomes and coalesced input groups lives in `c/kernel/sched/observe/input_ledger.c` and `c/kernel/sched/observe/coalesced_inputs.c`; its integration status is listed under limitations below.

## Fonts and glyph rendering

`text_init()` in `c/kernel/gui/text.c` loads fonts from the mounted filesystem. Parsed font objects reference the loaded bytes, so those bytes remain resident. Text requests construct a font preference list, and the shaper chooses the first face containing a glyph for each code point.

| Repository asset | Role |
|---|---|
| `fsroot/fonts/ui.ttf` | Proportional UI text with Latin and CJK coverage |
| `fsroot/fonts/ui-medium.ttf`, `fsroot/fonts/ui-semibold.ttf`, `fsroot/fonts/ui-bold.ttf` | UI weights 500, 600, and 700 |
| `fsroot/fonts/mono.ttf`, `fsroot/fonts/mono-bold.ttf` | Latin text on a fixed grid |
| `third_party/fonts/DejaVuSans.ttf` | Installed as `/fonts/text.ttf`; supplies fallback glyphs and layout tables for scripts including Arabic and Hebrew |

`Makefile` defines the installation mapping. `tools/mkfont.py` generates the UI and mono subsets from vendored Noto sources. UI faces retain GPOS kerning and GDEF; mono faces drop layout tables. The kernel maps UI weight requests to the nearest baked weight among 400, 500, 600, and 700, rounding ties upward.

`c/lib/text/ttf.c` reads font tables, maps code points to glyphs, and exposes metrics and outlines. It handles TrueType outlines and delegates CFF outlines to `c/lib/text/cff.c`. `c/lib/text/glyphras.c` converts outlines into paths and obtains coverage masks from the shared rasterizer. The kernel caches masks by font, glyph, pixel size, and horizontal phase, then blends them into the drawing target.

Font fallback also serves terminal text. Latin characters can use the mono face while Han characters use the UI face. The grid path in `c/lib/text/shape.c` assigns one or two cells according to the glyph’s scaled advance.

## Bidi and shaping

For proportional text, `shape_line()` in `c/lib/text/shape.c` performs five stages:

1. Decode UTF-8 into code points.
2. Resolve bidi embedding levels.
3. Divide text into runs sharing a script, direction, and font.
4. Arrange runs in visual order.
5. Shape each run and scale its glyph advances and offsets.

`c/lib/text/bidi.c` handles embeddings, overrides, isolates, weak and neutral characters, paired brackets, and visual reordering. `c/lib/text/script.c` supplies script classification, run segmentation, and Arabic joining forms.

`shape_run()` applies OpenType substitutions for contextual forms and ligatures, followed by positioning for kerning, cursive attachment, and marks. Table readers live in `c/lib/text/otlayout.c`. Legacy kerning is enabled when a font has a legacy kerning table and lacks a successfully opened GPOS table with lookups. This prevents applying both kerning mechanisms to the same text.

Kernel measurement and drawing use this shared shaping pipeline through `c/kernel/gui/text.c`, so layout can account for ligatures and kerning. Proportional drawing uses fractional glyph placement and returns the advance from an integer measurement pass.

## Paragraph layout and carets

Paragraph helpers run in applications. `Makefile` filters `c/lib/text/linebreak.c` and `c/lib/text/para.c` out of the kernel source list and builds them into the text library linked through GUI application support.

`tl_break_utf8()` computes break opportunities through `c/lib/text/linebreak_core.inc`, whose rules follow UAX #14 revision 53 for Unicode 16.0. The browser includes the same core from `c/apps/gui/browser/layout/text.c` and adds CSS handling.

`tl_para_layout()` greedily fills lines at available break opportunities. It supports left, center, and right alignment, a maximum line count, and ellipsis at the beginning, middle, or end. An overlong word can break at a supplied caret boundary. Output lines identify byte ranges in the original UTF-8 string, widths, alignment offsets, and any range replaced by an ellipsis.

The caller supplies a measurement callback. In `c/apps/gui/aui_text.inc`, that callback uses the `SYS_TEXT_MEASURE` caret query to obtain UTF-8 byte offsets and horizontal positions. `text_measure_carets()` in `c/kernel/gui/text.c` derives positions from bidi runs, glyph clusters, and advances, grouping several kinds of combining sequences into caret stops.

## Pinyin composition

The dictionary engine lives in `c/lib/ime/pinyin.c`; window ownership, key handling, and the candidate popup live in `c/kernel/gui/ime/ime_ui.c`. Placing the IME in keyboard dispatch gives applications a common route for receiving committed text.

Initialization prefers `/ime/pinyin-qwen.dat` and falls back to `/ime/pinyin.dat`. `ime_open()` validates dictionary structure and builds an index over resident bytes. Candidate generation combines exact dictionary matches, joins of dictionary words, syllable segmentation, spelling extensions, initials abbreviations, and selectable spelling prefixes. Candidate classes establish priority; frequency scores and learned weights influence ordering within classes.

Each window has an enabled state and a parked spelling and page. Focus changes save the outgoing composition and reconstruct the incoming composition by replaying its spelling. Closing a window clears its record before the window slot is reused.

`IME_TOGGLE_MOD` and `IME_TOGGLE_NAME` in `c/kernel/gui/ime/ime_ui.h` define Shift+Space as the toggle. The header records why the binding changed from Ctrl+Space: macOS consumes that chord before QEMU receives it.

With pinyin enabled, a lowercase letter starts composition. While a composition is open:

| Input | Effect |
|---|---|
| Lowercase letter | Extend the spelling |
| Apostrophe | Constrain syllable boundaries |
| Space | Accept the first visible candidate, or raw spelling when the candidate list is empty |
| Digit 1–9 | Select a candidate on the current page |
| Left, Page Up, or minus | Previous candidate page |
| Right, Page Down, or equals | Next candidate page |
| Enter | Commit raw spelling |
| Backspace | Remove the last spelling character |
| Escape | Cancel composition |

The popup displays up to nine candidates per page and accepts mouse selection. Selecting a candidate can consume a spelling prefix while keeping the suffix open.

Supported punctuation confirms pending composition and appends Chinese punctuation. It also produces Chinese punctuation when pinyin is enabled with an empty composition. Other printable characters encountered during composition commit the raw spelling followed by that character. Ctrl, Alt, and Command combinations pass through the IME handler while preserving composition.

Committed text reaches the application as one `EV_KEY` per code point with modifiers cleared. This prevents a modifier held during composition from becoming part of the resulting character event.

## Learned candidate weights

`c/kernel/gui/ime/ime_learn.c` maintains a shared table of selections identified by dictionary key and candidate UTF-8 text. Text identities keep entries meaningful when dictionary regeneration changes record offsets.

Selections attributable to a dictionary entry increment a count, which contributes a capped ranking bonus. Aging makes room for new entries by halving counts, removing entries that reach zero, and compacting storage.

A learned selection rearms a three-second quiet timer. Its callback queues a worker that snapshots the table and writes `/var/ime-learn.conf`. Toggling pinyin off or closing a window requests an earlier write. The table lock protects memory operations; allocation and filesystem I/O occur outside it.

## Limitations

- Both event queues can drop input when full. Motion eviction helps preserve other events; each implementation counts losses.
- Keyboard translation uses US QWERTY mappings. USB keyboards update held state and emit presses, but the USB keyboard handler does not publish key releases or modifier edges.
- Keyboard polling through control transfers is restricted to interfaces advertising boot keyboard subclass and protocol, with reports of at most 64 bytes. A failed or undecodable response ends polling for that interface.
- Scheduler observation storage is unconnected to the raw and window event queues. `SYS_GUI_RECEIPT` remains reserved and falls through to window manager handling in `c/kernel/exec/syscall.c`.
- The shaper lacks Indic, Khmer, and Myanmar syllable reordering, Unicode normalization, and vertical writing, as documented in `c/lib/text/shape.h`. Terminal grid rendering bypasses bidi resolution and shaping.
- Paragraph layout rejects caret positions that decrease in logical order and uses static scratch for one caller at a time. Kernel caret grouping omits spacing combining marks and Hangul jamo sequences.
- Command key values overlap Unicode code points in `include/abi/logit_abi.h`. The IME restricts dictionary candidate delivery to ASCII and U+4E00–U+9FFF; its punctuation conversion is an additional delivery path.
- Shift+Space consumes the space and can toggle pinyin when Shift remains held after a capital. Learned selections awaiting their asynchronous write can be lost on power failure.

## Source map

| File | What it contains |
|---|---|
| `c/drivers/char/keyboard.c`, `c/drivers/char/mouse.c` | PS/2 decoding, held keys, modifiers, pointer state, and wheel input |
| `c/drivers/usb/usb_hid.c` | HID setup, keyboard and pointer publication, repeat, and control polling |
| `c/drivers/usb/hid_report.c`, `c/drivers/usb/hid_report.h` | Report parsing, decoding, field budgets, and LED report discovery |
| `c/drivers/usb/usb_core.c` | Worker service scheduling and callback serialization |
| `c/drivers/usb/hid_diag.h`, `c/kernel/diag/hwhud.c` | HID diagnostics and hardware panel interaction |
| `c/drivers/char/serial.c` | Queued serial transmission and periodic draining |
| `c/kernel/gui/input/input_queue.h`, `c/kernel/gui/input/input_queue.c` | Raw queue capacity, coalescing, overload handling, and bounded draining |
| `c/kernel/gui/input/evq.h`, `c/kernel/gui/input/evq.c` | Window event queues and motion coalescing |
| `c/kernel/gui/wm.c` | Focus, routing, capture, IME integration, pointer synchronization, and event system calls |
| `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.c` | Software pointer overlay and presentation |
| `include/abi/logit_abi.h`, `c/apps/logit.h` | Event ABI, key codes, modifiers, and application wrappers |
| `c/kernel/sched/observe/observe_control.c`, `include/abi/logit_sched_observe.h` | Observation run control, export, and record ABI |
| `c/kernel/sched/observe/input_ledger.c`, `c/kernel/sched/observe/coalesced_inputs.c` | Storage for input outcomes and coalesced groups |
| `c/kernel/exec/syscall.c` | Observation dispatch and GUI receipt integration status |
| `c/apps/gui/textedit/textedit.c` | Command classification and UTF-8 insertion |
| `c/kernel/gui/text.c`, `c/kernel/gui/text_measure_dispatch.inc` | Fonts, glyph caching, drawing, measurements, and carets |
| `c/lib/text/ttf.c`, `c/lib/text/cff.c`, `c/lib/text/glyphras.c` | Font parsing, outlines, and coverage masks |
| `c/lib/text/bidi.c`, `c/lib/text/script.c` | Direction resolution, script runs, and joining properties |
| `c/lib/text/shape.h`, `c/lib/text/shape.c`, `c/lib/text/otlayout.c` | Shaping contracts, substitution, positioning, and glyph placement |
| `c/lib/text/linebreak_core.inc`, `c/lib/text/linebreak.c` | Shared line breaking rules and UTF-8 wrapper |
| `c/lib/text/para.h`, `c/lib/text/para.c`, `c/apps/gui/aui_text.inc` | Paragraph contracts, layout, and application integration |
| `c/apps/gui/browser/layout/text.c` | Browser line breaking and CSS text handling |
| `c/lib/ime/pinyin.h`, `c/lib/ime/pinyin.c` | Dictionary validation, candidate generation, ranking, and prefix consumption |
| `c/kernel/gui/ime/ime_ui.h`, `c/kernel/gui/ime/ime_ui.c` | Toggle, window composition state, popup, and commits |
| `c/kernel/gui/ime/ime_learn.h`, `c/kernel/gui/ime/ime_learn.c` | Learned weights, aging, and persistence |
| `tools/mkfont.py`, `Makefile` | Font generation, asset installation, application text linkage, and hardware panel build option |

## See also

- [Graphics and windowing](graphics-and-windowing.md)
- [GUI events, clipboard and notifications](../kernel/gui-events-and-services.md)
- [Keyboard, mouse, serial and VGA](../drivers/input-and-console.md)
- [USB](../drivers/usb.md)