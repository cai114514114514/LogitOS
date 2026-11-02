# Desktop applications

LogitOS provides desktop applications for file management, editing, browsing, media playback, settings and agent tasks. Their sources live under `c/apps/gui/`, and the build packages them as AEX programs. The kernel window manager discovers desktop packages, launches applications and routes document opens. AUI supplies shared controls, layout and themes.

## Desktop app inventory

The root `Makefile` installs Clock, TextEdit, Monitor, Terminal, Files, Preview, Code Studio, Browser and Settings at the guest filesystem root. `tests/agent.mk` adds Assistant. The window manager’s `scan_apps()` reads AEX headers from that directory to build its application registry (`c/kernel/gui/wm_dock.c`).

The package stems below identify the executables. Shared Chinese display names and English search aliases come from `c/include/ui/logit_apps.h`.

| Application | Purpose | Source entry point |
|---|---|---|
| Files / Finder (`files`) | Browses directories in grid or list form. Supports selection, copying, moving, deletion, renaming, folder creation and file information. Its project pane displays and controls tasks associated with the current directory. | `c/apps/gui/files/files.c` |
| TextEdit (`textedit`) | Opens and saves UTF-8 text, with caret navigation, selection, clipboard editing and undo. Its work view connects documents to agent tasks and proposed edits. | `c/apps/gui/textedit/textedit.c` |
| Monitor (`monitor`) | Shows a sortable process table, global memory figures and system information. A confirmation dialog controls termination of the selected process. | `c/apps/gui/monitor/monitor.c` |
| Terminal (`terminal`) | Runs the shell and displays command output, exit status and rich content, including images, tables, charts, progress indicators, audio and video. | `c/apps/gui/terminal/terminal.c` |
| Preview (`preview`) | Displays still and animated images and plays supported audio, video streams and media containers. Chooses its decoding path from file contents. | `c/apps/gui/preview/preview.c` |
| Code Studio (`studio`) | Provides project navigation, document tabs, source editing, search and replacement, completion, diagnostics and AetherScript execution. | `c/apps/gui/studio/studio.c` |
| Browser (`browser`) | Provides tabs, address entry, navigation history, HTML and CSS rendering, and JavaScript execution. | `c/apps/gui/browser/app/main.c` |
| Settings (`settings`) | Edits appearance, desktop, Dock, input, menu-bar clock and IPv4 preferences. Includes a settings inspector and system information views. | `c/apps/gui/settings/settings.c` |
| Clock (`clock`) | Shows an analogue dial, digital time and date, with controls for 12/24-hour format and seconds display. | `c/apps/gui/clock/main.c` |
| Logit Assistant (`assistant`) | Creates and monitors tasks, revises goals, pauses or resumes work, opens reports and presents document conflict decisions. Provides controls to read and replace application memory. | `c/apps/gui/assistant/assistant.c` |

Clock groups its state, layout, motion and rendering under `c/apps/gui/clock/`. Code Studio’s interface calls an application engine under `c/apps/gui/studio/core/`; `c/apps/gui/studio/core/engine.h` declares the interface for documents, projects, persistence, language results and child processes. These divisions give contributors a starting point for changes to application behaviour or presentation.

## Login, search and chat

Greeter, Spotlight and Chat are installed under `/sbin` or `/bin`. The desktop registry scans the filesystem root, so these programs are reached through login, a desktop shortcut or their executable paths.

| Program | Role and guest installation path | Source |
|---|---|---|
| Greeter (`greeter`) | `/sbin/greeter.aex`. Checks account credentials and establishes the login session that allows the desktop to start. | `c/apps/gui/greeter/greeter.c` |
| Spotlight (`spotlight`) | `/sbin/spotlight.aex`. Cmd+Space opens a panel that searches applications, files and settings. An Assistant action passes the query as its initial goal text. The window manager reuses the panel process. | `c/apps/gui/spotlight/spotlight.c` |
| Chat (`ch`) | `/bin/ch.aex`. Sends conversation messages to a configured model endpoint and displays streamed replies. Reads endpoint, model and credential configuration from `/etc/ai.conf`. | `c/apps/gui/ch/ch.c` |

Greeter shares password verification helpers with console login through `c/apps/coreutils/accounts.h`. In the default build, a nonempty `/etc/passwd` makes the window manager start Greeter and wait for a login session with a non-root user ID. Authentication through either Greeter or console login establishes that session, after which the window manager starts the desktop (`c/kernel/gui/wm.c`).

Installation rules live in `Makefile` for Greeter, `tests/fluid_desktop.mk` for Spotlight and `tests/ch.mk` for Chat.

## Hardware bring-up

