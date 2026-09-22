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
/* ---- MIME type parsing/serializing (mimesniff 4.4 / 4.5) ----
 * There was no MIME parser anywhere in the tree; the `type` member was a
 * printable-ASCII check plus a blanket .toLowerCase(), which is a different
 * and much weaker rule than "parse a MIME type, then serialize the record" --
 * it keeps duplicate/out-of-order parameters, keeps whitespace, lowercases
 * parameter VALUES it must leave alone, and collapses any non-ASCII input to
 * '' instead of parsing the ASCII prefix that remains after whitespace
 * stripping. mimeSerialize is also the thing that re-quotes a parameter value
 * that is not itself a valid HTTP token, which is where the printable-ASCII
 * check could never have gone even if it had been complete.
 *
 * These two functions are used from here (blobNormType), from Response/
 * Request's Body mixin (a Blob's serialized type), and from the data: URL
 * processor -- so they are installed early in the PRELUDE, in this file,
 * rather than beside any one of those three call sites. */
   /* HTTP whitespace (mimesniff / fetch): TAB LF CR SPACE. Deliberately NOT
      \f and NOT the wider set String.prototype.trim strips -- see
      isASCIIWhitespace below, which DOES include \f and is a different,
      wider set used only by the data: URL processor. */
"function isHTTPWSc(c) { return c === 9 || c === 10 || c === 13 || c === 32; }\n"
"function stripHTTPWS(s, lead, trail) {\n"
"  var i = 0, j = s.length;\n"
"  if (lead) while (i < j && isHTTPWSc(s.charCodeAt(i))) i++;\n"
"  if (trail) while (j > i && isHTTPWSc(s.charCodeAt(j - 1))) j--;\n"
"  return s.slice(i, j);\n"
"}\n"
   /* RFC 7230 tchar: DIGIT / ALPHA / one of !#$%&'*+-.^_`|~ -- note '%' IS a
      token character (it is easy to leave out by eye and the data: URL
      processor's own corpus catches it: a raw \f in a data: URL's type is
      percent-encoded to the literal text "%0c" by the URL parser before this
      function ever sees it, and the expected Content-Type keeps that text
      verbatim -- which only happens if '%' parses as an ordinary token byte
      in the subtype rather than breaking the token and failing the parse). */
"function isHTTPTokenChar(c) {\n"
"  if (c >= 48 && c <= 57) return true;\n"
"  if (c >= 65 && c <= 90) return true;\n"
"  if (c >= 97 && c <= 122) return true;\n"
"  return c === 33 || c === 35 || c === 36 || c === 37 || c === 38 || c === 39 ||\n"
"         c === 42 || c === 43 || c === 45 || c === 46 || c === 94 || c === 95 ||\n"
"         c === 96 || c === 124 || c === 126;\n"
"}\n"
"function isHTTPToken(s) {\n"
"  if (!s.length) return false;\n"
"  for (var i = 0; i < s.length; i++) if (!isHTTPTokenChar(s.charCodeAt(i))) return false;\n"
"  return true;\n"
"}\n"
   /* HTTP quoted-string token code point: TAB, U+0020..U+007E, U+0080..U+00FF.
      A parameter VALUE that fails this is dropped entirely, not truncated. */
"function isHTTPQSChar(c) { return c === 9 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF); }\n"
"function isAllHTTPQS(s) {\n"
"  for (var i = 0; i < s.length; i++) if (!isHTTPQSChar(s.charCodeAt(i))) return false;\n"
"  return true;\n"
"}\n"
   /* mimesniff #parsing-a-mime-type, transcribed step for step -- including
      the ORDER of the ';'-vs-'=' check in the parameter loop, which is the
      part every ad-hoc splitter gets backwards: a parameter with no '=' at
      all (bare "test" in "text/html;test;charset=gbk") must be walked past
      WITHOUT being recorded, and the ';' that ends it must be left
      unconsumed so the outer loop's own `pos++` is what skips it -- consume
      it here too and the next parameter's name silently swallows a
      character. */
