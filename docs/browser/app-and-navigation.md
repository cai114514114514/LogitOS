# The browser application: windows, tabs and navigation

The browser application owns the window, tabs, address field, navigation history and library panels. It coordinates document loading, JavaScript execution, layout and painting through the event loop in `c/apps/gui/browser/app/main.c`. The rendering engine maintains one active top-level tab; switching tabs reconstructs its document from retained source inputs. Tab storage and persistence live in `c/apps/gui/browser/app/tabs.c`.

## Window lifecycle

`app_main()` connects form submission, focus queries, downloads and layout callbacks to the application. It initialises CSS, image decoding and the JavaScript clock, then creates the browser window.

With usable display geometry, `pick_born_size()` uses saved dimensions or chooses 88% of the display width and 80% of its height. It applies desktop margins and minimum dimensions. After `gui_create()`, the app registers a minimum size of 480 × 320 and queries the dimensions accepted by the window manager. The CSS viewport then receives that allocated size. These steps are implemented in `c/apps/gui/browser/app/main.c`.

The window contains a tab strip, address bar, document viewport and status line. Horizontal document overflow adds a scrollbar. On `EV_RESIZE`, `browser_resize()` updates the viewport, reapplies CSS, rebuilds layout, clamps scrolling and dispatches the document’s resize event. Reapplying CSS lets media queries and viewport units respond to the new dimensions.

Startup loads browsing history and bookmarks, then consults `browser.restore_session` before restoring tab metadata. A restored session initially shows the selected tab’s address with a prompt to press Enter. A fresh session starts with a blank tab and the address field focused.

When the main event loop receives `EV_CLOSE`, it records the current scroll position, saves session, history and bookmarks, commits window settings, closes page resources and exits. Closing the last tab creates a fresh blank tab in the existing window. The window and tab paths are in `c/apps/gui/browser/app/main.c` and `c/apps/gui/browser/app/tabs.c`.

## Tabs and page state

The ownership model in `c/apps/gui/browser/app/tabs.h` follows the engine’s use of process globals for the active document’s rendering and scripting state. The selected tab supplies the live DOM, cascade, display list and page runtime. Background tabs retain inputs for reconstruction.

`struct tab` holds:

| State | Purpose |
|---|---|
| URL, response base URL and title | Tab identity, resource resolution and label |
| Horizontal and vertical scroll offsets | Position restored on return |
| Document navigation entries | Per-tab Back and Forward destinations |
| Document source bytes | Input for rebuilding the DOM |
| Assembled author CSS | Styles reused during reconstruction |
| Resource bytes keyed by absolute URL | Retained script and image responses |
| Document response policy | Policy retained with the document bytes |

`TAB_MAX` sets the capacity to twelve tabs.

`tab_switch_to()` first calls `tab_dehydrate()` to record the outgoing tab’s committed address and scroll position and release its derived state. It selects the destination slot, then calls `tab_hydrate()`. Hydration copies the retained document into a parsing buffer and enters the loader in restoration mode. A tab restored from disk fetches its saved URL when selected because its session record contains metadata.

The loader reuses retained author CSS and supplies retained resources to the fetch cache. The transport connection pool survives tab switching. See `c/apps/gui/browser/app/main.c`.

Teardown closes the page runtime before freeing the DOM because JavaScript wrappers hold references to nodes. It also clears focus, form state, selections, popups and cached pointer targets before node storage can be reused.

The app gives `sessionStorage` a session identity derived from the tab slot. That identity survives runtime reconstruction. Closing the tab drops its storage session before reusing the slot. The application calls are in `c/apps/gui/browser/app/main.c`; the storage interface and partitioning are in `c/apps/gui/browser/js/runtime/webapi.c` and `c/apps/gui/browser/storage/backend.c`.

## Address editing and navigation

The address field is an editable UTF-8 buffer with a caret and selection anchor. It supports character and word movement, selection, deletion and clipboard operations. `c/apps/gui/browser/app/address.inc` uses font measurement for drawing, caret placement and pointer hit testing. Long addresses scroll horizontally to keep the caret visible.

The committed document URL comes from `committed_page_url()`, while the address field holds text being edited. Native links and forms resolve through `navigation_base()`, which uses the committed location. This preserves the page’s resolution base while the user types another address. `tests/unit/navigation_base_test.c` exercises links, forms and a tab round trip after address editing.

Pressing Enter calls `addr_infer_scheme()`. It accepts an existing scheme and prepends `https://` to input recognised as a host, including dotted names, `localhost` and a host with a numeric port. Address-bar `javascript:` input takes an evaluation path in the current page runtime, then restores the committed address to the field. These paths live in `c/apps/gui/browser/app/main.c`.

The application handles the following shortcuts before dispatching page keys. Cmd means the Super modifier; Cmd shortcuts in this table also accept Ctrl.

