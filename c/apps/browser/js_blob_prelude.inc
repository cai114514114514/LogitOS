/* Blob / File, the WHATWG File API's binary-data object.
 *
 * A chunk of js_webapi.c's PRELUDE -- adjacent string literals, included in
 * the middle of that concatenation, same shape as js_encoding_prelude.inc.
 *
 * WHY THIS FILE AND NOT js_platform.c, WHICH ALREADY HAD ONE. js_platform.c's
 * Blob/File (its `if (!G.Blob) { ... }` block) was always a FALLBACK, not the
 * feature: it exists so a page gets *something* rather than a ReferenceError
 * when js_platform.o is absent, and its own createObjectURL comment says so
 * outright ("returns a data: URL rather than a blob: one" -- a decision made
 * because nothing could resolve a real blob: URL). js_page.c installs
 * js_webapi_install BEFORE js_media_install and js_platform_install, and both
 * of those guard their own installs on `if (!G.Blob)` / `if (!G.URL.
 * createObjectURL)` -- so this file, run first, is what actually wins; theirs
 * become dead code by construction, not by edit. That is also why nothing
 * needed to change in either file: the guards were already there, waiting for
 * something upstream to define the real thing.
 *
 * blob:<origin>/<serial> IS THE THING createObjectURL's own comment in
 * js_platform.c said was out of reach: revocable, bounded, and dereferenced by
 * fetch() the same way -- see the object-URL table and the fetch() blob:
 * branch, both further down in js_webapi.c, right after G.URL exists (the
 * table is *of* URLs and has nowhere to attach until then). This file only
 * has the object itself: constructing one, sizing it, slicing it, reading it
 * back. */

/* BlobPart -> owned bytes. A COPY, always -- a Blob is a snapshot at
 * construction time (the spec's own wording), so `new Blob([buf])` followed
 * by writing through a view over `buf` must not change the Blob's contents.
 * `.slice(0)` on the buffer is what makes that true; a bare `new Uint8Array(x)`
 * (what toU8/encToBytes does, for the different job of "read these bytes
 * once, right now") would alias instead of copy. */
"function blobPartBytes(p) {\n"
"  if (p instanceof G.Blob) return p._b;\n"
"  if (typeof p === 'string') return new G.TextEncoder().encode(p);\n"
"  if (p instanceof ArrayBuffer) return new Uint8Array(p.slice(0));\n"
"  if (typeof SharedArrayBuffer === 'function' && p instanceof SharedArrayBuffer)\n"
"    return new Uint8Array(p.slice(0));\n"
"  if (ArrayBuffer.isView(p))\n"
"    return new Uint8Array(p.buffer.slice(p.byteOffset, p.byteOffset + p.byteLength));\n"
   /* Not a BlobPart at all by the spec's own union type -- but a page that
      passes, say, a number is more likely served by String(p) than by a
      TypeError three layers down from where the mistake was made. */
"  return new G.TextEncoder().encode(String(p));\n"
"}\n"
/* The `type` member: ASCII-lowercased, and reset to '' if it contains any
 * character outside U+0020..U+007E (the spec's "cannot be represented" rule --
 * a type string is meant to become an HTTP header value, and a control
 * character or a non-ASCII byte cannot be one safely). */
"function blobNormType(t) {\n"
"  if (t === undefined || t === null) return '';\n"
"  t = String(t);\n"
"  return /^[\\x20-\\x7E]*$/.test(t) ? t.toLowerCase() : '';\n"
"}\n"

"G.Blob = function Blob(parts, opts) {\n"
"  var arr = parts === undefined || parts === null ? [] : Array.from(parts);\n"
"  var chunks = new Array(arr.length), total = 0, i;\n"
"  for (i = 0; i < arr.length; i++) { chunks[i] = blobPartBytes(arr[i]); total += chunks[i].length; }\n"
"  var all = new Uint8Array(total), o = 0;\n"
"  for (i = 0; i < chunks.length; i++) { all.set(chunks[i], o); o += chunks[i].length; }\n"
   /* Non-enumerable: `_b` is bookkeeping, not part of the Blob interface, and
      an enumerable one would show up in `for...in` / `JSON.stringify`. */
"  Object.defineProperty(this, '_b', { value: all, enumerable: false, configurable: true, writable: true });\n"
"  Object.defineProperty(this, 'size', { value: total, enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'type', { value: blobNormType(opts && opts.type), enumerable: true, configurable: true });\n"
"};\n"
"G.Blob.prototype = {\n"
"  constructor: G.Blob,\n"
   /* start/end: negative counts back from the end, then both clamp into
      [0, size] and end is not allowed before start -- the exact algorithm in
      the spec's "slice blob" steps, not merely "seems to work on positive
      inputs". */
"  slice: function (start, end, contentType) {\n"
"    var size = this.size;\n"
"    var relStart = start === undefined ? 0 : (start < 0 ? Math.max(size + start, 0) : Math.min(start, size));\n"
"    var relEnd = end === undefined ? size : (end < 0 ? Math.max(size + end, 0) : Math.min(end, size));\n"
"    var span = Math.max(relEnd - relStart, 0);\n"
"    var b = Object.create(G.Blob.prototype);\n"
"    Object.defineProperty(b, '_b', { value: this._b.slice(relStart, relStart + span), enumerable: false, configurable: true, writable: true });\n"
"    Object.defineProperty(b, 'size', { value: span, enumerable: true, configurable: true });\n"
"    Object.defineProperty(b, 'type', { value: blobNormType(contentType), enumerable: true, configurable: true });\n"
"    return b;\n"
"  },\n"
   /* text()/arrayBuffer()/bytes() are real microtasks (Promise.resolve().then),
      not values wearing a `.then`: a page that does
      `blob.text().then(f); somethingSynchronous();` must see the synchronous
      code run first, same as it would against a real Blob. */
"  text: function () { var self = this; return Promise.resolve().then(function () {\n"
"    return self._b.length ? __utf8(u8ab(self._b)) : ''; }); },\n"
"  arrayBuffer: function () { var self = this; return Promise.resolve().then(function () {\n"
"    return u8ab(self._b); }); },\n"
"  bytes: function () { var self = this; return Promise.resolve().then(function () {\n"
"    return new Uint8Array(u8ab(self._b)); }); },\n"
"  stream: function () { return rsOf(this._b.slice()); }\n"
"};\n"
"Object.defineProperty(G.Blob.prototype, Symbol.toStringTag, { value: 'Blob', configurable: true });\n"

"G.File = function File(parts, name, opts) {\n"
"  G.Blob.call(this, parts, opts);\n"
"  Object.defineProperty(this, 'name', { value: String(name), enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'lastModified', {\n"
"    value: (opts && opts.lastModified !== undefined) ? Number(opts.lastModified) : Date.now(),\n"
"    enumerable: true, configurable: true });\n"
"};\n"
"G.File.prototype = Object.create(G.Blob.prototype);\n"
"G.File.prototype.constructor = G.File;\n"
"Object.defineProperty(G.File.prototype, Symbol.toStringTag, { value: 'File', configurable: true });\n"
