/* TextEncoder / TextDecoder, the Encoding Standard's object surface.
 *
 * A chunk of js_webapi.c's PRELUDE -- adjacent string literals, included in
 * the middle of that concatenation. It is a separate file because the thing it
 * replaced was thirty lines and this is four hundred, and js_webapi.c was
 * already 3,351.
 *
 * WHAT WAS HERE BEFORE, and why it read as working: a TextDecoder that set
 * `this.encoding = 'utf-8'` unconditionally, DISCARDED its label argument, and
 * decoded through QuickJS's own UTF-8 reader. For a UTF-8 page that is right,
 * which is why it survived. Everything else it got wrong silently -- and
 * silently is the operative word, because `new TextDecoder('shift_jis')` did
 * not fail, it returned an object claiming to be UTF-8 and then mojibake.
 *
 * THE DECODERS ARE STATE MACHINES OVER A STREAM, not functions over a buffer,
 * and that shape is forced by two requirements that look unrelated and are the
 * same one:
 *   - decode(chunk, {stream: true}) must carry a partial sequence across the
 *     call boundary. A three-byte CJK character split across two TCP reads is
 *     the ordinary case, not an edge case.
 *   - the UTF-8 error handling in the spec RESTORES a byte to the stream when
 *     a continuation byte turns out not to be one ("prepend"), so that
 *     E0 41 decodes as U+FFFD 'A' and not as U+FFFD alone. Getting this wrong
 *     is invisible on well-formed input and is most of what
 *     encoding/textdecoder-mistakes.any.js is about.
 * So every decoder here is an object with a `handler(byte)` and a small amount
 * of carried state, driven by one shared loop that owns the prepend queue, the
 * BOM sniff and the fatal flag. One loop, five handlers.
 *
 * WHAT IS NOT IMPLEMENTED, said plainly because a silent approximation here is
 * exactly the bug being removed: the multi-byte legacy CJK decoders (gbk,
 * gb18030, big5, euc-jp, iso-2022-jp, shift_jis, euc-kr). Their index tables
 * are tens of thousands of entries and are not vendored. Constructing one
 * WORKS and reports the right `encoding`, because that is what the label
 * lookup is; decoding falls back to UTF-8 and says so once, on the console,
 * naming the encoding. That is strictly better than what it replaced -- same
 * bytes out, but `encoding` is now honest and the fallback is audible instead
 * of silent -- and it is not the same thing as support. The corresponding WPT
 * files still fail and are not claimed.
 */

/* ---- the label lookup, shared by both constructors ---- */
"function encGetEncoding(label) {\n"
"  return __encLabel(label === undefined ? 'utf-8' : String(label));\n"
"}\n"

/* ---- byte-source coercion ----
 * BufferSource is ArrayBuffer or any view over one. A DataView and a
 * Float64Array are both legal input and both mean "these bytes", so the view's
 * element type is deliberately ignored and only its byte range is used. */
"function encToBytes(x) {\n"
"  if (x === null || x === undefined) return new Uint8Array(0);\n"
"  if (x instanceof Uint8Array) return x;\n"
"  if (x instanceof ArrayBuffer) return new Uint8Array(x);\n"
"  if (typeof SharedArrayBuffer === 'function' && x instanceof SharedArrayBuffer)\n"
"    return new Uint8Array(x);\n"
"  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);\n"
"  throw new TypeError('Failed to decode: the input is not a BufferSource');\n"
"}\n"

/* ---- the handlers ----
 * Each returns a code point, or one of these two sentinels. `ENC_CONT` means
 * "I consumed the byte and have no output yet"; `ENC_ERR` means "error", and
 * the driver decides whether that is U+FFFD or a TypeError. Negative numbers
 * so a code point never collides with them. */
"var ENC_CONT = -1, ENC_ERR = -2;\n"

