# Browser architecture

The LogitOS browser is a native ring 3 application that combines the browser interface, document engine and JavaScript bindings in one process. Its sources live under `c/apps/gui/browser/`. QuickJS executes JavaScript, and NetSurf LibCSS parses CSS and selects styles. The application connects the rendering pipeline to LogitOS socket, file, font and window services.

## Scope and size

A recursive inventory on 2026-11-02 counted 223 C source files, headers and implementation includes under `c/apps/gui/browser/`, totaling 133,082 physical lines. The JavaScript integration subtree contains 121 of those files and 70,451 lines. The count covers generated tables and embedded JavaScript text; vendored engines and shared operating system libraries sit outside the counted directory.

The browser executable links the application and rendering pipeline with QuickJS, LibCSS, libc, shared graphics code, image codecs and a Rust static library. `Makefile` defines that composition. The application service wrappers are in `c/apps/logit.h`.

## Module map

The browser directories group code by responsibility:

| Directory | Responsibility |
|---|---|
| `c/apps/gui/browser/app/` | Window lifecycle, navigation, tabs, resource coordination, input handling and the main event loop. |
| `c/apps/gui/browser/html/` | HTML tokenization and tree construction, including recovery from malformed markup. |
| `c/apps/gui/browser/dom/` | Native document ownership, nodes, attributes, mutation, forms and focus. |
| `c/apps/gui/browser/css/` | LibCSS integration, custom property expansion, stylesheet imports and additional property processing. |
| `c/apps/gui/browser/layout/` | Block, inline, flex, grid and text layout; display list construction and element geometry. |
| `c/apps/gui/browser/paint/` | Display list painting, clipping, text, SVG integration and hit testing. |
| `c/apps/gui/browser/net/` | Resource fetch contracts, URL resolution helpers and the HTTP cache. |
| `c/apps/gui/browser/storage/` | Web Storage backing state and cookie persistence. |
| `c/apps/gui/browser/frame/` | Embedded document loading, policy checks, sandbox flags, context selection and input routing. |
| `c/apps/gui/browser/js/` | QuickJS integration and browser interfaces exposed to scripts. |

Start with `c/apps/gui/browser/app/main.c` to follow application control flow. The native document representation is in `c/apps/gui/browser/dom/tree.h`; `c/apps/gui/browser/layout/block.h` defines the display list consumed by painting and hit testing.

The JavaScript integration has its own service map:

| Directory | Responsibility |
|---|---|
| `c/apps/gui/browser/js/runtime/` | Runtime lifecycle, interface installation, module loading, task budgets and diagnostics. |
| `c/apps/gui/browser/js/dom/` | Node wrappers, events, forms, ranges, observers and frame interfaces. |
| `c/apps/gui/browser/js/style/` | CSSOM, geometry queries and animation interfaces. |
| `c/apps/gui/browser/js/net/` | URLs, Blob, FormData, cancellation, WebSocket and download interfaces. |
| `c/apps/gui/browser/js/storage/` | IndexedDB, Cache and service worker registration interfaces. |
| `c/apps/gui/browser/js/workers/` | Dedicated workers, message ports and message delivery. |
| `c/apps/gui/browser/js/streams/` | Readable, writable and transform streams, including queue and pipe machinery. |
| `c/apps/gui/browser/js/media/` | Media element bindings and the Media Source engine. |
| `c/apps/gui/browser/js/graphics/` | Canvas bindings and backing pixels. |
| `c/apps/gui/browser/js/crypto/` | Digest and SubtleCrypto interfaces. |
| `c/apps/gui/browser/js/text/` | Character encoding and internationalization interfaces. |
| `c/apps/gui/browser/js/wasm/` | WebAssembly bindings over the project’s decoder, validator and interpreter. |

Fetch and XMLHttpRequest coordination lives in `c/apps/gui/browser/js/runtime/webapi.c`, alongside Web Storage, history and location integration. Directory boundaries describe ownership; calls connect services across them.

## From URL to first paint