"function mimeParse(input) {\n"
"  input = stripHTTPWS(String(input), true, true);\n"
"  var n = input.length, pos = 0;\n"
"  var ts = pos;\n"
"  while (pos < n && input.charCodeAt(pos) !== 47) pos++;\n"
"  var type = input.slice(ts, pos);\n"
"  if (!type.length || !isHTTPToken(type)) return null;\n"
"  if (pos >= n) return null;\n"
"  pos++;\n"
"  var ss = pos;\n"
"  while (pos < n && input.charCodeAt(pos) !== 59) pos++;\n"
"  var subtype = stripHTTPWS(input.slice(ss, pos), false, true);\n"
"  if (!subtype.length || !isHTTPToken(subtype)) return null;\n"
"  var rec = { type: type.toLowerCase(), subtype: subtype.toLowerCase(), params: [] };\n"
"  while (pos < n) {\n"
"    pos++;\n"
"    while (pos < n && isHTTPWSc(input.charCodeAt(pos))) pos++;\n"
"    var nameStart = pos;\n"
"    while (pos < n && input.charCodeAt(pos) !== 59 && input.charCodeAt(pos) !== 61) pos++;\n"
"    var pname = input.slice(nameStart, pos).toLowerCase();\n"
"    if (pos < n) {\n"
"      if (input.charCodeAt(pos) === 59) continue;\n"
"      pos++;\n"
"    }\n"
"    if (pos >= n) break;\n"
"    var pvalue;\n"
"    if (input.charCodeAt(pos) === 34) {\n"
       /* Collect an HTTP quoted string (fetch spec), extract-value flag set:
          a trailing lone backslash with nothing after it is a literal
          backslash and ends the string; an unterminated quote (no closing ")
          ends at input's end rather than failing the whole parse. */
"      pos++;\n"
"      pvalue = '';\n"
"      for (;;) {\n"
"        while (pos < n && input.charCodeAt(pos) !== 34 && input.charCodeAt(pos) !== 92) { pvalue += input.charAt(pos); pos++; }\n"
"        if (pos >= n) break;\n"
"        var qb = input.charCodeAt(pos);\n"
"        pos++;\n"
"        if (qb === 92) {\n"
"          if (pos >= n) { pvalue += '\\\\'; break; }\n"
"          pvalue += input.charAt(pos); pos++;\n"
"        } else break;\n"
"      }\n"
"      while (pos < n && input.charCodeAt(pos) !== 59) pos++;\n"
"    } else {\n"
"      var vs = pos;\n"
"      while (pos < n && input.charCodeAt(pos) !== 59) pos++;\n"
"      pvalue = stripHTTPWS(input.slice(vs, pos), false, true);\n"
"      if (!pvalue.length) continue;\n"
"    }\n"
"    if (pname.length && isHTTPToken(pname) && isAllHTTPQS(pvalue) &&\n"
"        !rec.params.some(function (p) { return p[0] === pname; }))\n"
"      rec.params.push([pname, pvalue]);\n"
"  }\n"
"  return rec;\n"
"}\n"
   /* mimesniff #serializing-a-mime-type. A value that is empty or is not
      itself a bare HTTP token must be re-quoted, backslash-escaping '\\' and
      '\"' -- NOT re-run through percent-encoding or any other scheme. */
"function mimeSerialize(rec) {\n"
"  var s = rec.type + '/' + rec.subtype;\n"
"  for (var i = 0; i < rec.params.length; i++) {\n"
"    var pn = rec.params[i][0], pv = rec.params[i][1];\n"
"    s += ';' + pn + '=';\n"
"    s += isHTTPToken(pv) ? pv : ('\"' + pv.replace(/[\\\\\"]/g, '\\\\$&') + '\"');\n"
"  }\n"
"  return s;\n"
"}\n"
/* The `type` member: parse-then-serialize (mimesniff 4.4/4.5), '' on parse
 * failure. This USED to be a printable-ASCII check plus .toLowerCase() --
 * see the header above mimeParse for the seven distinct wrong answers that
 * produced, all fixed by routing through the real algorithm instead. */
"function blobNormType(t) {\n"
"  if (t === undefined || t === null) return '';\n"
"  var m = mimeParse(String(t));\n"
"  return m ? mimeSerialize(m) : '';\n"
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
