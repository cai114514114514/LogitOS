# Spec: executing dynamically-inserted `<script>` elements

Status: DIAGNOSED, NOT IMPLEMENTED (2026-08-16). Deferred deliberately — it
straddles the ring-3/kernel DOM boundary and involves re-entrancy, which is the
class this project handles spec-first rather than opportunistically.

## The gap

Per HTML5, inserting a `<script>` element into the document (via
`appendChild` / `insertBefore` / `append` / `innerHTML` in some cases) whose
`src` is set, or with inline text, must FETCH and EXECUTE it. LogitOS today
does neither: `insert_run()` in `c/apps/browser/js_dom.c` splices the node into
the tree and marks layout, full stop. Only the scripts present in the parsed
HTML are collected (`collect_scripts` in `browser.c`) and run
(`run_collected_scripts`).

## Evidence it matters

`baidu` on the site scoreboard (2026-08-16 full pass) throws
`Error: [MODULE_TIMEOUT]Hang(none) Miss(plugins/bzPopper, ...)` from a timer:
its AMD-style loader injects module `<script>`s and waits for their `onload`,
which never fires because the injected scripts are never fetched. Many modern
sites load this way; this is a whole-class blocker, not one site.

## Why it is not a five-minute change

1. **Two address spaces.** The DOM mutation happens in `js_dom.c` (ring 3, but
   the DOM is the kernel-mirrored structure); the fetch+eval machinery
   (`bfetch_*`, `js_page_eval`) is browser.c/js_page.c. `insert_run` needs to
   signal "a runnable script just entered the document" across to the loader.
   A hook function pointer (`dom_set_script_sink`, set by browser.c) is the
   shape, mirroring how `js_dom_set_note` already crosses the same seam.

2. **Re-entrancy.** A script, while executing, can insert another script.
   `js_page_eval` is already on the stack. Synchronous execution from inside
   `insert_run` would recurse the evaluator through a DOM-mutation callback —
   the exact footgun the CPU-slice watchdog (commit 14157e9e3) exists near.
   The safe model is: `insert_run` ENQUEUES the node onto a pending-script
   queue; the browser's existing per-frame loop drains it (the same loop that
   pumps timers and `res_fetch_all`). `src` scripts fetch async and run on
   completion in insertion order; inline scripts run at the next drain. This
   also matches the spec's "prepare a script" / microtask ordering better than
   a synchronous call would.

3. **Ordering and `async`/`defer`.** Classic inserted scripts without `async`
   run in insertion order; `async` run as they arrive. `document.write` is out
   of scope (and rare in live code). The existing `resent` table already
   models ordered fetch-and-run; the work is feeding inserted nodes into it
   rather than only parse-time ones.

## Test shape

`loader_test` is the right host bed (it links the real js_dom.c + js_page.c +
a fake bfetch). A fixture whose first script does
`var s=document.createElement('script'); s.src='/late.js'; document.head.appendChild(s);`
where `/late.js` sets a global — assert the global is set after the load
settles, and that a second inserted script runs after the first. Negative
control: the same with the sink unhooked, global stays unset.

## Verdict

Real, worth doing, but a designed change with a re-entrancy trap in it — a
subagent task with the queue-not-recurse model stated up front, or a careful
solo pass, not an inline patch.
