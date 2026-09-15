/* Shared production prelude: each evaluation owns its closure and JSValues.
 * WEBAPI_FETCH_WORKER_PRELUDE excludes document-only additions. */
/* The hooks C calls back through. */
   /* Used by fetch_fail (C) so a network failure rejects with a REAL
      TypeError -- one whose `.constructor === TypeError` and `instanceof
      TypeError` both hold -- rather than a plain Error wearing a `.name`
      property that only LOOKS like one in a printed message.
      AbortError/NetworkError/TimeoutError are the three DOMException names
      the fetch spec rejects with (fetch_fail's `name` argument at every C
      call site is one of these three or "TypeError"), and they go through
      G.DOMException rather than G[name] -- there is no global constructor
      named `AbortError`, so the old `G[name] || Error` fell back to a plain
      Error wearing a `.name` for exactly these, same trap as
      AbortController's own abortError() helper had (see its comment above).
      Genuine TypeErrors (a real network failure, per the fetch spec) still
      go through G.TypeError below -- turning THOSE into a DOMException
      would be the opposite error. */
"var DOM_ERROR_NAMES = { AbortError: 1, NetworkError: 1, TimeoutError: 1 };\n"
"function mkError(name, message) {\n"
   /* Storage calls this after the full page installer has published the real
      DOMException. Narrow WebAPI-only host links have no such constructor;
      keep a named Error there, not a fabricated DOMException class. */
"  if ((DOM_ERROR_NAMES[name] === 1 || name === 'QuotaExceededError') && typeof G.DOMException === 'function') return new G.DOMException(message, name);\n"
"  var C = G[name];\n"
"  if (typeof C === 'function') return new C(message);\n"
"  var e = new Error(message); e.name = name; return e;\n"
"}\n"
"return {\n"
"  mkError: mkError,\n"
/* The table stays private. Only its owning C realm may request bytes; copying
 * to worker-owned memory happens synchronously at construction, before a
 * caller can revoke the URL on the next line. A fragment is not a store key. */
"  blobConfigure: function (origin, generation) { __objURLOrigin=origin; __objURLGeneration=generation; },\n"
"  blobSnapshot: function (url) { var hash=url.indexOf('#'); if(hash>=0)url=url.slice(0,hash);\n"
"    return Object.prototype.hasOwnProperty.call(__objURLs,url)?__objURLs[url]._b:null; },\n"
   /* Called when the HEADERS arrive, not when the body does. C keeps the
      three functions handed back and drives the body through them, which is
      what makes the response a stream the page can read from while the
      network is still writing to it. `handle` is the SAME abort handle
      __fetchAbort() already takes (fetch_deliver_headers passes wf_handle(f)
      as argv[7]); wiring it as this stream's underlying-source `cancel` is
      what makes `response.body.getReader().cancel()` (and a `for await`
      loop's `break`, which calls the same thing) a REAL cancellation rather
      than a no-op the C side never hears about. Before this, rsCancel would
      settle the JS-side stream and stop there: fetch_step kept pumping the
      abandoned transfer to completion off the wire, and the WF_HIGHWATER
      byte counter it throttles on -- reset to 0 by the very cancel that was
      supposed to stop it -- could never trip again, so a cancelled download
      ran at full speed for the rest of its length and held one of WF_MAX
      slots the whole time. __fetchAbort's own generation check (js_fetch_abort,
      above) is what makes calling this safe on a handle whose slot the body
      already finished and released: a stale handle is a no-op by
      construction, not by a guard added here. */
"  mkResponse: function (status, statusText, pairs, url, redirected, type, nobody, handle, diagnosticJSON) {\n"
"    var ctrl = null;\n"
"    var stream = new G.ReadableStream({ start: function (c) { ctrl = c; },\n"
"      cancel: function () { __fetchAbort(handle); } });\n"
#ifdef WEBAPI_RESPONSE_PUBLIC_NETWORK
"    var r = new G.Response(nobody ? null : stream, { status: status, statusText: statusText,\n"
#else
"    var r = makeResponse(nobody ? null : stream, { status: status, statusText: statusText,\n"
#endif
"      headers: pairs, url: url, redirected: redirected, type: type, guard: 'immutable' });\n"
#if defined(JS_RUNTIME_DIAGNOSTICS) && !defined(WEBAPI_FETCH_WORKER_PRELUDE)
"    xhrDiagSet(xhrResponseDiag, r, {fetch: handle, json: diagnosticJSON, status: status});\n"
#endif
"    return { r: r,\n"
"      push: function (buf) { ctrl.enqueue(new Uint8Array(buf)); return ctrl.bytes(); },\n"
"      close: function () { ctrl.close(); },\n"
   /* The failure that arrives AFTER the promise settled belongs to the body
      stream -- which is what a browser does when a connection dies (or is
      aborted) mid-download. The name is carried through so an abort reads as
      an AbortError to the page and not as a generic network failure. */
"      error: function (m, n) { ctrl.error(mkError(n || 'TypeError', m)); } };\n"
"  },\n"