| Input | Action |
|---|---|
| Cmd+L | Focus and select the address |
| Cmd+T | Create and select a blank tab |
| Cmd+W | Close the active tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Cycle through tabs |
| Cmd+1 through Cmd+8 | Select a tab by position |
| Cmd+9 | Select the last tab |
| Cmd+R | Reload while the address field is out of edit mode |
| Cmd+D | Toggle a bookmark for the address |
| Cmd+H or Cmd+Y | Toggle browsing history |
| Cmd+B | Toggle bookmarks |
| Cmd+J | Toggle downloads |

Page keyboard routing dispatches cancelable DOM events and lets focused controls consume their editing keys before applying application defaults. With address editing inactive and the key available to the default handler, Alt+Left and Alt+Right traverse history. Plain Left and Right scroll horizontally; Backspace traverses backward. See `c/apps/gui/browser/app/main.c`.

## Navigation history and persistence

History has three representations:

| Representation | Owner | Contents |
|---|---|---|
| Document navigation | `struct tab` | Up to 64 URL entries per tab |
| History within the current document | JavaScript Web API layer | Entries created by page history operations |
| Browsable history | Tab storage module | Up to 256 visits, deduplicated by URL |

`hist_go()` first calls `js_webapi_hist_step()` to traverse entries within the current document. A successful step updates the address and queues the history event. Otherwise, `tab_hist_go()` supplies a document URL for loading. The application logic is in `c/apps/gui/browser/app/main.c`; history traversal and event queuing are in `c/apps/gui/browser/js/runtime/webapi.c`.

New document navigations stage their history change through `hist_push()`. The loader commits it after response classification accepts a document. This ordering keeps attachment responses from adding a Back entry. Adding an entry truncates the forward branch; a full history discards its oldest entry. See `c/apps/gui/browser/app/tabs.c`.

`load_from()` follows navigation requested by newly loaded pages through a bounded loop. Subsequent destinations replace the current history entry so Back can leave the chain. It copies the initiating document URL before teardown and passes that identity to the fetcher.

After initial document processing, the loader updates the tab title, records a fetched document in browsing history and saves the session. History search matches titles and URLs using a case-insensitive substring. Session snapshots contain tab addresses, titles, both scroll offsets and the active index. The store interface and persistence paths are defined in `c/apps/gui/browser/app/tabs.h`, with their implementation in `c/apps/gui/browser/app/tabs.c`.

## Driving loading and rendering

Navigation enters through `load()` or `load_from()`. For each destination, `load_once_impl()` begins by staging the response through `download_stage_navigation()` in `c/apps/gui/browser/app/downloads.inc`.

Staging fetches while the current document remains alive. A document response transfers its bytes, final URL, status and response policy to the loader. A download response writes the attachment and returns to the current page. Fetch failures also return at this stage.

For an accepted document, the loader in `c/apps/gui/browser/app/main.c` performs these phases:

1. Dispatch `pagehide`, close the outgoing runtime and release its document and layout state.
2. Parse the response and establish the document’s resource base.
3. Collect inline styles, apply CSS, build layout and paint.
4. Fetch and assemble external CSS, then apply styles, layout and paint again.
5. Open the page runtime, fetch collected scripts and execute them.
6. Dispatch `DOMContentLoaded`, reconcile mutations and continue resource work through the event loop.

The early paint gives the document a visible frame while external resources arrive. During synchronous waits, `bfetch_wait()` pumps requests, invokes the loading callback and yields execution. Its implementation is in `c/apps/gui/browser/app/runtime.c`.

After initial loading, `settle_frame()` coordinates stylesheet changes, image completion, DOM mutations and further image requests. `finish_page_load()` waits for tracked image, script and stylesheet work before dispatching `load` and `pageshow`. It then settles mutations made by those handlers.

The outer event loop bounds each native event burst by count and elapsed guest time. Between bursts it advances JavaScript work, resources and rendering. It consumes script-requested navigation after callbacks have returned, allowing the loader to retire the runtime after the executing callback has unwound.

`redraw()` paints browser chrome, the document, overlays and status before flushing the window. Address edits can use `redraw_chrome()` to flush the address band. Eligible document changes use `redraw_page()`, which consults the painter’s dirty region. The loop derives its idle wait from JavaScript deadlines and pending resource work. These decisions live in `c/apps/gui/browser/app/main.c`.

## Reload

Cmd+R calls `load()` for a nonempty address while editing is inactive. It uses the current history position by calling the loader directly.

The handler sets `bfetch_set_bypass(1)` before loading and resets it when `load()` returns. The fetcher’s HTTP cache lookup and insertion paths consult this flag. Resource work processed after the synchronous call returns uses the default cache policy. The handler is in `c/apps/gui/browser/app/main.c`; cache handling is in `c/apps/gui/browser/app/runtime.c`.

## Downloads UI and storage

Response classification considers a forced download request, `Content-Disposition`, content type and recognised filename extensions. An attachment header or forced request takes precedence; otherwise, an HTML content type takes precedence over extension inference. `download_should_save()` implements that ordering in `c/apps/gui/browser/app/tabs.c`.

