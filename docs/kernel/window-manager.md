# Window manager

The LogitOS window manager runs in the kernel and owns window geometry, stacking, focus, desktop controls and composition. Applications run in ring 3, draw into canvases allocated by the kernel through GUI system calls, and receive window and input events. `c/kernel/gui/wm.c` coordinates these paths; companion modules implement placement, titlebars, menus, the dock and animations.

## Execution and locking

`wm_init()` allocates desktop buffers, reads appearance settings, initializes the input method and discovers applications for the dock. The boot path in `c/kernel/init/kmain.c` calls `wm_init()`, renders the first frame through `wm_render()`, and later enters `wm_run()`. That entry point initializes the scheduler, starts rendering workers and drives the desktop loop.

Keyboard and mouse drivers submit input through the queueing entry points declared in `c/kernel/gui/wm.h`. The desktop loop drains at most 16 queued events per pass before continuing through canvas resizing, desktop updates, animations and composition. This bound lets rendering and scheduler work proceed during sustained input. `c/kernel/gui/input/input_queue.h` defines the budget; `c/kernel/gui/input/input_queue.c` implements coalescing, overflow handling and draining.

The WM state mutex protects application records, window metadata and canvas lifetime. Drawing and composition also use the graphics mutex. When a path acquires both, it takes the graphics mutex first. During composition, the renderer releases the state mutex for pixel work while retaining the graphics mutex. Event calls can then inspect window state while rendering proceeds. Drawing syscalls likewise release the state mutex around raster work. These paths are in `c/kernel/gui/wm.c`.

Desktop startup uses the account store to choose between the greeter and the desktop. In a build configured with `HWHUD=1`, `devssh_desktop_locked()` in `c/kernel/init/devssh.c` recognizes the developer account by its name and locked password hash and permits startup when that is the store’s sole account type. Other account records select the greeter.

## Window objects and lifetime

`c/kernel/gui/wm_internal.h` defines the application and window state:

| Object | Role |
|---|---|
| `struct app` | Application identity, display name, executable stem, launch argument, liveness and window index. |
| `struct win` | Outer rectangle, title, content canvas, event ring, wait queue, menus and presentation state. |
| `wm_wins` | Window slots. |
| `wm_order` | Window slot indices in stacking order, with the front window at the end. |

The calling process reaches its application record through its `gui` field. `wm_launch()` loads an AEX image, creates a process and starts a user thread. It matches the AEX display name against live application records and restores and raises an existing window when it finds a match. Loading runs with the state mutex released, so the launcher checks again before publishing a new application record.

A process started from the command line acquires an application record through its first `SYS_GUI_CREATE`. The adoption path binds that record to the process and continues through window creation. Both paths are implemented in `c/kernel/gui/wm.c`.

Creation claims a window slot as `WK_BUILDING`, allocates its canvas and initializes its event and presentation state. The window enters `wm_order` after the canvas exists. This order keeps rendering and size updates from treating a partly initialized slot as a usable application window.

The titlebar close control sends `EV_CLOSE`. Applications handle that request; for example, `c/apps/gui/clock/main.c` exits when it receives the event. Process teardown in `c/kernel/exec/proc.c` calls `wm_app_exit()`, which marks the application dead and wakes its event waiters.

The renderer’s `reap()` defers reclamation while event calls hold references to the slot. It then removes the window, releases menus and drawing state, and either frees the canvas or transfers it to a closing animation. This lets a closing picture outlive its window slot.

## Geometry and placement

Application dimensions and pointer coordinates use logical points. Window frames and canvases use device pixels. `S()` and `PT()` in `c/kernel/gui/wm_internal.h` convert between these spaces. Application pointer events carry coordinates relative to the content origin, below the titlebar.

`c/kernel/gui/wm_place.c` centralizes placement and frame changes:

- New windows start at one of six cascade positions and are fitted into the work area below the menu bar and above the dock.
- Frame resizing derives a minimum content size of 180 by 60 points, raised by application requests through `SYS_GUI_WIN_MIN`, then clamps frame dimensions to the screen.
- Zoom saves the current frame and fills the display below the menu bar through the bottom edge. Restoring retrieves the saved frame.
- Dragging to a side edge previews placement in half the work area. Dragging into the menu-bar region previews zoom. Releasing applies the placement.

During a resize drag, frame geometry follows pointer movement while the canvas update pass uses a 120 ms throttle. Release applies the final canvas size. Composition stretches the retained canvas into the current frame while their dimensions differ. These paths are in `c/kernel/gui/wm.c`.

Canvas allocations grow with spare capacity and serve subsequent size changes from that capacity. The policy addresses heap fragmentation caused by repeated requests for slightly different canvas sizes. After reshaping the canvas, the WM sends `EV_RESIZE` with its content width and height in points.

## Stacking, focus and pointer capture