Builds selected with `HWHUD=1` adapt desktop startup for machines whose keyboard support is still being brought up. The account check permits startup without authentication when the account store contains the `dev` account with its locked password record. Additional accounts, malformed records and oversized or unreadable stores select the login path. The provisioner gives `dev` SSH key access; the desktop exception keeps the graphical environment accessible with that account present (`c/kernel/init/devssh.c`, `c/kernel/gui/wm.c`).

The draft’s description of a store containing “just the provisioned `dev` account” corresponds to a check of the account name and locked password hash. The desktop check reads those fields from the store; provisioning validates further account details in `c/kernel/init/devssh.c`.

The same build option adds a kernel panel showing hardware and performance information (`Makefile`, `c/kernel/diag/hwhud.c`). Clicking its title row collapses or expands it. The panel hides while system overlays, menus, the app switcher, Exposé, visible panel windows such as Spotlight, IME composition or notifications are present, then returns when they close. The window manager supplies this visibility decision so the diagnostic panel yields screen space to desktop controls (`c/kernel/gui/wm.c`, `c/kernel/diag/hwhud_panel.h`).

## Toolkit demos

The build retains two GUI examples for toolkit development.

| Demo | Purpose | Source |
|---|---|---|
| Gallery (`gallery`) | Exercises controls, shapes, data widgets and overlays. Its rendered states provide input to GUI regression harnesses. | `c/apps/gui/gallery/gallery.c` |
| Widgets (`widgets`) | Demonstrates buttons, checkboxes, a text field, colour swatches and a theme toggle. | `c/apps/gui/widgets/widgets.c` |

`Makefile` provides a build rule for Widgets and allows Gallery to be included in a disk through `GALLERY_AEX`. The default desktop package list contains the applications in the inventory above.

## Shared UI toolkit

AUI is an immediate mode toolkit declared in `c/apps/gui/aui.h`. Widget calls draw controls, process the current event and return interactions. The application owns its state and uses it to produce subsequent frames, keeping control behaviour and presentation together in the frame function.

The toolkit provides layout helpers, text fields, buttons, lists, tables, navigation controls, popups and dialogs. It manages focus, hover state, clipping and animation deadlines. Layout coordinates use logical points; raster drawing accounts for the display’s backing scale (`c/apps/gui/aui.h`, `c/apps/gui/aui.c`).

An application creates a window through `c/apps/logit.h`, retrieves events with `poll_event()` and passes events to `aui_feed()`. Its frame function draws between `aui_begin()` and `aui_end()`, and `aui_feed_done()` completes event handling. Clock demonstrates processing and drawing each event before advancing to the next, which preserves quick press and release interactions (`c/apps/gui/clock/main.c`).

`wait_idle()` blocks on the window event queue. A zero timeout waits for an event; a positive timeout supports animation, playback or other deadlines. The loop guidance in `c/apps/gui/aui.h` explains how this avoids repeated polling while idle.

AUI compares tracked drawing operations between frames and submits damage rectangles for presentation (`c/apps/gui/aui.c`). It uses the graphics engine under `c/lib/gfx/` for rendering. Shared headers under `c/include/ui/` supply colour and material tokens, typography, elevation, motion presets, application identities and formatting helpers.

## Terminal and media presentation

Terminal’s shell owns input editing, history and completion (`c/apps/coreutils/sh.c`). Terminal receives stdout and stderr through distinct pipes, plus structured messages through the protocol in `c/apps/coreutils/logit_rich.h`.

The protocol carries command boundaries, exit status and rich objects. Terminal uses these records to associate output with a command and identify its output stream. Its handlers draw images, tables, charts and progress indicators, and manage audio and video playback (`c/apps/gui/terminal/terminal.c`).

Preview links image, audio, video and container decoding into its user process. The application schedules animation frames and media playback, submitting pixels for display and audio samples for playback. Keeping decoding in the application lets it manage sequences of frames and their timing (`c/apps/gui/preview/preview.c`).

## Opening documents and sharing work

AEX headers contribute file extensions to the desktop registry. For document files, the window manager checks registered handlers, then checks whether the file should open in Preview using content sniffing with an extension fallback. Terminal handles the remaining files. Markdown uses the registered text handler when there is no Markdown handler (`c/kernel/gui/wm_dock.c`, `c/kernel/gui/wm.c`).

Files publishes selected paths as agent context and opens Assistant with that context. Its project pane also creates and controls tasks for the current directory (`c/apps/gui/files/files.c`, `c/apps/gui/files/files_project.inc`).

TextEdit exchanges document revisions with the agent service and presents proposed changes in its work view. Assistant exposes task controls, report opening and conflict decisions. These interfaces connect file selection, document editing and task progress (`c/apps/gui/textedit/textedit.c`, `c/apps/gui/textedit/textedit_work.inc`, `c/apps/gui/assistant/assistant.c`).

## Limitations