/* UTF-8, https://encoding.spec.whatwg.org/#utf-8-decoder, transcribed rather
 * than reinvented. The lower/upper boundary pair is what rejects the
 * overlong forms and the surrogate range without a separate check afterwards:
 * E0 narrows the next byte to A0..BF, ED to 80..9F, F0 to 90..BF, F4 to 80..8F. */
"function Utf8Decoder() { this.needed = 0; this.cp = 0; this.seen = 0; this.lo = 0x80; this.hi = 0xBF; }\n"
"Utf8Decoder.prototype.reset = function () {\n"
"  this.needed = 0; this.cp = 0; this.seen = 0; this.lo = 0x80; this.hi = 0xBF;\n"
"};\n"
"Utf8Decoder.prototype.handler = function (st, byte) {\n"
"  if (byte === -1) {\n"
"    if (this.needed !== 0) { this.reset(); return ENC_ERR; }\n"
"    return ENC_CONT;\n"
"  }\n"
"  if (this.needed === 0) {\n"
"    if (byte <= 0x7F) return byte;\n"
"    if (byte >= 0xC2 && byte <= 0xDF) { this.needed = 1; this.cp = byte & 0x1F; }\n"
"    else if (byte >= 0xE0 && byte <= 0xEF) {\n"
"      if (byte === 0xE0) this.lo = 0xA0;\n"
"      if (byte === 0xED) this.hi = 0x9F;\n"
"      this.needed = 2; this.cp = byte & 0x0F;\n"
"    } else if (byte >= 0xF0 && byte <= 0xF4) {\n"
"      if (byte === 0xF0) this.lo = 0x90;\n"
"      if (byte === 0xF4) this.hi = 0x8F;\n"
"      this.needed = 3; this.cp = byte & 0x07;\n"
"    } else return ENC_ERR;\n"
"    return ENC_CONT;\n"
"  }\n"
"  if (byte < this.lo || byte > this.hi) {\n"
"    this.reset();\n"
"    st.prepend(byte);\n"       /* the restore -- see the file header */
"    return ENC_ERR;\n"
"  }\n"
"  this.lo = 0x80; this.hi = 0xBF;\n"
"  this.cp = (this.cp << 6) | (byte & 0x3F);\n"
"  this.seen++;\n"
"  if (this.seen !== this.needed) return ENC_CONT;\n"
"  var out = this.cp;\n"
"  this.reset();\n"
"  return out;\n"
"};\n"

/* UTF-16, both byte orders from one machine. The surrogate pairing is here and
 * not left to the string layer because an UNPAIRED lead followed by a
 * non-trail must restore BOTH bytes of the non-trail unit to the stream, which
 * only the decoder knows the order of. */
"function Utf16Decoder(be) { this.be = !!be; this.lead = null; this.leadSurr = null; }\n"
"Utf16Decoder.prototype.reset = function () { this.lead = null; this.leadSurr = null; };\n"
"Utf16Decoder.prototype.handler = function (st, byte) {\n"
"  if (byte === -1) {\n"
"    if (this.lead !== null || this.leadSurr !== null) { this.reset(); return ENC_ERR; }\n"
"    return ENC_CONT;\n"
"  }\n"
"  if (this.lead === null) { this.lead = byte; return ENC_CONT; }\n"
"  var unit = this.be ? ((this.lead << 8) | byte) : ((byte << 8) | this.lead);\n"
"  this.lead = null;\n"
"  if (this.leadSurr !== null) {\n"
"    var ls = this.leadSurr;\n"
"    this.leadSurr = null;\n"
"    if (unit >= 0xDC00 && unit <= 0xDFFF)\n"
"      return 0x10000 + ((ls - 0xD800) << 10) + (unit - 0xDC00);\n"
"    var b1 = unit >> 8, b2 = unit & 0xFF;\n"
"    if (this.be) { st.prepend(b1); st.prepend(b2); } else { st.prepend(b2); st.prepend(b1); }\n"
"    return ENC_ERR;\n"
"  }\n"
"  if (unit >= 0xD800 && unit <= 0xDBFF) { this.leadSurr = unit; return ENC_CONT; }\n"
"  if (unit >= 0xDC00 && unit <= 0xDFFF) return ENC_ERR;\n"
"  return unit;\n"
"};\n"