Navigation enters `load_once_impl()` in `c/apps/gui/browser/app/main.c`. It calls `download_stage_navigation()`, implemented in `c/apps/gui/browser/app/downloads.inc`. That function starts the request through `bfetch_start_nav_from()`, examines the response and stages document bytes. Staging the response before document teardown lets an attachment become a download while preserving the current page.

The fetcher in `c/apps/gui/browser/app/runtime.c` advances requests through `bfetch_pump()`. It uses nonblocking sockets, HTTP/1.1 and HTTP/2 implementations from `c/net/http/http1.c` and `c/net/http/http2.c`, and connection pooling from `c/net/http/hpool.c`. Pooling allows resources from the same origin to reuse connections and their TCP and TLS setup.

After accepting a document response, the loader closes the previous JavaScript runtime and frees its document. It calls `dom_parse()` in `c/apps/gui/browser/dom/tree.c`, which delegates to `html_parse()` in `c/apps/gui/browser/html/tree.c`. Tree construction consumes tokens from `c/apps/gui/browser/html/tokenizer.c` and creates the native nodes used throughout the engine.

The loader gathers inline styles and expands custom properties, then calls `browser_style_layout()`. That function runs three stages:

1. `css_apply()` selects and composes styles through LibCSS.
2. `css_extra_apply()` applies additional property processing.
3. `layout_page()` computes geometry and builds the display list.

The browser’s style representation is `struct cstyle` in `c/apps/gui/browser/css/style.h`.

For a fresh navigation, the application paints this initial layout before fetching linked stylesheets. It then collects external sheets and imports, resolves their resource URLs against stylesheet bases, assembles the author CSS and repeats style, layout and painting. This ordering presents document content while external styles arrive.

## Scripts and later frames

After the stylesheet stage, the loader creates the page runtime through `js_page_open()`. It collects scripts, fetches external script resources and runs the collected scripts. Module loading is coordinated by `c/apps/gui/browser/js/runtime/module.c`.

The loader dispatches `DOMContentLoaded` after that initial script stage. Image requests, inserted scripts and later stylesheet work continue through the application loop. `finish_page_load()` waits for tracked image, script and stylesheet work before dispatching `load` and `pageshow`. These transitions are in `c/apps/gui/browser/app/main.c`.

Decoded images can supply new intrinsic dimensions. The layout engine records that change; the application then performs another layout, exposes the updated geometry and paints the affected frame.

Layout produces a flat list of `struct item` entries containing geometry, content references and painting information. Entries also retain their originating DOM node, connecting rendered content to event targeting. `browser_paint_scroll()` in `c/apps/gui/browser/paint/renderer.c` walks the list and draws through GUI primitives. Their respective services own canvas backing pixels and decoded media frames, which painting consumes.

The application’s `redraw()` finishes with `gui_flush()`. Paths with a calculated damage rectangle use `gui_flush_rect()`. The wrappers in `c/apps/logit.h` reach the window manager in `c/kernel/gui/wm.c`, which records the window’s changed region for compositing.

## Runtime and document ownership

The main loop routes native input into DOM events, advances asynchronous work, reconciles document changes and paints. `js_page_run_due()` in `c/apps/gui/browser/js/runtime/page.c` services network callbacks, messages, workers, animation work and timers through phases with a turn budget. Promise jobs run at checkpoints through `c/apps/gui/browser/js/dom/dom.c`.

Native documents allocate nodes, attribute arrays and text from an arena of chunks. `c/apps/gui/browser/dom/tree.h` explains the reason: allocating each object through libc’s allocator would add repeated searches through its allocation chain. Arena allocation also lets document destruction release the chunk chain.

JavaScript wrappers hold references to native nodes. Navigation and tab switching close the runtime before freeing the DOM so wrapper finalizers can still access their nodes during cleanup. The teardown order is documented beside both paths in `c/apps/gui/browser/app/main.c`.

