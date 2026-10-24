# Browser modules

The browser lives under the desktop application tree. Directories describe
ownership; filenames describe the role within that subsystem. C function and
ABI names are unchanged by the 2026-09-22 reorganization.

| Directory | Responsibility |
| --- | --- |
| `app/` | Window lifecycle, navigation UI, tabs and page state |
| `dom/` | Native document tree, mutation, forms, focus and top layer |
| `html/` | Tokenizer, tree builder and generated HTML tables |
| `css/` | Styles, cascade, variables, imports, interpolation and reporting |
| `layout/` | Block, flex, grid and text layout |
| `paint/` | Rasterization consumers, text/SVG painting, projection and hit geometry |
| `net/` | Fetch contract, URL resolution and HTTP cache |
| `storage/` | Persistent browser storage and cookie adapters |
| `frame/` | Passive frames, isolation policy and embedded navigation |
| `js/runtime/` | Realm lifecycle, platform installation, modules, budgets and diagnostics |
| `js/dom/` | DOM/event/form/frame bindings, observers, ranges and reflection |
| `js/style/` | CSSOM, animation and font interfaces |
| `js/net/` | Fetch, Blob, URL, WebSocket and download bindings |
| `js/storage/` | IndexedDB, Cache and service-worker registration bindings |
| `js/workers/` | Workers, ports and inter-context messaging |
| `js/streams/` | Readable/writable queues, readers, writers, pipes, transforms and text streams |
| `js/media/` | Media element bindings and Media Source engine |
| `js/graphics/` | Canvas and matrix bindings |
| `js/crypto/` | Digest, SubtleCrypto and entropy contract |
| `js/text/` | Encoding and internationalization |
| `js/wasm/` | WebAssembly bindings |

## Includes and build rules

Cancellation lives in `js/net/abort.c`; stream state machines live in
`js/streams/{core,readable,writable,transform}.c`. Both page and Worker Fetch
install the same services. Their private hooks carry byte accounting and body
disturbance into Fetch, without copying another implementation into a prelude.
`BROWSER_WEBAPI_SRC` includes these services for narrower host consumers.

The HTML priority batch uses `js/runtime/clone.c` and `clone_graph.c` for
structured cloning, `js/runtime/timers.h` for shared page/Worker timer rules,
`dom/validation.c` and `js/dom/validation.c` for live form validation, and
`dom/entries.c` for successful-control snapshots. Blob/File and FormData now
keep bytes and entries private in `js/net/{blob,formdata}.c`. Native submit,
scripted requestSubmit, button activation and Enter share the form controller.
Scope and deferred validation are recorded in
[`html-priority-2026-09-30.md`](../../../../docs/engine-plan/html-priority-2026-09-30.md).

The 2026-09-30 stream expansion is an implementation batch awaiting combined
validation. It adds default streams and text pipelines; byte sources/BYOB and
transferable streams still require separate buffer-ownership work.
Correction after the validation phase: see the measured host and guest results,
observed negative controls and remaining gaps in
[`html-priority-validation-2026-09-30.md`](../../../../docs/engine-plan/html-priority-validation-2026-09-30.md).

The 2026-10-01 CSS batch adds focused nesting, sizing and background producers,
ratio/column/Grid consumers, bitmap resource discovery and rounded box painting.
It follows declarations through real resource requests and guest scanout; the
default disk contains the same browser binary as the validated isolated build.
Measured before/after results, fault controls and unsupported cases are in
[`css-expansion-2026-10-01.md`](../../../../docs/engine-plan/css-expansion-2026-10-01.md).

Include browser headers from the browser root, for example `"dom/tree.h"`
and `"js/dom/dom.h"`. Only that root is on the compiler include path; adding
every nested directory would make identically named headers depend on search
order. Includes of external libraries must also stay unambiguous:
`js/media/media.h` is a binding API, while `c/lib/media/media.h` is the demuxer.

`BROWSER_FILES` in the root Makefile is the recursive inventory. JS source,
headers and implementation fragments derive from it. Flat wildcards previously
selected every `js_*.c`; using a flat wildcard after nesting would silently
omit bindings from both the browser and WPT, allowing both lists to agree on
the same omission. `test-wpt-link` and `test-browser-wiring` check the existing
source/link contracts and their negative controls remain prerequisites.