/* The 27 legacy single-byte encodings, one machine over a 128-entry table.
 * A 0 in the table is a hole, not U+0000 -- see the generator. */
"function SingleByteDecoder(idx) { this.idx = idx; }\n"
"SingleByteDecoder.prototype.reset = function () {};\n"
"SingleByteDecoder.prototype.handler = function (st, byte) {\n"
"  if (byte === -1) return ENC_CONT;\n"
"  if (byte <= 0x7F) return byte;\n"
"  var cp = this.idx[byte - 0x80];\n"
"  return cp === 0 ? ENC_ERR : cp;\n"
"};\n"

/* x-user-defined: the identity for ASCII, and the private-use block above it.
 * Two lines, and it is what a page reading binary through a text channel
 * asks for. */
"function XUserDefinedDecoder() {}\n"
"XUserDefinedDecoder.prototype.reset = function () {};\n"
"XUserDefinedDecoder.prototype.handler = function (st, byte) {\n"
"  if (byte === -1) return ENC_CONT;\n"
"  return byte <= 0x7F ? byte : 0xF780 + (byte - 0x80);\n"
"};\n"

/* `replacement`: the spec's decoder for the encodings that exist only so that
 * naming them cannot be used to smuggle content past a filter. It emits ONE
 * error for a non-empty stream and then finishes. */
"function ReplacementDecoder() { this.done = false; }\n"
"ReplacementDecoder.prototype.reset = function () { this.done = false; };\n"
"ReplacementDecoder.prototype.handler = function (st, byte) {\n"
"  if (byte === -1) return ENC_CONT;\n"
"  if (this.done) return ENC_CONT;\n"
"  this.done = true;\n"
"  return ENC_ERR;\n"
"};\n"

/* The unimplemented multi-byte legacy set. See the file header: this is a
 * UTF-8 decode wearing the right `encoding` name, and it says so once. */
"var ENC_MULTIBYTE = ['gbk','gb18030','big5','euc-jp','iso-2022-jp','shift_jis','euc-kr'];\n"
"function ApproxDecoder(name) { this.inner = new Utf8Decoder(); this.name = name; this.warned = false; }\n"
"ApproxDecoder.prototype.reset = function () { this.inner.reset(); };\n"
"ApproxDecoder.prototype.handler = function (st, byte) {\n"
"  if (!this.warned) {\n"
"    this.warned = true;\n"
"    if (G.console && G.console.warn)\n"
"      G.console.warn('TextDecoder: ' + this.name + ' is not implemented; decoding as UTF-8');\n"
"  }\n"
"  return this.inner.handler(st, byte);\n"
"};\n"

"function encMakeDecoder(name) {\n"
"  if (name === 'utf-8') return new Utf8Decoder();\n"
"  if (name === 'utf-16le') return new Utf16Decoder(false);\n"
"  if (name === 'utf-16be') return new Utf16Decoder(true);\n"
"  if (name === 'x-user-defined') return new XUserDefinedDecoder();\n"
"  if (name === 'replacement') return new ReplacementDecoder();\n"
"  var idx = __encIndex(name);\n"
"  if (idx) return new SingleByteDecoder(idx);\n"
"  return new ApproxDecoder(name);\n"
"}\n"

/* ---- code point -> UTF-16, appended to a chunk list ----
 * Accumulating into an array of String.fromCharCode batches and joining at the
 * end rather than `s += c`: a megabyte of decoded text is a megabyte of rope
 * nodes otherwise, and this runs on an emulated CPU. */