`raise_win()` moves a window to the end of `wm_order`. Rendering walks the stack from back to front, while window hit tests search from front to back. `top_visible()` selects the highest window whose minimized state is clear. Minimized windows retain their stack entries. These helpers are in `c/kernel/gui/wm.c`.

`wm_keyboard_focus()` selects the greeter while the screen is locked. Menus, modal overlays and Exposé take keyboard routing while active; otherwise, ownership follows the highest visible window. `wm_sync_focus()` sends `EV_WINDOW_FOCUS` when ownership changes, updates recent-window order and informs the input method.

Motion and wheel events target application content under the pointer. A press delivered to an application establishes pointer capture. Subsequent motion, wheel events and button releases use that captured window, including coordinates beyond its content bounds. Capture lets the application finish a drag that crosses a window edge.

`c/kernel/gui/wm_chrome.c` implements close, minimize and zoom controls. A titlebar double-click toggles zoom; a press elsewhere in the titlebar starts dragging.

Cmd+Tab opens a switcher ordered by recent focus when it has at least two eligible windows. Further Tab presses advance the selection, Shift+Tab reverses direction, and releasing Cmd restores and raises the selected window. The panel appears after a 150 ms hold, or on the next switcher tick with reduced motion enabled. A quick chord commits before the panel appears. `c/kernel/gui/wm_overlay.c` implements the switcher.

Exposé arranges window thumbnails in a grid, including minimized windows. Cmd+E enters it; the configured hot corner provides another entry after a 150 ms dwell in the upper-right corner. Selecting a thumbnail restores and raises its window. The grid and transitions are implemented in `c/kernel/gui/wm_expose.c`.

## Pointer presentation

At initialization, the WM tries a display cursor plane, then a software overlay for CPU framebuffer presentation, then cursor drawing within composition. The software overlay restores the old pointer rectangle and paints the new one from the RAM back buffer through `c/kernel/gui/fb/fb.c` and `c/kernel/gui/fb/fb_overlay.c`.

The cursor plane and software overlay handle pointer movement through presentation. Dock hover and application drawing report their own damage when movement changes their content.

The desktop loop calls `wm_pointer_sync()` after acquiring the graphics and state mutexes in that order. It publishes the latest software cursor shape and position after draining an input batch, keeping overlay updates within the graphics locking order.

## Menu bar and application menus

`c/kernel/gui/wm_menubar.c` draws the menu bar at a height of 24 points. It contains menu titles, a theme switch, an input-method indicator and a clock. Drawing publishes the geometry used to hit-test titles and controls. The clock requests menu-bar damage every 50 timer ticks, corresponding to half a second at the configured cadence.

`c/kernel/gui/wm_menu.c` builds the menu model from the highest visible application window:

| Model | Menu order |
|---|---|
| Application has published menus | LogitOS, application menus, Window. |
| Default model | LogitOS, File, View. |

The LogitOS menu offers About, Settings and power actions. The default File menu provides close and minimize; View provides Exposé. The Window menu combines these window actions after the application’s menus.

The kernel supplies Chinese labels for File, View and Window. Application menu titles come from the published blob.

<!-- Correction to the previous page's “Titles other than LogitOS are rendered with Chinese labels”: c/kernel/gui/wm_menu.c copies application titles from the blob; the kernel supplies the Chinese File, View and Window labels. -->

Applications publish menus through `SYS_GUI_MENU_SET`. `c/kernel/gui/menu_blob.h` defines the byte format, parser and builder helpers. The kernel copies and validates a blob before replacing the window’s menu set. The displayed model holds copies of titles and items, and rebuilding waits until the dropdown closes so its choices stay stable during selection.

Choosing an enabled application item sends `EV_MENU`: `a` contains the item ID and `b` contains its menu index in the application’s publication. Cmd shortcuts reach the same event path after system shortcuts have been considered.

Each publication receives a generation value. Command delivery checks that the source window still carries the generation used to build the displayed model. This guards against delivering a cached command after menu replacement or window-slot reuse.

## Dock

`scan_apps()` in `c/kernel/gui/wm_dock.c` scans the filesystem root for AEX files. It reads candidate headers through `vfs_pread()` and records filenames, display names, file associations and icon metadata. Reading the header avoids loading an entire executable during discovery.

A dock click calls the shared launch path. A running application has its window restored and raised; a new application begins loading. Running dots use the same display-name lookup as launch. Desktop preferences control running dots, hover magnification and launch bounce.

Dock layout supplies the icon rectangles used by minimize and restore animations. When the front application window is zoomed, the dock slides below the display. Moving the pointer to the bottom edge reveals it over the window, and its interaction area keeps it visible while the pointer remains there. `c/kernel/gui/wm_dock.c` computes layout, visibility progress, painting and hit geometry.

## Animations and repainting