- TextEdit limits documents to 1 MiB and keeps one undo snapshot (`c/apps/gui/textedit/textedit.c`, `include/abi/aex_agent.h`).
- Monitor lacks per-process CPU time, resident memory, disk and network accounting (`c/apps/gui/monitor/monitor.c`).
- A launch matching a live application’s AEX header name focuses its window and discards the new launch argument (`c/kernel/gui/wm.c`).
- Switching Browser tabs discards the outgoing page’s JavaScript heap state. Returning to a tab rebuilds its document from retained resources (`c/apps/gui/browser/app/tabs.h`, `c/apps/gui/browser/app/main.c`).
- Drawing outside AUI’s tracked operations requires application-supplied damage, such as an `aui_end_rect()` region. Files documents and implements this requirement beside its damage calculation (`c/apps/gui/files/files.c`).

## See also

- [Userland](../architecture/userland.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [GUI events, clipboard and notifications](../kernel/gui-events-and-services.md)
- [Agent runtime](agent-runtime.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Desktop application build rules, AEX metadata, package lists, demo selection and the hardware diagnostic build option. |
| `tests/agent.mk` | Assistant build and installation rules. |
| `tests/ch.mk` | Chat build and installation rules. |
| `tests/fluid_desktop.mk` | Spotlight build and installation rules. |
| `c/apps/gui/files/files.c` | File browsing, selection, file operations, damage reporting and Assistant handoff. |
| `c/apps/gui/files/files_project.inc` | Project task display, creation and controls. |
| `c/apps/gui/textedit/textedit.c` | Text loading, saving, editing, undo and revision exchange. |
| `c/apps/gui/textedit/textedit_work.inc` | Work view, task controls and proposal review. |
| `c/apps/gui/monitor/monitor.c` | Process table, memory views and termination controls. |
| `c/apps/gui/terminal/terminal.c` | Shell connections, scrollback and rich output handling. |
| `c/apps/gui/preview/preview.c` | Media detection, decoding and playback. |
| `c/apps/gui/studio/studio.c` | Code Studio’s interface and event handling. |
| `c/apps/gui/studio/core/engine.h` | Studio application engine interface. |
| `c/apps/gui/browser/app/main.c` | Browser interface, navigation, document lifecycle and event loop. |
| `c/apps/gui/browser/app/tabs.h` | Retained tab state and resource model. |
| `c/apps/gui/settings/settings.c` | Settings state, validation and application of edits. |
| `c/apps/gui/settings/desktop.inc` | Dock, input, menu-bar clock and system information views. |
| `c/apps/gui/clock/main.c` | Clock events, time sampling and repaint scheduling. |
| `c/apps/gui/clock/view.c` | Clock presentation and format controls. |
| `c/apps/gui/assistant/assistant.c` | Task, report, conflict and application memory controls. |
| `c/apps/gui/greeter/greeter.c` | Graphical authentication and session establishment. |
| `c/apps/gui/spotlight/spotlight.c` | Search panel and result actions. |
| `c/apps/gui/ch/ch.c` | Chat configuration, requests and streamed presentation. |
| `c/apps/gui/gallery/gallery.c` | Toolkit demonstration used by GUI regression harnesses. |
| `c/apps/gui/widgets/widgets.c` | Compact toolkit example. |
| `c/apps/gui/aui.h` | Toolkit interface and application loop guidance. |
| `c/apps/gui/aui.c` | Input, focus, frame handling, damage tracking and widget integration. |
| `c/apps/logit.h` | Window, event, drawing and waiting wrappers. |
| `c/include/ui/logit_apps.h` | Application identities, display names and aliases. |
| `c/include/ui/logit_tokens.h` | Colour, material and typography definitions. |
| `c/include/ui/logit_elevation.h` | Shadow and elevation definitions. |
| `c/include/ui/logit_motion.h` | Spring and timed motion presets. |
| `c/include/ui/logit_fmt.h` | Shared size, count, date and time formatting. |
| `c/kernel/gui/wm_dock.c` | Application discovery and extension registry. |
| `c/kernel/gui/wm.c` | Launching, document routing, login transition, Spotlight shortcut and diagnostic panel integration. |
| `c/kernel/init/devssh.c` | Developer SSH provisioning and the desktop login decision for hardware bring-up. |
| `c/kernel/diag/hwhud.c` | Hardware and performance panel sampling, drawing and click handling. |
| `c/kernel/diag/hwhud_panel.h` | Panel visibility, geometry and collapse decisions. |
| `c/apps/coreutils/sh.c` | Shell input editing, history, completion and command records. |
| `c/apps/coreutils/logit_rich.h` | Command metadata and rich output protocol. |
| `c/apps/coreutils/accounts.h` | Shared account and password verification helpers. |
| `include/abi/aex_agent.h` | Agent ABI definitions, including the document size limit. |