"function encPush(out, cp) {\n"
"  if (cp <= 0xFFFF) { out.push(String.fromCharCode(cp)); return; }\n"
"  cp -= 0x10000;\n"
"  out.push(String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)));\n"
"}\n"

/* ---- TextDecoder ---- */
"G.TextDecoder = function TextDecoder(label, options) {\n"
"  if (!(this instanceof TextDecoder))\n"
"    throw new TypeError(\"Constructor TextDecoder requires 'new'\");\n"
"  var name = encGetEncoding(label);\n"
"  if (name === null)\n"
"    throw new RangeError(\"The encoding label provided ('\" + String(label) + \"') is invalid.\");\n"
"  options = options || {};\n"
"  var enc = name;\n"
"  Object.defineProperty(this, 'encoding', { value: enc, enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'fatal', { value: !!options.fatal, enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'ignoreBOM', { value: !!options.ignoreBOM, enumerable: true, configurable: true });\n"
"  this._dec = null;\n"
"  this._bomSeen = false;\n"
"  this._doNotFlush = false;\n"
"};\n"

/* The one driver. Owns the prepend queue, the BOM sniff and the fatal flag, so
 * that every handler above is only the encoding's own logic. */
"G.TextDecoder.prototype.decode = function (input, options) {\n"
"  var stream = !!(options && options.stream);\n"
"  if (!this._doNotFlush) {\n"
"    this._dec = encMakeDecoder(this.encoding);\n"
"    this._bomSeen = false;\n"
"  }\n"
"  this._doNotFlush = stream;\n"
"  var bytes = encToBytes(input);\n"
"  var dec = this._dec, out = [], queue = [], self = this;\n"
"  var st = { prepend: function (b) { queue.push(b); } };\n"
"  var fatal = this.fatal;\n"
"\n"
/* The BOM sniff is on the STREAM, not on the call, so it has to survive a
 * chunk boundary that lands inside the mark. The pending buffer is at most two
 * bytes and only exists until the sniff resolves. */
"  var i = 0, n = bytes.length;\n"
"  var bomBytes = this.encoding === 'utf-8' ? [0xEF, 0xBB, 0xBF]\n"
"               : this.encoding === 'utf-16le' ? [0xFF, 0xFE]\n"
"               : this.encoding === 'utf-16be' ? [0xFE, 0xFF] : null;\n"
"  if (bomBytes && !this.ignoreBOM && !this._bomSeen) {\n"
"    if (this._bomBuf === undefined) this._bomBuf = [];\n"
"    while (this._bomBuf.length < bomBytes.length && i < n) this._bomBuf.push(bytes[i++]);\n"
"    var buf = this._bomBuf, k, match = true;\n"
"    for (k = 0; k < buf.length; k++) if (buf[k] !== bomBytes[k]) { match = false; break; }\n"
"    if (!match) { this._bomSeen = true; for (k = 0; k < buf.length; k++) queue.push(buf[k]); this._bomBuf = undefined; }\n"
"    else if (buf.length === bomBytes.length) { this._bomSeen = true; this._bomBuf = undefined; }\n"
"    else if (!stream) { this._bomSeen = true; for (k = 0; k < buf.length; k++) queue.push(buf[k]); this._bomBuf = undefined; }\n"
"    else { return ''; }\n"   /* the mark is still incomplete; wait for more */
"  }\n"
"\n"
"  function feed(b) {\n"
"    var r = dec.handler(st, b);\n"
"    if (r === ENC_CONT) return;\n"
"    if (r === ENC_ERR) {\n"
"      if (fatal) throw new TypeError('The encoded data was not valid for encoding ' + self.encoding + '.');\n"
"      out.push('\\uFFFD');\n"
"      return;\n"
"    }\n"
"    encPush(out, r);\n"
"  }\n"
"\n"
/* Byte order: anything the handler pushed back is reprocessed BEFORE the next
 * byte from the input, or E0 41 42 would come out as U+FFFD 'B' 'A'. */