Timer and animation frame tasks use ownership tokens from `c/apps/gui/browser/app/page.c`. A token contains an owner and an epoch; closing and reopening the owner changes the epoch, allowing the queue to reject callbacks belonging to an earlier document.

Tabs retain document, stylesheet and resource bytes, plus navigation and scroll state. Selecting a retained tab reconstructs its document through the loader. Embedded documents have their own DOM, layout and page contexts; `c/apps/gui/browser/frame/passive.c` activates those contexts while executing or painting their work.

Application startup installs file adapters for local storage and persistent cookies. Session storage uses a tab session key that survives document reconstruction. The storage implementations are in `c/apps/gui/browser/storage/backend.c`, `c/apps/gui/browser/storage/cookies.c` and `c/apps/gui/browser/js/runtime/webapi.c`.

## Embedded components

QuickJS lives in `third_party/quickjs/`. `QJS_SRC` in `Makefile` selects its engine, utility, regular expression, Unicode and bignum sources. The executable links those sources with the project C library and the musl math subset under `third_party/libm/`. Browser interfaces are installed by `c/apps/gui/browser/js/runtime/page.c`.

NetSurf LibCSS lives in `third_party/css/libcss/`. The adapter in `c/apps/gui/browser/css/engine.c` supplies DOM callbacks, creates stylesheets, performs style selection and converts computed styles into `struct cstyle`.

Older dependency records name NetSurf libparserutils and libwapcaplet. The current build supplies those interfaces from `c/lib/logitparserutils/` and `c/lib/logitwapcaplet/`. `THIRD_PARTY.md` preserves the historical entries beside the correction and provenance information.

## Build and include rules

`BROWSER_FILES` in `Makefile` recursively inventories the browser directory. `BROWSER_SOURCES`, `BROWSER_HEADERS` and `BROWSER_INCLUDES` filter that inventory. `BROWSER_JS_SRC` combines the application entry point with the recursively discovered JavaScript integration sources. `BROWSER_PIPE` lists the native pipeline translation units.

An owning translation unit includes some implementation files. `c/apps/gui/browser/layout/block.c` includes the flex and grid implementations; its comments explain how this keeps established host harness source lists connected to the same layout implementation. `c/apps/gui/browser/app/runtime.c` includes `c/apps/gui/browser/net/cache.c`. Build dependencies track these included implementations alongside compiler dependency files.

The compiler search path includes the browser root, `c/apps/gui/browser/`. The global `INCDIRS` scan skips nested browser directories, keeping header selection stable when subsystems share a basename. For example, the headers included by the application have the repository paths `c/apps/gui/browser/dom/tree.h` and `c/apps/gui/browser/js/dom/dom.h`.

The JavaScript object rules compile browser code with `BROWSER_JS_CF`, which turns off forced inclusion of `c/apps/libc/include/features.h`. That header defines a `hidden` macro. The Makefile records an earlier failure where the macro removed the `hidden` member from one compiler view of `struct item`, causing consumers to read the display list with the wrong stride.

The link rule combines the engine, browser, CSS, graphics and Rust objects into a native ELF executable. `tools/mkaex.py` wraps it as an AEX application. `BROWSER_AEX` selects the package placed on the disk image through `ROOT_AEX_PACK`.

The default `all` target builds the ISO. Browser development also requires rebuilding the disk image, which carries the application package into the guest.

## Limitations

- The browser and its origin checks are experimental and unsuitable as a security boundary for hostile pages or valuable credentials. `SECURITY.md` and `TRANSPARENCY.md` define this scope.
- One top level tab owns the live top level engine. Switching tabs discards its JavaScript heap and stops its timers; retained bytes support reconstruction. See `c/apps/gui/browser/app/tabs.h`.
- Initial navigation completes HTML parsing before executing collected scripts. Parser execution interleaved with script execution is unavailable on this path. See `c/apps/gui/browser/html/tree.c` and `c/apps/gui/browser/app/main.c`.
- Custom property expansion uses a table for the document. Overrides scoped to a subtree can affect values elsewhere in that document. See `c/apps/gui/browser/css/vars.c`.
- Dedicated workers share the browser’s execution thread. Long synchronous worker tasks can delay page work. See `c/apps/gui/browser/js/workers/worker.c`.
- Network frames are bounded to four frames and one embedding level, with additional resource limits. This loader supports inline and external scripts; module scripts and direct parent access to a child’s DOM fall outside its support. See `c/apps/gui/browser/frame/passive.c`.
- Service worker support covers registration and input validation; execution and fetch interception fall outside that support. Registration rejects otherwise acceptable requests with `NotSupportedError`. See `c/apps/gui/browser/js/storage/swreg.c`.