The directory layout does not imply architectural dependency isolation.
Existing cross-subsystem calls, function names, header guards and feature
behavior are retained. Large functions were not split in this migration.

## Previous names

Paths below were relative to the former `c/apps/browser/` directory. Dated
reports retain those names; this table provides their current locations.

| Former file | Current file |
| --- | --- |
| `address_geometry.inc` | [`app/address.inc`](app/address.inc) |
| `bfetch.h` | [`net/fetch.h`](net/fetch.h) |
| `bfetch_url.inc` | [`net/url.inc`](net/url.inc) |
| `browser.c` | [`app/main.c`](app/main.c) |
| `browser_backface.inc` | [`paint/backface.inc`](paint/backface.inc) |
| `browser_downloads.inc` | [`app/downloads.inc`](app/downloads.inc) |
| `browser_images.inc` | [`app/images.inc`](app/images.inc) |
| `browser_input_trace.inc` | [`app/input_trace.inc`](app/input_trace.inc) |
| `browser_paint.c` | [`paint/renderer.c`](paint/renderer.c) |
| `browser_paint.h` | [`paint/renderer.h`](paint/renderer.h) |
| `browser_rt.c` | [`app/runtime.c`](app/runtime.c) |
| `browser_stylesheets.inc` | [`app/stylesheets.inc`](app/stylesheets.inc) |
| `control_text_metrics.h` | [`paint/text_metrics.h`](paint/text_metrics.h) |
| `cookie_persistence.c` | [`storage/cookies.c`](storage/cookies.c) |
| `cookie_persistence.h` | [`storage/cookies.h`](storage/cookies.h) |
| `cookie_store_guest.inc` | [`storage/cookie_guest.inc`](storage/cookie_guest.inc) |
| `css.h` | [`css/style.h`](css/style.h) |
| `css_engine.c` | [`css/engine.c`](css/engine.c) |
| `css_engine.h` | [`css/engine.h`](css/engine.h) |
| `css_extra.c` | [`css/extra.c`](css/extra.c) |
| `css_extra_cascade.inc` | [`css/extra_cascade.inc`](css/extra_cascade.inc) |
| `css_import.c` | [`css/import.c`](css/import.c) |
| `css_import.h` | [`css/import.h`](css/import.h) |
| `css_inset_math.inc` | [`css/inset_math.inc`](css/inset_math.inc) |
| `css_interp.c` | [`css/interp.c`](css/interp.c) |
| `css_interp.h` | [`css/interp.h`](css/interp.h) |
| `css_logical_margin.inc` | [`css/logical_margin.inc`](css/logical_margin.inc) |
| `css_physical_box.inc` | [`css/physical_box.inc`](css/physical_box.inc) |
| `css_report.c` | [`css/report.c`](css/report.c) |
| `css_report.h` | [`css/report.h`](css/report.h) |
| `css_spacing_math.inc` | [`css/spacing_math.inc`](css/spacing_math.inc) |
| `css_transform_context.h` | [`css/transform_context.h`](css/transform_context.h) |
| `css_vars.c` | [`css/vars.c`](css/vars.c) |
| `dom.c` | [`dom/tree.c`](dom/tree.c) |
| `dom.h` | [`dom/tree.h`](dom/tree.h) |
| `dom_mutation.inc` | [`dom/mutation.inc`](dom/mutation.inc) |
| `dom_serialize.c` | [`dom/serialize.c`](dom/serialize.c) |
| `dom_serialize.h` | [`dom/serialize.h`](dom/serialize.h) |
| `dom_text_mutation.inc` | [`dom/text_mutation.inc`](dom/text_mutation.inc) |
| `element_scroll_wiring.inc` | [`paint/element_scroll.inc`](paint/element_scroll.inc) |
| `embedded_focus.inc` | [`frame/focus.inc`](frame/focus.inc) |
| `embedded_window.inc` | [`frame/window.inc`](frame/window.inc) |
| `encoding_tables.inc` | [`js/text/encoding_tables.inc`](js/text/encoding_tables.inc) |
| `focus.c` | [`dom/focus.c`](dom/focus.c) |
| `focus.h` | [`dom/focus.h`](dom/focus.h) |
| `forms.c` | [`dom/forms.c`](dom/forms.c) |
| `forms.h` | [`dom/forms.h`](dom/forms.h) |
| `frame_open.h` | [`frame/open.h`](frame/open.h) |
| `frame_open.inc` | [`frame/open.inc`](frame/open.inc) |
| `html_entities.inc` | [`html/entities.inc`](html/entities.inc) |
| `html_tags.inc` | [`html/tags.inc`](html/tags.inc) |
| `html_tokenizer.c` | [`html/tokenizer.c`](html/tokenizer.c) |
| `html_tokenizer.h` | [`html/tokenizer.h`](html/tokenizer.h) |
| `html_tree.c` | [`html/tree.c`](html/tree.c) |
| `html_tree.h` | [`html/tree.h`](html/tree.h) |
| `http_cache.c` | [`net/cache.c`](net/cache.c) |
| `http_cache.h` | [`net/cache.h`](net/cache.h) |
| `iframe_policy.c` | [`frame/policy.c`](frame/policy.c) |
| `iframe_policy.h` | [`frame/policy.h`](frame/policy.h) |
| `js_anim.c` | [`js/style/anim.c`](js/style/anim.c) |
| `js_anim.h` | [`js/style/anim.h`](js/style/anim.h) |
| `js_blob_prelude.inc` | [`js/net/blob_prelude.inc`](js/net/blob_prelude.inc) |
| `js_bootstrap_scan.inc` | [`js/runtime/bootstrap_scan.inc`](js/runtime/bootstrap_scan.inc) |
| `js_cache.c` | [`js/storage/cache.c`](js/storage/cache.c) |
| `js_cache.h` | [`js/storage/cache.h`](js/storage/cache.h) |
| `js_canvas.c` | [`js/graphics/canvas.c`](js/graphics/canvas.c) |
| `js_characterdata.c` | [`js/dom/characterdata.c`](js/dom/characterdata.c) |
| `js_characterdata.h` | [`js/dom/characterdata.h`](js/dom/characterdata.h) |
| `js_crypto_keys.inc` | [`js/crypto/crypto_keys.inc`](js/crypto/crypto_keys.inc) |
| `js_crypto_keys_script.inc` | [`js/crypto/crypto_keys_script.inc`](js/crypto/crypto_keys_script.inc) |
| `js_crypto_ops.inc` | [`js/crypto/crypto_ops.inc`](js/crypto/crypto_ops.inc) |
| `js_cssom.c` | [`js/style/cssom.c`](js/style/cssom.c) |
| `js_cssom.h` | [`js/style/cssom.h`](js/style/cssom.h) |
| `js_devtools.c` | [`js/runtime/devtools.c`](js/runtime/devtools.c) |
| `js_devtools.h` | [`js/runtime/devtools.h`](js/runtime/devtools.h) |
| `js_digest.inc` | [`js/crypto/digest.inc`](js/crypto/digest.inc) |
| `js_dom.c` | [`js/dom/dom.c`](js/dom/dom.c) |
| `js_dom.h` | [`js/dom/dom.h`](js/dom/dom.h) |
| `js_dom_iface.inc` | [`js/dom/dom_iface.inc`](js/dom/dom_iface.inc) |
| `js_domparser.c` | [`js/dom/domparser.c`](js/dom/domparser.c) |
| `js_download.c` | [`js/net/download.c`](js/net/download.c) |
| `js_download.h` | [`js/net/download.h`](js/net/download.h) |
| `js_encoding.inc` | [`js/text/encoding.inc`](js/text/encoding.inc) |
| `js_encoding_prelude.inc` | [`js/text/encoding_prelude.inc`](js/text/encoding_prelude.inc) |
| `js_events.c` | [`js/dom/events.c`](js/dom/events.c) |
| `js_events.h` | [`js/dom/events.h`](js/dom/events.h) |
| `js_fetch_body_prelude.inc` | [`js/net/fetch_body_prelude.inc`](js/net/fetch_body_prelude.inc) |
| `js_fetch_hooks_prelude.inc` | [`js/net/fetch_hooks_prelude.inc`](js/net/fetch_hooks_prelude.inc) |
| `js_fetch_object_url_prelude.inc` | [`js/net/fetch_object_url_prelude.inc`](js/net/fetch_object_url_prelude.inc) |
| `js_filenet.inc` | [`js/net/filenet.inc`](js/net/filenet.inc) |
| `js_fontface_prelude.inc` | [`js/style/fontface_prelude.inc`](js/style/fontface_prelude.inc) |
| `js_forms.c` | [`js/dom/forms.c`](js/dom/forms.c) |
| `js_frame.c` | [`js/dom/frame.c`](js/dom/frame.c) |
| `js_frame.h` | [`js/dom/frame.h`](js/dom/frame.h) |
| `js_idb.c` | [`js/storage/idb.c`](js/storage/idb.c) |
| `js_idb.h` | [`js/storage/idb.h`](js/storage/idb.h) |
| `js_importmap.inc` | [`js/runtime/importmap.inc`](js/runtime/importmap.inc) |
| `js_intl.c` | [`js/text/intl.c`](js/text/intl.c) |
| `js_live_range_native.inc` | [`js/dom/live_range_native.inc`](js/dom/live_range_native.inc) |
| `js_live_range_shim.inc` | [`js/dom/live_range_shim.inc`](js/dom/live_range_shim.inc) |
| `js_matrix.inc` | [`js/graphics/matrix.inc`](js/graphics/matrix.inc) |
| `js_media.c` | [`js/media/media.c`](js/media/media.c) |
| `js_media.h` | [`js/media/media.h`](js/media/media.h) |
| `js_media_src.c` | [`js/media/media_src.c`](js/media/media_src.c) |
| `js_message_port.inc` | [`js/workers/message_port.inc`](js/workers/message_port.inc) |
| `js_module.c` | [`js/runtime/module.c`](js/runtime/module.c) |
| `js_module.h` | [`js/runtime/module.h`](js/runtime/module.h) |
| `js_native_mo.inc` | [`js/dom/native_mo.inc`](js/dom/native_mo.inc) |
| `js_page.c` | [`js/runtime/page.c`](js/runtime/page.c) |
| `js_page.h` | [`js/runtime/page.h`](js/runtime/page.h) |
| `js_platform.c` | [`js/runtime/platform.c`](js/runtime/platform.c) |
| `js_platform.h` | [`js/runtime/platform.h`](js/runtime/platform.h) |
| `js_ports.c` | [`js/workers/ports.c`](js/workers/ports.c) |
| `js_ports.h` | [`js/workers/ports.h`](js/workers/ports.h) |
| `js_reflect.c` | [`js/dom/reflect.c`](js/dom/reflect.c) |
| `js_reflect.h` | [`js/dom/reflect.h`](js/dom/reflect.h) |
| `js_reflect.inc` | [`js/dom/reflect.inc`](js/dom/reflect.inc) |
| `js_rejections.inc` | [`js/runtime/rejections.inc`](js/runtime/rejections.inc) |
| `js_runtime_diagnostics.inc` | [`js/runtime/runtime_diagnostics.inc`](js/runtime/runtime_diagnostics.inc) |
| `js_select.c` | [`js/dom/select.c`](js/dom/select.c) |
| `js_semantics.c` | [`js/dom/semantics.c`](js/dom/semantics.c) |
| `js_semantics.h` | [`js/dom/semantics.h`](js/dom/semantics.h) |
| `js_stall.c` | [`js/runtime/stall.c`](js/runtime/stall.c) |
| `js_stall.h` | [`js/runtime/stall.h`](js/runtime/stall.h) |
| `js_subtle.c` | [`js/crypto/subtle.c`](js/crypto/subtle.c) |
| `js_swreg.c` | [`js/storage/swreg.c`](js/storage/swreg.c) |
| `js_swreg.h` | [`js/storage/swreg.h`](js/storage/swreg.h) |
| `js_task_budget.h` | [`js/runtime/task_budget.h`](js/runtime/task_budget.h) |
| `js_tokenlist.c` | [`js/dom/tokenlist.c`](js/dom/tokenlist.c) |
| `js_tokenlist.h` | [`js/dom/tokenlist.h`](js/dom/tokenlist.h) |
| `js_url.c` | [`js/net/url.c`](js/net/url.c) |
| `js_url.h` | [`js/net/url.h`](js/net/url.h) |
| `js_urlbind.c` | [`js/net/urlbind.c`](js/net/urlbind.c) |
| `js_urlbind.h` | [`js/net/urlbind.h`](js/net/urlbind.h) |
| `js_waapi_native.inc` | [`js/style/waapi_native.inc`](js/style/waapi_native.inc) |
| `js_wasm.c` | [`js/wasm/wasm.c`](js/wasm/wasm.c) |
| `js_wasm.h` | [`js/wasm/wasm.h`](js/wasm/wasm.h) |
| `js_wasm_prelude.inc` | [`js/wasm/wasm_prelude.inc`](js/wasm/wasm_prelude.inc) |
| `js_webapi.c` | [`js/runtime/webapi.c`](js/runtime/webapi.c) |
| `js_webapi.h` | [`js/runtime/webapi.h`](js/runtime/webapi.h) |
| `js_websocket.c` | [`js/net/websocket.c`](js/net/websocket.c) |
| `js_websocket.h` | [`js/net/websocket.h`](js/net/websocket.h) |
| `js_websocket_prelude.inc` | [`js/net/websocket_prelude.inc`](js/net/websocket_prelude.inc) |
| `js_worker.c` | [`js/workers/worker.c`](js/workers/worker.c) |
| `js_worker.h` | [`js/workers/worker.h`](js/workers/worker.h) |
| `js_worker_promise_diagnostics.inc` | [`js/workers/worker_promise_diagnostics.inc`](js/workers/worker_promise_diagnostics.inc) |
| `layout.c` | [`layout/block.c`](layout/block.c) |
| `layout.h` | [`layout/block.h`](layout/block.h) |
| `layout_flex.c` | [`layout/flex.c`](layout/flex.c) |
| `layout_flex.h` | [`layout/flex.h`](layout/flex.h) |
| `layout_grid.c` | [`layout/grid.c`](layout/grid.c) |
| `layout_grid.h` | [`layout/grid.h`](layout/grid.h) |
| `layout_stacking.inc` | [`layout/stacking.inc`](layout/stacking.inc) |
| `layout_text.c` | [`layout/text.c`](layout/text.c) |
| `layout_text.h` | [`layout/text.h`](layout/text.h) |
| `linebreak_data.inc` | [`layout/linebreak_data.inc`](layout/linebreak_data.inc) |
| `page_runtime.c` | [`app/page.c`](app/page.c) |
| `page_runtime.h` | [`app/page.h`](app/page.h) |
| `passive_frame.c` | [`frame/passive.c`](frame/passive.c) |
| `passive_frame.h` | [`frame/passive.h`](frame/passive.h) |
| `segbreak_data.inc` | [`js/text/segbreak_data.inc`](js/text/segbreak_data.inc) |
| `storage_backend.c` | [`storage/backend.c`](storage/backend.c) |
| `storage_backend.h` | [`storage/backend.h`](storage/backend.h) |
| `storage_persistence.inc` | [`storage/persistence.inc`](storage/persistence.inc) |
| `svg_dom_paint.inc` | [`paint/svg_dom_paint.inc`](paint/svg_dom_paint.inc) |
| `svg_reflow_cache.inc` | [`paint/svg_reflow_cache.inc`](paint/svg_reflow_cache.inc) |
| `svg_style_props.h` | [`paint/svg_style_props.h`](paint/svg_style_props.h) |
| `tabs.c` | [`app/tabs.c`](app/tabs.c) |
| `tabs.h` | [`app/tabs.h`](app/tabs.h) |
| `text_paint_wiring.inc` | [`paint/text.inc`](paint/text.inc) |
| `top_layer.h` | [`dom/top_layer.h`](dom/top_layer.h) |
| `top_layer.inc` | [`dom/top_layer.inc`](dom/top_layer.inc) |
| `web_entropy.h` | [`js/crypto/entropy.h`](js/crypto/entropy.h) |