"  while (true) {\n"
"    if (queue.length) { feed(queue.shift()); continue; }\n"
"    if (i >= n) break;\n"
"    feed(bytes[i++]);\n"
"  }\n"
"  if (!stream) {\n"
"    while (queue.length) feed(queue.shift());\n"
"    feed(-1);\n"
"    while (queue.length) feed(queue.shift());\n"
"    this._dec = null;\n"
"    this._bomBuf = undefined;\n"
"  }\n"
"  return out.join('');\n"
"};\n"
"Object.defineProperty(G.TextDecoder.prototype, Symbol.toStringTag,\n"
"  { value: 'TextDecoder', configurable: true });\n"

/* ---- TextEncoder ----
 * The label argument is accepted and IGNORED, which is not sloppiness: the
 * Encoding Standard removed encoder support for everything except UTF-8, and
 * `new TextEncoder('shift_jis').encoding === 'utf-8'` is a subtest. */
"G.TextEncoder = function TextEncoder() {\n"
"  if (!(this instanceof TextEncoder))\n"
"    throw new TypeError(\"Constructor TextEncoder requires 'new'\");\n"
"};\n"
"Object.defineProperty(G.TextEncoder.prototype, 'encoding',\n"
"  { get: function () { return 'utf-8'; }, enumerable: true, configurable: true });\n"
"Object.defineProperty(G.TextEncoder.prototype, Symbol.toStringTag,\n"
"  { value: 'TextEncoder', configurable: true });\n"

/* Scalar-value iteration: a lone surrogate is U+FFFD, not a hole and not an
 * error. Shared by encode and encodeInto so the two cannot disagree. */
"function encNextScalar(s, i) {\n"
"  var c = s.charCodeAt(i);\n"
"  if (c < 0xD800 || c > 0xDFFF) return [c, 1];\n"
"  if (c <= 0xDBFF && i + 1 < s.length) {\n"
"    var d = s.charCodeAt(i + 1);\n"
"    if (d >= 0xDC00 && d <= 0xDFFF) return [0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00), 2];\n"
"  }\n"
"  return [0xFFFD, 1];\n"
"}\n"
"function encUtf8Len(cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }\n"
"function encWriteUtf8(dst, at, cp) {\n"
"  if (cp < 0x80) { dst[at] = cp; return 1; }\n"
"  if (cp < 0x800) { dst[at] = 0xC0 | (cp >> 6); dst[at + 1] = 0x80 | (cp & 63); return 2; }\n"
"  if (cp < 0x10000) {\n"
"    dst[at] = 0xE0 | (cp >> 12); dst[at + 1] = 0x80 | ((cp >> 6) & 63); dst[at + 2] = 0x80 | (cp & 63);\n"
"    return 3;\n"
"  }\n"
"  dst[at] = 0xF0 | (cp >> 18); dst[at + 1] = 0x80 | ((cp >> 12) & 63);\n"
"  dst[at + 2] = 0x80 | ((cp >> 6) & 63); dst[at + 3] = 0x80 | (cp & 63);\n"
"  return 4;\n"
"}\n"
"G.TextEncoder.prototype.encode = function (input) {\n"
"  var s = input === undefined ? '' : String(input);\n"
"  var n = 0, i, r;\n"
"  for (i = 0; i < s.length; ) { r = encNextScalar(s, i); n += encUtf8Len(r[0]); i += r[1]; }\n"
"  var out = new Uint8Array(n), at = 0;\n"
"  for (i = 0; i < s.length; ) { r = encNextScalar(s, i); at += encWriteUtf8(out, at, r[0]); i += r[1]; }\n"
"  return out;\n"
"};\n"

/* encodeInto: the point of it is that it NEVER writes a partial sequence, so
 * the caller can treat `written` as a valid UTF-8 prefix without checking.
 * `read` counts UTF-16 code units consumed, which is why a surrogate pair that
 * does not fit leaves read at the value before it -- see the WPT case
 * "\u{1D306}A" into 3 bytes, which must report read 0 and write nothing. */