## See also

- [The browser application: windows, tabs and navigation](app-and-navigation.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [HTML parsing](html-parsing.md), [the document tree](dom.md) and [CSS](css.md)
- [Layout](layout.md) and [painting and hit testing](paint.md)
- [The JavaScript runtime](javascript-runtime.md) and [DOM and event bindings](dom-bindings.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Browser security model](security-model.md) and [testing the browser](testing.md)
- [Build system](../architecture/build-system.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Source inventories, include paths, compiler flags, linking and disk packaging. |
| `c/apps/gui/browser/app/main.c` | Navigation, load stages, input, event loop, teardown and repaint decisions. |
| `c/apps/gui/browser/app/downloads.inc` | Navigation response staging and download handling. |
| `c/apps/gui/browser/app/runtime.c` | Runtime adapters, resource fetching and HTTP connection integration. |
| `c/apps/gui/browser/app/tabs.h`, `c/apps/gui/browser/app/tabs.c` | Tab state, retained resources, history and persistence. |
| `c/apps/gui/browser/app/page.c` | Timer and animation frame ownership, epochs and queue operations. |
| `c/apps/gui/browser/dom/tree.h`, `c/apps/gui/browser/dom/tree.c` | Native document representation, arena ownership, mutation and parser entry point. |
| `c/apps/gui/browser/html/tokenizer.c`, `c/apps/gui/browser/html/tree.c` | HTML tokenization and tree construction. |
| `c/apps/gui/browser/css/engine.c`, `c/apps/gui/browser/css/extra.c`, `c/apps/gui/browser/css/vars.c` | LibCSS integration, additional properties and custom property expansion. |
| `c/apps/gui/browser/layout/block.h`, `c/apps/gui/browser/layout/block.c` | Display list representation, geometry and layout execution. |
| `c/apps/gui/browser/paint/renderer.c` | Painting, damage calculation and hit geometry. |
| `c/apps/gui/browser/js/runtime/page.c`, `c/apps/gui/browser/js/runtime/module.c` | QuickJS lifecycle, task execution and module loading. |
| `c/apps/gui/browser/js/runtime/webapi.c`, `c/apps/gui/browser/js/dom/dom.c` | Fetch and storage integration, DOM wrappers, events and Promise job checkpoints. |
| `c/apps/gui/browser/frame/passive.c`, `c/apps/gui/browser/frame/policy.c` | Embedded contexts, loading and policy checks. |
| `c/apps/gui/browser/storage/backend.c`, `c/apps/gui/browser/storage/cookies.c` | Web Storage backing state and cookie persistence. |
| `c/apps/gui/browser/js/workers/worker.c`, `c/apps/gui/browser/js/storage/swreg.c` | Worker scheduling and service worker registration behavior. |
| `c/net/http/http1.c`, `c/net/http/http2.c`, `c/net/http/hpool.c` | HTTP clients and connection pooling. |
| `c/apps/logit.h`, `c/kernel/gui/wm.c` | Application GUI calls and window presentation. |
| `third_party/quickjs/quickjs.c`, `third_party/css/libcss/src/select/select.c` | Embedded JavaScript engine and CSS selection implementation. |
| `tools/mkaex.py` | Native executable packaging. |
| `THIRD_PARTY.md`, `TRANSPARENCY.md`, `SECURITY.md` | Dependency provenance, claim rules and security scope. |
