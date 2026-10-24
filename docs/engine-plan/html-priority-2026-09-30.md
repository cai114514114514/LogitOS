# HTML priority implementation batch — 2026-09-30

The owner narrowed the proposed complete Canvas chapter to important missing
features across the HTML Standard. This batch prioritizes state transfer,
form submission, and scheduling because their previous behavior lost data or
bypassed real controls. This is source inspection, not a measured site ranking.
Three agents implemented independent modules; root integrated their consumers.
No new dependency was introduced. Existing QuickJS gained three small native
brand/buffer accessors; that is an interface change to the existing engine.

## Implemented code

| Standard area | Previous behavior | New behavior and consumer |
| --- | --- | --- |
| [Structured data](https://html.spec.whatwg.org/multipage/structured-data.html) | Array extra keys disappeared; author constructors/getters could determine clone types; transfer options were ignored | Native brands plus a private graph walker preserve cycles, aliases, Map/Set, boxed values, Date/RegExp, Error cause, and shared backing buffers. Transfer validates and clones first, revalidates all sources, then detaches. Page and Worker install it. |
| [Constraint validation](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constraints) | Control wrappers and scripted activation could bypass native state and validation | Live ValidityState, custom errors, required/type/range/step/length checks, invalid reporting, novalidate/formnovalidate, and checked submitter ownership. JS requestSubmit, native buttons, scripted click and Enter use the common controller. |
| [Form entry lists](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constructing-the-entry-list) | FormData(form) ignored the form; navigation used a different collector; option values could truncate at 512 bytes and use labels | Shared native snapshots include external controls, duplicate names, selected enabled options, disabled-fieldset rules, empty File entries and UTF-8 charset entries. FormDataEvent listener edits feed native GET navigation. Names and string values normalize CR/LF in the shared URL encoder. |
| [Timers](https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers) | Nested timers had no clamp; conversion exceptions could be swallowed; allocation failure could drop arguments; Worker receivers and interval ownership differed | Shared long conversion, nesting rules, positive ID reuse and zero-delay interval semantics. Both loops retain running timer ownership across callbacks/checkpoints. DOM and Worker microtasks reset timer nesting. |
| [Blob/File](https://w3c.github.io/FileAPI/#blob) | Public _b could change bytes behind Fetch, object URLs and clone; FormData _e could replace its payload | Private WeakMaps own immutable bytes, File metadata and FormData entries. Fetch encoding, Blob URLs, export and cloning consume private hooks. FormData iteration follows mutations rather than a snapshot. |

## Files and integration

- `js/runtime/clone.c`, `clone_graph.c`, their headers and existing
  `third_party/quickjs/quickjs.{c,h}` own cloning. The old platform copier was
  replaced before MessagePort fallback captures the function.
- `js/runtime/timers.h` is the one scheduling-rule source; page and Worker
  remain separate queues with their existing budgets/epoch boundaries.
- `dom/validation.c`, `js/dom/validation.c` provide the form controller and
  wrappers; native state and JS wrappers share existing forms storage.
- `dom/entries.c`, `js/dom/entries.c` collect and encode controls without
  author-controlled collection getters. The latter can deliver actual
  payload-bearing UA events through the private DOM event door.
- `js/net/blob.c`, `formdata.c` replace the large class implementations in
  preludes with factory calls. MIME helpers remain shared with Fetch.
- Makefile groups cover these dependencies, including narrow host lists that
  previously named forms.c alone. No commit or deployment was requested.

## Boundaries deliberately still open

- Native MessagePort/Worker packet serialization still uses JS_WriteObject;
  adding structuredClone does not give those packets its full type coverage.
  Streams/ports are not transferable through this implementation.
- More JS-backed platform objects need private serialization/brand hooks.
  Blob/File clone correctly; registered FormData, Fetch objects, Abort objects
  and Streams are explicitly refused rather than copied as ordinary data.
- Pattern validation requires genuine RegExp Unicode sets (`v`). The engine
  currently reports unavailable support instead of substituting `u`.
- Date/time input states remain text fallbacks, with honest `type` reflection;
  calendar controls and temporal validation were not added.
- reportValidity uses real focus and visible status text; there is no complete
  validation balloon UI. Native POST and method=dialog form navigation remain
  explicitly unsupported. Fetch/FormData multipart POST is a separate path.
- File selection has no native picker/broker: empty selected-file state is
  real, but this work does not invent files. Image-button click coordinates
  still need native capture; constructor entries currently use initial 0,0.
- `dirname` uses HTML directionality and existing Unicode classes, including
  ancestor dir=auto text scanning and isolated-subtree exclusion. Shadow-root
  host/assigned-node direction still needs a bridge and is explicitly refused.
- String timer handlers remain explicit refusals pending TrustedScript/CSP.
  Fully-active document waiting and Worker suspension were not added.

## Deferred combined validation

Historical implementation-phase record follows. Correction after the owner's
“开始验证”: combined validation is now recorded in
[the validation report](html-priority-validation-2026-09-30.md). The earlier
statement below describes the point before that instruction, not the current
tested tree.

At the owner's request, no builds or tests were run during this implementation
batch. Earlier Abort/Fetch results predate these edits and are not evidence for
the current tree. This batch is not claimed buildable or conformant yet.

Ordinary fixtures under `tests/fixtures/html-priority/` cover clone, forms,
FormData, Blob and timers, with a Worker running the same clone/timer scenarios.
They must be run through the actual rebuilt browser and inspected for rendered
results. Host probes alone cannot establish guest behavior.

Three controls are present but have not yet been observed failing:
`JS_CLONE_KEEP_BUFFER`, `JS_TIMER_NO_CLAMP`, `FORM_VALIDATION_IGNORE_ERRORS`.
The final gate must make each control a prerequisite, rebuild ring-3 disk
contents, run `make test-mk-wired`, and report each observed red assertion.
ID wrap/OOM, Worker checkpoint yield/termination, lifecycle cleanup and existing
Fetch/Streams/DOM regressions remain part of that combined phase.