"G.TextEncoder.prototype.encodeInto = function (source, destination) {\n"
"  var s = source === undefined ? '' : String(source);\n"
"  if (!(destination instanceof Uint8Array))\n"
"    throw new TypeError('encodeInto requires a Uint8Array destination');\n"
"  var cap = destination.length, read = 0, written = 0, i = 0;\n"
"  while (i < s.length) {\n"
"    var r = encNextScalar(s, i), cp = r[0], units = r[1];\n"
"    var need = encUtf8Len(cp);\n"
"    if (written + need > cap) break;\n"
"    written += encWriteUtf8(destination, written, cp);\n"
"    read += units;\n"
"    i += units;\n"
"  }\n"
"  return { read: read, written: written };\n"
"};\n"

/* The stream pair. Unchanged in shape from what was here, but the decoder now
 * carries a label and its options, which is what made TextDecoderStream over a
 * non-UTF-8 response body meaningless before. */
"G.TextDecoderStream = function TextDecoderStream(label, options) {\n"
"  var dec = new G.TextDecoder(label, options);\n"
"  var t = new G.TransformStream({\n"
"    transform: function (chunk, c) { var s = dec.decode(chunk, { stream: true }); if (s) c.enqueue(s); },\n"
"    flush: function (c) { var s = dec.decode(new Uint8Array(0)); if (s) c.enqueue(s); }\n"
"  });\n"
"  Object.defineProperty(this, 'encoding', { value: dec.encoding, enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'fatal', { value: dec.fatal, enumerable: true, configurable: true });\n"
"  Object.defineProperty(this, 'ignoreBOM', { value: dec.ignoreBOM, enumerable: true, configurable: true });\n"
"  this.readable = t.readable; this.writable = t.writable;\n"
"};\n"
"G.TextEncoderStream = function TextEncoderStream() {\n"
"  var enc = new G.TextEncoder();\n"
"  var pendingHigh = '';\n"
/* A surrogate pair split across two chunks is one character, and encoding the
 * lead on its own would put U+FFFD in the middle of a sentence. */
"  var t = new G.TransformStream({\n"
"    transform: function (chunk, c) {\n"
"      var s = pendingHigh + String(chunk);\n"
"      pendingHigh = '';\n"
"      if (s.length) {\n"
"        var last = s.charCodeAt(s.length - 1);\n"
"        if (last >= 0xD800 && last <= 0xDBFF) { pendingHigh = s.charAt(s.length - 1); s = s.slice(0, -1); }\n"
"      }\n"
"      if (s.length) c.enqueue(enc.encode(s));\n"
"    },\n"
"    flush: function (c) { if (pendingHigh) c.enqueue(enc.encode(pendingHigh)); pendingHigh = ''; }\n"
"  });\n"
"  Object.defineProperty(this, 'encoding', { value: 'utf-8', enumerable: true, configurable: true });\n"
"  this.readable = t.readable; this.writable = t.writable;\n"
"};\n"

/* toU8/u8ab/joinParts stay: Response.arrayBuffer, Blob and the fetch body sink
 * all use them, and they predate this file. */
"function toU8(x) { return encToBytes(x); }\n"
"function u8ab(u) { return u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength); }\n"
"function joinParts(parts) {\n"
"  var a = [], n = 0, i;\n"
"  for (i = 0; i < parts.length; i++) {\n"
"    a.push(typeof parts[i] === 'string' ? new G.TextEncoder().encode(parts[i]) : toU8(parts[i]));\n"
"    n += a[i].length;\n"
"  }\n"
"  var out = new Uint8Array(n), o = 0;\n"
"  for (i = 0; i < a.length; i++) { out.set(a[i], o); o += a[i].length; }\n"
"  return out.buffer;\n"
"}\n"
