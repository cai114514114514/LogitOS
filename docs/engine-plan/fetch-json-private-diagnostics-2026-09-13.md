# Response.json validation metadata — 2026-09-13

A page may consume an HTTP 4xx response with `Response.json()` without using
XHR. The existing diagnostic build could record its HTTP status but had no
bounded view of ordinary validation categories. This adds that observation
without exposing validation messages, field names or submitted values.

## Product scope

Only `c/apps/browser/js_fetch_body_prelude.inc` and
`c/apps/browser/js_runtime_diagnostics.inc` changed. The existing closure-private
network metadata WeakMap and native `__xhrDiag` argument are reused; no new
page global or public Response property was added. `js_webapi.c` is unchanged.
This observer is compiled only with `JS_RUNTIME_DIAGNOSTICS` in the page Fetch
prelude; it does not add a Worker WebAPI or an authentication behavior.

`Response.json()` still calls the same public `text()` path once and the same
public `JSON.parse()` once. After that text is available, a response carrying
private real-network status 400–499 can be classified. Public status and
header getters are not consulted. Cloning retains the existing one-tee
algorithm and copies only the private metadata association.

The native observer rejects decoded text over 65,536 UTF-8 bytes, then uses a
separate bounded native JSON parse. It does not inspect or modify the
page-visible parse result and does not consume the stream a second time.
Malformed JSON still reaches the original public parser and rejects with its
ordinary SyntaxError. The extra native parse is diagnostic work, not zero-cost
instrumentation or a performance measurement.

Only the following fixed metadata can be logged:

* native fetch handle and HTTP status;
* `detail_kind`: missing, null, array, object, string, number, boolean, other,
  or invalid_json;
* `detail_count` and an aggregate bucket count;
* `type`: missing, string_type, string_pattern_mismatch, json_invalid,
  value_error, or other;
* `loc`: body, query, path, header, or other.

Array entries are aggregated into at most 30 fixed type/location buckets and
share the existing diagnostic line budget. Only own descriptors of freshly
parsed JSON values are read. Location components after index 0, message text,
input, arbitrary field names and unknown type/location strings are not output.
String matching includes its byte length, so an enum prefix followed by an
embedded NUL is correctly categorized as other.

## Local verification

Independent artifacts: `BUILD=build-ai-sites-fetchdiag`.

* 16 ordinary HTTP scenarios traverse the real HTTP parser, Fetch stream and
  `Response.json()`. Both flag-on and flag-off builds pass **145 checks**.
* Before the fixture releases the rest of the body, JSON remains pending and
  no diagnostic classification is emitted.
* Fixed types, all location categories, repeated bucket counts, primitive and
  missing detail shapes, malformed JSON, cloned responses, and text/plain
  responses explicitly consumed as JSON are checked.
* Original values, parser call count, second-consumption rejection and clone
  tee consumption remain correct in both builds. Prototype and public-status
  getter counters remain zero.
* The exact 65,536-byte case is classified. An oversized ASCII body and a body
  below the JavaScript character limit but above the UTF-8 byte limit are not.
  Status 200 and 500 cases are also excluded.
* Private sentinels in message/input/field/type/location fields appear nowhere
  in either complete test log. Every diagnostic line must match the fixed
  schema. The flag-off negative control fails specifically with
  `missing Response.json validation diagnostics` after functional and privacy
  checks have passed.
* Existing streaming Fetch: **62 checks, 0 failures**. Existing XHR diagnostic
  fixture: **48 checks, 0 failures**, with its numeric metadata and sentinel
  checks preserved.
* x86_64-elf diagnostic `js_webapi.o` compiled successfully. `test-mk-wired`
  passed with 301 fragments (300 reachable and one declared wrapper).
* A second agent independently reviewed the JSON/clone hooks and native
  classification without finding an additional stream read, user getter call,
  result mutation or arbitrary-field output.

The main evidence is `fetch-json-diagnostics/{off,on,off-check}.log`,
`final-gates.log`, `xhr-regression.log` and `cross-build.log` under the independent
build. `product.sha256` records the two product files supplied for the root
agent's frozen-tree overlay; those hashes still match after final tests.

```sh
make -j3 BUILD=build-ai-sites-fetchdiag test-fetch-json-diagnostics \
  test-stream test-mk-wired build-ai-sites-fetchdiag/runtime-diagnostics/xhr-on
build-ai-sites-fetchdiag/runtime-diagnostics/xhr-on
```

No site request, SMS retry, account inspection, challenge execution or VM input
was performed by this work. It does not establish what caused any real 422
response; the root agent owns subsequent real-page observation and integration.