An accepted attachment preserves the current document and restores its committed address in the field. Page download requests enter a queue drained by the outer event loop. The queue carries the initiating page URL; a redirect across origins clears the forced filename suggestion. These paths are in `c/apps/gui/browser/app/downloads.inc`.

`download_record_as()` writes under `/download`. The naming rules in `c/apps/download_name.h` choose and sanitise a basename. Collision suffixes preserve existing files. With the guest store, the writer reserves a staging directory, writes the body, reads it back for byte comparison and renames the file to its destination. The record holds success or failure, which the app reports through the status line and serial output.

Cmd+J opens the library overlay in downloads mode. Rows show the destination path and source URL. Arrow and page keys move selection, Escape dismisses the panel, and a mouse click selects a record. Enter navigates to the selected record’s source URL through the shared panel activation path. See `panel_row_text()` and the panel input handlers in `c/apps/gui/browser/app/main.c`.

## Closing during loading

`load_poll_close_request()` and `load_close_checkpoint()` service close requests during loading. Checkpoints occur after fetching and between parsing, styling, layout, script execution and lifecycle dispatch. The JavaScript interrupt probe also polls the window queue while page code runs.

On `EV_CLOSE`, the app latches cancellation with `js_page_request_cancel()`. If a JavaScript entry is active, the checkpoint closes transport once and lets the entry unwind. This covers a module dependency fetch whose native wait is nested inside QuickJS. The next safe checkpoint releases staged response bytes and resource queues, closes the runtime and transport, and exits.

The order protects the interpreter stack: freeing QuickJS during its nested native wait would invalidate the runtime that resumes when that wait returns. Application checkpoints are in `c/apps/gui/browser/app/main.c`; cancellation and runtime entry tracking are in `c/apps/gui/browser/js/runtime/page.c`.

Input encountered ahead of close enters a bounded queue for replay by the main loop. Adjacent motion and wheel samples can coalesce. Before page scripts start, suitable wheel gestures can scroll a clean committed frame during resource loading. Routing checks account for inline handlers and nested overflow containers.

`tests/unit/browser_close_during_load_test.c` defines close scenarios after document fetching, during script execution, during subresource loading and inside a module fetch. It also checks wheel input during loading and cancellation after the JavaScript entry unwinds.

## Limitations

- Background tabs lose JavaScript heap state, listeners, timers and live DOM changes. Returning rebuilds the document and reruns scripts; newly created frames or Workers can issue requests. See `c/apps/gui/browser/app/tabs.h`.
- Disk sessions retain metadata, excluding document bytes and per-tab Back/Forward entries. The loading-close checkpoint exits without the final persistence calls performed by the main loop’s close handler. See `c/apps/gui/browser/app/tabs.c` and `c/apps/gui/browser/app/main.c`.
- The application provides no user Stop command. Loading cancellation is connected to window close; Escape handles panels and document interactions. See `c/apps/gui/browser/app/main.c`.
- Bare address words receive a status message because no search engine is configured. Native `follow_link()` skips pure fragment links, and address-bar `javascript:` evaluation discards string completion values. See `c/apps/gui/browser/app/main.c`.
- Download records remain in memory, retaining the latest 32 records. The writer accepts bodies up to 64 MiB; the panel lacks file opening and transfer cancellation. See `c/apps/gui/browser/app/tabs.c` and `c/apps/gui/browser/app/main.c`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/app/main.c` | Window lifecycle, input, navigation, tab transitions, panels and rendering loop |
| `c/apps/gui/browser/app/tabs.h` | Tab ownership, retained state, capacities and persistence interfaces |
| `c/apps/gui/browser/app/tabs.c` | Tab allocation, history, sessions, bookmarks and download writing |
| `c/apps/gui/browser/app/address.inc` | Address drawing, caret geometry, hit testing and horizontal scrolling |
| `c/apps/gui/browser/app/downloads.inc` | Response staging, queued downloads and status reporting |
| `c/apps/gui/browser/app/runtime.c` | Request pumping, synchronous waits, HTTP cache bypass and transport shutdown |
| `c/apps/gui/browser/js/runtime/page.c` | Page runtime lifecycle, location, interrupt cancellation and cleanup |
| `c/apps/gui/browser/js/runtime/webapi.c` | History within a document and tab storage-session bindings |
| `c/apps/gui/browser/storage/backend.c` | Storage partitioning by origin and session identity |
| `c/apps/download_name.h` | Download directory and filename selection and sanitisation |
| `tests/unit/navigation_base_test.c` | Native navigation after address editing and tab reconstruction |
| `tests/unit/browser_close_during_load_test.c` | Close cancellation and wheel input across loading stages |

## See also

- [Browser architecture](overview.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [Painting and hit testing](paint.md)
- [The JavaScript runtime](javascript-runtime.md)
- [Testing the browser](testing.md)