Animations use monotonic milliseconds. `c/kernel/gui/wm_anim.c` provides timing helpers, and the renderer captures one animation time for each composed frame. Rendering workers share that instant so animated objects have consistent progress across the picture.

| Transition | Behavior |
|---|---|
| Opening | Grows from approximately 0.84 scale to full size over 160 ms. |
| Minimize and restore | Moves and scales between the window frame and dock icon over 180 ms, fading near the dock. |
| Closing | Retains the last canvas in a ghost that shrinks and fades over 150 ms. |
| Exposé | Moves between window frames or dock icons and grid cells over 180 ms. |
| Dock visibility | Slides over 220 ms. |

Window opening, minimize, restore and closing are implemented in `c/kernel/gui/wm_anim.c`. Exposé and dock transitions live in their respective modules.

Minimizing changes the hidden state at the start of the transition, allowing focus to move while the picture travels toward the dock. The application continues running. A closing ghost owns its retained canvas, allowing the window slot to be reused during the fade.

`win_draw_rect()` in `c/kernel/gui/wm.c` supplies the displayed rectangle and opacity used by rendering and damage calculations. Animation damage covers previous and current footprints. Completion reports the final affected area so composition paints the endpoint or restores the background.

Applications report completed drawing through flush calls. `SYS_GUI_FLUSH` reports the whole content canvas. `SYS_GUI_FLUSH_RECT` reports a rectangle in content coordinates, with a degenerate rectangle selecting the whole canvas. The compositor expands damage where glass effects sample surrounding backdrop pixels.

The first pixel-writing call after a flush marks the canvas as being drawn. The next flush completes that frame. Composition defers damage overlapping a visible window whose canvas is being drawn, retaining the previous composed picture while the application prepares its update. Deferred damage returns to the pending list for a later pass.

The `ui.reduce_motion` setting settles window and dock transitions and suppresses launch bounce.

## Application interface

`c/kernel/exec/syscall.c` forwards GUI requests to `wm_gui_syscall()`. `include/abi/logit_abi.h` defines syscall numbers, event payloads and window-state selectors. `c/apps/logit.h` provides application wrappers for creation, event retrieval, menu publication and drawing transport. OpenLogit’s window transport uses the same GUI drawing and flush interface.

| Interface | Purpose |
|---|---|
| `SYS_GUI_CREATE` | Creates the application window and canvas. |
| `SYS_GUI_FLUSH`, `SYS_GUI_FLUSH_RECT` | Reports completed drawing for composition. |
| `SYS_POLL_EVENT` | Retrieves an event immediately. |
| `SYS_WAIT_EVENT` | Waits for an event, with an optional timeout in milliseconds. |
| `SYS_GUI_WIN_MIN` | Sets the application’s minimum content dimensions. |
| `SYS_GUI_WIN_STATE` | Queries content size and focus, controls zoom and minimization, and configures panel behavior. |
| `SYS_GUI_MENU_SET` | Publishes application menus; a zero length withdraws them. |

Each window contains an event ring and a wait queue. Adjacent pointer-motion events coalesce to retain the newest position. Appending a new event wakes a waiter for that window. Queue handling is implemented in `c/kernel/gui/input/evq.c`.

`SYS_WAIT_EVENT` releases the state mutex before sleeping. A reference pins the window slot until the call returns. Passing a null event pointer waits for availability and preserves queued events for a later polling pass. `wait_event_ms()` and `wait_idle()` in `c/apps/logit.h` expose these forms. Applications request key-release delivery through `WINS_WANT_KEYUP`.

Panels use the same window object and event transport. `WINS_SET_PANEL` removes the titlebar and places the panel near the upper center of the display. A focused panel hides when it loses keyboard ownership. `c/kernel/gui/wm_panel.c` implements panel placement, visibility and resizing.

## Diagnostics

After the first desktop frame, boot calls `fb_present_bench()` in `c/kernel/gui/fb/fb.c`. It runs pixel-kernel diagnostics and, for CPU framebuffer presentation, times a full framebuffer copy.

Slow-frame records in `c/kernel/gui/wm.c` measure elapsed time from before graphics-mutex acquisition through frame completion. They include the compositor thread’s CPU time, graphics-mutex wait, layer rendering, top overlays, presentation, glass work, damage rectangles and pixel count. The older compositor counter measures the pixel pass. Parallel rendering also accumulates band durations and the longest band from each frame. Glass time sums work across bands and overlaps layer time.

`c/kernel/gui/wm_trace.c` counts frames taking at least 33 ms and retains the worst record. Slow-frame serial reports are limited to one per second and are printed after both mutexes are released. These samples begin once a scheduler thread exists.

Performance and lock statistics are copied while holding the state mutex and printed afterward. Per-tag lock reports carry cumulative counters and print a tag when its call count changes, reducing repeated serial output.

The `HWHUD=1` option in `Makefile` enables the diagnostic panel in `c/kernel/diag/hwhud.c`. The desktop loop queries `wm_system_overlay_visible()` under the state mutex, then updates the panel outside that mutex. The panel yields to dialogs, menus, a shown switcher, Exposé, visible panel windows, input-method composition and notifications, requesting repaint of its former rectangle. A left press on its title row toggles collapse; the WM consumes that press through release.

## Limitations

- `MAXWIN` is 16. Each application record holds one window index; another `SYS_GUI_CREATE` returns success and retains the existing window. See `c/kernel/gui/wm_internal.h` and `c/kernel/gui/wm.c`.
- Input and application event queues have finite capacity. Overflow can drop events after coalescing and motion eviction opportunities are exhausted; counters record the losses. See `c/kernel/gui/input/input_queue.c` and `c/kernel/gui/input/evq.c`.
- The drawing guard expires after 400 ms, allowing composition to display an unfinished canvas when an application stalls. See `c/kernel/gui/wm.c`.
- If canvas growth allocation fails during resizing, composition continues stretching the existing canvas. See `win_apply_size()` in `c/kernel/gui/wm.c`.
- `SYS_GUI_RECEIPT` (225) is reserved in `include/abi/logit_abi.h`. It has no WM handler and reaches the default error return of `-1` through `c/kernel/exec/syscall.c` and `c/kernel/gui/wm.c`.

## See also

- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Compositing and effects](compositing-and-effects.md)
- [GUI events, clipboard and notifications](gui-events-and-services.md)
- [Text and input](../architecture/text-and-input.md)
- [Applications and AEX packages](../architecture/applications-aex.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/gui/wm.h` | Kernel entry points, input submission, external damage and overlay visibility interfaces. |
| `c/kernel/gui/wm_internal.h` | Application and window objects, shared state and geometry units. |
| `c/kernel/gui/wm.c` | Launching, GUI syscalls, stacking, focus, input routing, canvas lifetime, composition and desktop loop. |
| `c/kernel/gui/wm_place.c` | Cascade placement, work-area fitting, size bounds, zoom and snapping. |
| `c/kernel/gui/wm_chrome.c` | Window frames, titlebar controls and double-click handling. |
| `c/kernel/gui/wm_menubar.c` | Menu-bar layout, indicators, clock and hit geometry. |
| `c/kernel/gui/wm_menu.c` | Menu model, dropdowns, actions and application command delivery. |
| `c/kernel/gui/menu_blob.h` | Application menu format, validation and builder helpers. |
| `c/kernel/gui/wm_dock.c` | AEX discovery, dock layout, launch actions and visibility animation. |
| `c/kernel/gui/wm_anim.c` | Animation timing, opening, minimize and restore motion, and closing ghosts. |
| `c/kernel/gui/wm_expose.c` | Thumbnail grid, hot corner and Exposé transitions. |
| `c/kernel/gui/wm_overlay.c` | About and power overlays, recent-window order and Cmd+Tab switcher. |
| `c/kernel/gui/wm_panel.c` | Panel placement, visibility and sizing. |
| `c/kernel/gui/input/input_queue.h` | Raw input queue types, capacity and drain budget. |
| `c/kernel/gui/input/input_queue.c` | Input coalescing, overflow handling and bounded draining. |
| `c/kernel/gui/input/evq.c` | Application event rings, motion coalescing and overflow accounting. |
| `c/kernel/gui/wm_trace.h` | Slow-frame record and diagnostic interfaces. |
| `c/kernel/gui/wm_trace.c` | Slow-frame history, formatting and serial report policy. |
| `c/kernel/gui/fb/fb.c` | Framebuffer presentation, software cursor updates and boot diagnostics. |
| `c/kernel/gui/fb/fb_overlay.c` | Software cursor clipping and composition from the RAM back buffer. |
| `c/kernel/diag/hwhud.c` | Diagnostic panel, system-UI yielding and title-row interaction. |
| `c/kernel/init/devssh.c` | Developer account provisioning and diagnostic-build desktop lock selection. |
| `include/abi/logit_abi.h` | GUI syscall numbers, events and window-state selectors. |
| `c/apps/logit.h` | Application GUI and event wrappers. |
| `c/apps/gui/openlogit_window.h` | OpenLogit window transport through GUI system calls. |
| `c/apps/gui/clock/main.c` | Application handling of close, resize, theme and input events. |
| `c/kernel/exec/syscall.c` | Routing into the WM syscall handler. |
| `c/kernel/exec/proc.c` | Process teardown and the WM exit hook. |
| `c/kernel/init/kmain.c` | WM initialization, first frame, framebuffer diagnostics and desktop-loop entry. |
| `Makefile` | The `HWHUD=1` build option. |