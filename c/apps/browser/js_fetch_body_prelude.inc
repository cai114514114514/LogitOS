/* Shared production prelude: each evaluation owns its closure and JSValues.
 * WEBAPI_FETCH_WORKER_PRELUDE excludes document-only additions. */
/* ---- Headers ----
 * Used to be a bare [name,value] array with no guard, no validation and
 * insertion-order iteration. Four spec mechanisms were simply absent -- see
 * the cluster this closed for the corpus evidence -- and the fix is guard +
 * validation + sorted/combined iteration + getSetCookie, in that order,
 * because the guard is what stops a page from reading Host/Cookie/Origin back
 * off a Request it built itself (fetch's kernel side already drops those
 * before they reach the wire; this object used to just lie about it). */
   /* mimesniff's HTTP-token helpers (isHTTPToken, stripHTTPWS, mimeParse) are
      already installed by js_blob_prelude.inc, above. */
"function hdrValidName(n) {\n"
"  var s = String(n);\n"
"  if (!isHTTPToken(s)) throw new TypeError(\"Invalid header name: '\" + s + \"'\");\n"
"  return s;\n"
"}\n"
   /* Header values are ByteString in the IDL (a byte sequence, not text) --
      any code unit above U+00FF must throw at the binding boundary, before
      the value-specific rule even runs. That is checked on the RAW value,
      before HTTP-whitespace stripping; the strip-then-check-for-NUL/CR/LF
      that follows is the spec's own order for the rest of the rule, so a
      bare \\n in the middle is still rejected even though leading/trailing
      whitespace is not. */
"function hdrValidValue(v) {\n"
"  v = String(v);\n"
"  for (var bi = 0; bi < v.length; bi++) if (v.charCodeAt(bi) > 255) throw new TypeError('Invalid header value');\n"
"  var s = stripHTTPWS(v, true, true);\n"
"  for (var i = 0; i < s.length; i++) {\n"
"    var c = s.charCodeAt(i);\n"
"    if (c === 0 || c === 10 || c === 13) throw new TypeError('Invalid header value');\n"
"  }\n"
"  return s;\n"
"}\n"
"var FORBIDDEN_REQ_HDR = ['accept-charset', 'accept-encoding', 'access-control-request-headers',\n"
"  'access-control-request-method', 'connection', 'content-length', 'cookie', 'cookie2', 'date', 'dnt',\n"
"  'expect', 'host', 'keep-alive', 'origin', 'referer', 'set-cookie', 'te', 'trailer', 'transfer-encoding',\n"
"  'upgrade', 'via'];\n"
"var FORBIDDEN_METHODS = ['connect', 'trace', 'track'];\n"
   /* The override header's value can be a comma-separated list ("GET,track ");
      ANY element naming a forbidden method makes the whole header forbidden,
      not just an exact single-method match -- \"trace,\" (trailing comma, one
      empty element) and \"GET,track \" (trailing space on the second element)
      are both in the corpus specifically to catch a version that only
      compares the trimmed WHOLE string. */
"function isForbiddenMethod(v) {\n"
"  var parts = String(v).split(',');\n"
"  for (var i = 0; i < parts.length; i++)\n"
"    if (FORBIDDEN_METHODS.indexOf(stripHTTPWS(parts[i], true, true).toLowerCase()) >= 0) return true;\n"
"  return false;\n"
"}\n"
   /* The X-HTTP-Method(-Override) trio is forbidden only when its VALUE names
      a forbidden method -- so this is a name+value check, not a name-only
      one, and it is why delete() (which has no value) treats it as allowed. */
"function isForbiddenReqHeaderName(n, v) {\n"
"  var k = n.toLowerCase();\n"
"  if (FORBIDDEN_REQ_HDR.indexOf(k) >= 0) return true;\n"
"  if (k.slice(0, 6) === 'proxy-' || k.slice(0, 4) === 'sec-') return true;\n"
"  if (k === 'x-http-method' || k === 'x-http-method-override' || k === 'x-method-override')\n"
"    return v !== undefined && isForbiddenMethod(v);\n"
"  return false;\n"
"}\n"
   /* CORS-unsafe-request-header-byte (fetch #cors-unsafe-request-header-byte):
      a control byte other than TAB, or one of the punctuation bytes that make
      a header value look like it is trying to smuggle syntax. */
"function isCorsUnsafeByte(c) {\n"
"  if (c < 0x20 && c !== 9) return true;\n"
"  return c === 0x22 || c === 0x28 || c === 0x29 || c === 0x3A || c === 0x3C || c === 0x3E ||\n"
"         c === 0x3F || c === 0x40 || c === 0x5B || c === 0x5C || c === 0x5D || c === 0x7B ||\n"
"         c === 0x7D || c === 0x7F;\n"
"}\n"
"function hasCorsUnsafeByte(v) {\n"
"  for (var i = 0; i < v.length; i++) if (isCorsUnsafeByte(v.charCodeAt(i))) return true;\n"
"  return false;\n"
"}\n"
"function isCorsSafelistedLangValue(v) {\n"
"  for (var i = 0; i < v.length; i++) {\n"
"    var c = v.charCodeAt(i);\n"
"    if (!((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) ||\n"
"          c === 32 || c === 42 || c === 44 || c === 45 || c === 46 || c === 59 || c === 61)) return false;\n"
"  }\n"
"  return true;\n"
"}\n"
"function isCorsSafelistedReqHeader(n, v) {\n"
"  var k = String(n).toLowerCase(); v = String(v);\n"
"  if (v.length > 128) return false;\n"
"  if (k === 'accept') return !hasCorsUnsafeByte(v);\n"
"  if (k === 'accept-language' || k === 'content-language') return isCorsSafelistedLangValue(v);\n"
"  if (k === 'content-type') {\n"
"    if (hasCorsUnsafeByte(v)) return false;\n"
"    var m = mimeParse(v);\n"
"    if (!m) return false;\n"
"    var ess = m.type + '/' + m.subtype;\n"
"    return ess === 'application/x-www-form-urlencoded' || ess === 'multipart/form-data' || ess === 'text/plain';\n"
"  }\n"
"  return false;\n"
"}\n"
   /* Whether a `set` (or an `append`'s OWN value, before combining -- see
      below) with this guard must be silently dropped. 'immutable' is handled
      separately by the callers below -- it THROWS rather than drops, per
      spec, so it is not folded in here. */
"function hdrGuardDrops(guard, n, v) {\n"
"  if (guard === 'request') return isForbiddenReqHeaderName(n, v);\n"
"  if (guard === 'request-no-cors') return !isCorsSafelistedReqHeader(n, v);\n"
"  if (guard === 'response') { var k = n.toLowerCase(); return k === 'set-cookie' || k === 'set-cookie2'; }\n"
"  return false;\n"
"}\n"
   /* delete()'s guard rule is a STRICT SUBSET of set()'s: the spec's "delete a
      header" algorithm has cases for 'request' and 'response' but NONE for
      'request-no-cors' -- a no-cors Headers object still allows deleting a
      header it would have refused to ADD. Folding this into hdrGuardDrops
      would make delete() silently keep a header the spec says must go. */
"function hdrGuardDropsDelete(guard, n) {\n"
"  if (guard === 'request') return isForbiddenReqHeaderName(n, undefined);\n"
"  if (guard === 'response') { var k = n.toLowerCase(); return k === 'set-cookie' || k === 'set-cookie2'; }\n"
"  return false;\n"
"}\n"
   /* Sorted-and-combined view (fetch #concept-header-list-sort-and-combine):
      every iteration surface goes through this, and it is why Set-Cookie is
      the one name that yields ONE ENTRY PER OCCURRENCE instead of a single
      ', '-joined string -- combining Set-Cookie values the way every other
      header combines is not merely wrong formatting, it is unparsiable back
      into separate cookies. */
"function hdrSortedCombined(l) {\n"
#ifdef WEBAPI_HEADERS_RAW_ITERATION
   /* Negative control for the actual fetch/Headers consumer. The old
      webapi_test expected two forEach callbacks for two X-Trace fields even
      though Fetch requires one combined entry. Returning the raw list revives
      that wrong iteration contract without dropping either wire value. */
"  return l.slice();\n"
#endif
"  var names = [];\n"
"  for (var i = 0; i < l.length; i++) { var k = l[i][0].toLowerCase(); if (names.indexOf(k) < 0) names.push(k); }\n"
"  names.sort();\n"
"  var out = [];\n"
"  for (i = 0; i < names.length; i++) {\n"
"    var k2 = names[i];\n"
"    if (k2 === 'set-cookie') {\n"
"      for (var j = 0; j < l.length; j++) if (l[j][0].toLowerCase() === 'set-cookie') out.push([k2, l[j][1]]);\n"
"    } else {\n"
"      var vs = [];\n"
"      for (j = 0; j < l.length; j++) if (l[j][0].toLowerCase() === k2) vs.push(l[j][1]);\n"
"      out.push([k2, vs.join(', ')]);\n"
"    }\n"
"  }\n"
"  return out;\n"
"}\n"
"G.Headers = function Headers(init) {\n"
"  this._l = []; this._guard = 'none';\n"
"  if (init !== undefined && init !== null) {\n"
"    if (Array.isArray(init)) {\n"
"      for (var i = 0; i < init.length; i++) {\n"
"        var pair = init[i];\n"
"        if (pair === null || typeof pair !== 'object' || typeof pair.length !== 'number' || pair.length !== 2)\n"
"          throw new TypeError('Headers: each init entry must be a name/value pair');\n"
"        this.append(pair[0], pair[1]);\n"
"      }\n"
"    } else if (init instanceof G.Headers) {\n"
"      for (var j = 0; j < init._l.length; j++) this.append(init._l[j][0], init._l[j][1]);\n"
"    } else if (typeof init.forEach === 'function') {\n"
"      var self = this; init.forEach(function (v, k) { self.append(k, v); });\n"
       /* Own enumerable string keys only -- NOT for...in, which would also
          walk the prototype chain and pick up keys the caller never wrote. */
"    } else {\n"
"      var keys = Object.keys(init);\n"
"      for (var kk = 0; kk < keys.length; kk++) this.append(keys[kk], init[keys[kk]]);\n"
"    }\n"
"  }\n"
"};\n"
"G.Headers.prototype = {\n"
"  constructor: G.Headers,\n"
"  append: function (n, v) {\n"
"    n = hdrValidName(n); v = hdrValidValue(v);\n"
"    if (this._guard === 'immutable') throw new TypeError('Headers is immutable');\n"
     /* request-no-cors checks the COMBINED value (existing + ', ' + new), not
        the new value alone -- appending "" onto an already-127-byte-long
        Accept value must be refused because the RESULT would be 129 bytes,
        even though "" by itself passes every per-value rule trivially. Get
        this wrong and two safelisted appends in a row silently smuggle a
        combined value that was never checked as a whole. */
"    if (this._guard === 'request-no-cors') {\n"
"      var existing = this.get(n);\n"
"      var combined = existing === null ? v : existing + ', ' + v;\n"
"      if (!isCorsSafelistedReqHeader(n, combined)) return;\n"
"    } else if (hdrGuardDrops(this._guard, n, v)) return;\n"
"    this._l.push([n, v]);\n"
"  },\n"
"  set: function (n, v) {\n"
"    n = hdrValidName(n); v = hdrValidValue(v);\n"
"    if (this._guard === 'immutable') throw new TypeError('Headers is immutable');\n"
"    if (hdrGuardDrops(this._guard, n, v)) return;\n"
"    var k = n.toLowerCase();\n"
"    this._l = this._l.filter(function (p) { return p[0].toLowerCase() !== k; });\n"
"    this._l.push([n, v]);\n"
"  },\n"
"  delete: function (n) {\n"
"    n = hdrValidName(n);\n"
"    if (this._guard === 'immutable') throw new TypeError('Headers is immutable');\n"
"    if (!this.has(n)) return;\n"
"    if (hdrGuardDropsDelete(this._guard, n)) return;\n"
"    var k = n.toLowerCase();\n"
"    this._l = this._l.filter(function (p) { return p[0].toLowerCase() !== k; });\n"
"  },\n"
"  has: function (n) { n = hdrValidName(n); var k = n.toLowerCase();\n"
"    return this._l.some(function (p) { return p[0].toLowerCase() === k; }); },\n"
   /* Several same-named headers join with ', ', which is what the spec says
      and what makes Set-Cookie the one header you must not read this way --
      get('set-cookie') still joins (the spec requires it, for callers that
      have not been updated to getSetCookie), but nothing else in this file
      should read Set-Cookie through get(). */
"  get: function (n) { n = hdrValidName(n); var k = n.toLowerCase();\n"
"    var v = this._l.filter(function (p) { return p[0].toLowerCase() === k; }).map(function (p) { return p[1]; });\n"
"    return v.length ? v.join(', ') : null; },\n"
"  getSetCookie: function () {\n"
"    return this._l.filter(function (p) { return p[0].toLowerCase() === 'set-cookie'; }).map(function (p) { return p[1]; });\n"
"  },\n"
"  forEach: function (fn, t) { var s = this; hdrSortedCombined(this._l).forEach(function (p) { fn.call(t, p[1], p[0], s); }); },\n"
"  keys: function () { return hdrSortedCombined(this._l).map(function (p) { return p[0]; })[Symbol.iterator](); },\n"
"  values: function () { return hdrSortedCombined(this._l).map(function (p) { return p[1]; })[Symbol.iterator](); },\n"
"  entries: function () { return hdrSortedCombined(this._l)[Symbol.iterator](); }\n"
"};\n"
"G.Headers.prototype[Symbol.iterator] = G.Headers.prototype.entries;\n"
   /* Symbol.toStringTag: Object.prototype.toString on a Headers used to read
      '[object Object]'. That is not cosmetic -- axios's kindOfTest and every
      library built the same way (isXxx(v) = Object.prototype.toString.call(v)
      === '[object Xxx]') use exactly this string to answer "is this a
      Headers/Request/Response/ReadableStream", and gate real behaviour on the
      answer. See the matching tags on Response/Request/ReadableStream below;
      this is the same fix, once per interface, done where each prototype is
      already being finished. */
"Object.defineProperty(G.Headers.prototype, Symbol.toStringTag, { value: 'Headers', configurable: true });\n"

/* ---- streams ----
 * A real ReadableStream, because Response.body has to be one and because a
 * chat reply is a stream before it is a string. The queue is a plain array and
 * a byte count; the byte count is what C reads back from push() to decide
 * whether to keep reading the socket, so it is load-bearing rather than
 * decorative. */
   /* Captured HERE, before any page script has had a chance to run, so it is
      the REAL Promise.prototype.then and not whatever a page later assigns
      to that name. getReader() below uses it to mark a reader's `closed`
      promise as handled without going through the (patchable) exposed
      `.then`/`.catch` -- calling those instead makes `rs.pipeTo(ws)` itself
      throw synchronously on a page that has patched Promise.prototype.then
      (streams/readable-streams/patched-global.any.js does exactly this),
      because getReader() runs synchronously at the top of pipeTo(), outside
      the `new Promise` executor that would otherwise swallow it. */
"var __nativeThen = Promise.prototype.then;\n"
"function chLen(c) { return c == null ? 0 : (c.byteLength !== undefined ? c.byteLength : (c.length || 0)); }\n"
"function rsPut(s, ch) {\n"
"  if (s._st !== 'readable') return;\n"
"  if (s._w.length) { s._w.shift()[0]({ value: ch, done: false }); return; }\n"
"  s._q.push(ch); s._qb += chLen(ch);\n"
"}\n"
   /* rsClosedSettle notifies every watcher of reader.closed that the stream
      has finished -- fulfilled on a normal close, rejected with the same
      error on an error. It is the piece that was entirely missing: before
      this, `closed` was `new Promise(function () {})`, an executor that
      captures neither resolve nor reject, so nothing anywhere could ever
      settle it and a page awaiting it hung forever with no error and no log
      line (see the header of this finding in the tree's own notes). */
"function rsClosedSettle(s) {\n"
"  var w = s._cw; s._cw = [];\n"
"  for (var i = 0; i < w.length; i++) {\n"
"    if (s._st === 'errored') w[i][1](s._e); else w[i][0](undefined);\n"
"  }\n"
"}\n"
"function rsEnd(s) {\n"
"  if (s._st !== 'readable') return;\n"
"  s._st = 'closed';\n"
"  while (s._w.length) s._w.shift()[0]({ value: undefined, done: true });\n"
"  rsClosedSettle(s);\n"
"}\n"
"function rsErr(s, e) {\n"
"  if (s._st !== 'readable') { return; }\n"
"  s._st = 'errored'; s._e = e;\n"
"  while (s._w.length) s._w.shift()[1](e);\n"
"  s._q = []; s._qb = 0;\n"
"  rsClosedSettle(s);\n"
"}\n"
   /* pull() may be an async function -- its return value is a promise, and a
      SYNCHRONOUS throw inside it never happens; the rejection surfaces only
      through that promise. Before this fix the promise pull() returned was
      simply dropped, so an async pull that rejected settled nothing: the
      reader's read() stayed pending forever, with no error and no log line
      (measured against axios's trackStream, whose pull ends
      `.catch(err => { onFinish(err); throw err })` -- exactly this shape, and
      exactly the shape a mid-stream network failure produces). Route it to
      rsErr the same way a synchronous throw already is. */
"function rsPull(s) {\n"
"  if (typeof s._src.pull !== 'function') return;\n"
"  try {\n"
"    var __rp = s._src.pull(s._c);\n"
"    if (__rp && typeof __rp.then === 'function') __rp.then(undefined, function (e) { rsErr(s, e); });\n"
"  } catch (e) { rsErr(s, e); }\n"
"}\n"
"function rsRead(s) {\n"
"  if (s._q.length) { var c = s._q.shift(); s._qb -= chLen(c); if (s._qb < 0) s._qb = 0;\n"
"    rsPull(s); return Promise.resolve({ value: c, done: false }); }\n"
"  if (s._st === 'closed') return Promise.resolve({ value: undefined, done: true });\n"
"  if (s._st === 'errored') return Promise.reject(s._e);\n"
"  return new Promise(function (res, rej) { s._w.push([res, rej]); rsPull(s); });\n"
"}\n"
"function rsCancel(s, reason) {\n"
"  if (s._st === 'readable') { s._st = 'closed';\n"
"    while (s._w.length) s._w.shift()[0]({ value: undefined, done: true });\n"
"    rsClosedSettle(s); }\n"
"  s._q = []; s._qb = 0;\n"
"  if (typeof s._src.cancel === 'function') { try { s._src.cancel(reason); } catch (e) {} }\n"
"  return Promise.resolve();\n"
"}\n"
"G.ReadableStream = function ReadableStream(src) {\n"
"  var s = this;\n"
"  s._src = src || {}; s._q = []; s._qb = 0; s._st = 'readable'; s._e = undefined;\n"
"  s._w = []; s._cw = []; s._locked = false;\n"
"  s._c = {\n"
"    enqueue: function (ch) { rsPut(s, ch); },\n"
"    close: function () { rsEnd(s); },\n"
"    error: function (e) { rsErr(s, e); },\n"
"    bytes: function () { return s._qb; },\n"
"    get desiredSize() { return 65536 - s._qb; }\n"
"  };\n"
"  if (typeof s._src.start === 'function') s._src.start(s._c);\n"
"};\n"
"G.ReadableStream.prototype = {\n"
"  constructor: G.ReadableStream,\n"
"  get locked() { return this._locked; },\n"
   /* getReader() gives the reader a real identity (`released`/`pair`) rather
      than just flipping the stream's lock bit, because release() has to undo
      exactly what THIS acquisition did: reject the reads THIS reader left
      pending and settle THIS reader's `closed`, in a stream a second reader
      may already be locking again by the time release() runs (spec:
      ReadableStreamDefaultReaderRelease). `closedP` is built already-settled
      when the stream finished before acquisition; otherwise its [res, rej]
      pair is parked on s._cw for rsClosedSettle to find later.
      `closedP` is a `var`, not a `const`, because the spec's release
      algorithm has TWO cases and they are not the same case: if the stream
      is still "readable" when release() runs, the EXISTING closedPromise is
      rejected in place (same identity, now settled); otherwise (already
      closed or errored) it is REPLACED with a brand-new rejected promise,
      because the old one is already fulfilled/rejected and cannot be
      un-settled. WPT tests this identity distinction directly
      (default-reader.any.js "closed is replaced when stream closes and
      reader releases its lock", templated.any.js "releasing the lock should
      cause closed to reject and change identity") -- getting the branch
      backwards is invisible to any test that only checks `.closed` rejects,
      which is why it is called out here rather than left for the next
      reader to rediscover. */
"  getReader: function () {\n"
"    if (this._locked) throw new TypeError('ReadableStream is locked');\n"
"    var s = this; s._locked = true;\n"
"    var released = false, pair = null;\n"
"    var closedP = new Promise(function (res, rej) {\n"
"      if (s._st === 'closed') { res(undefined); return; }\n"
"      if (s._st === 'errored') { rej(s._e); return; }\n"
"      pair = [res, rej]; s._cw.push(pair);\n"
"    });\n"
     /* Mark `closedP` handled through the CAPTURED native then, not the
        exposed (patchable) one -- see __nativeThen's own comment above. */
"    __nativeThen.call(closedP, undefined, function () {});\n"
"    function release() {\n"
"      if (released) return; released = true; s._locked = false;\n"
"      var e = new TypeError('Reader was released');\n"
"      while (s._w.length) s._w.shift()[1](e);\n"
"      if (s._st === 'readable') {\n"
"        if (pair) { var i = s._cw.indexOf(pair); if (i >= 0) s._cw.splice(i, 1); pair[1](e); }\n"
"      } else {\n"
"        closedP = Promise.reject(e);\n"
"        __nativeThen.call(closedP, undefined, function () {});\n"
"      }\n"
"    }\n"
"    return {\n"
"      read: function () {\n"
"        if (released) return Promise.reject(new TypeError('Reader was released'));\n"
"        return rsRead(s);\n"
"      },\n"
     /* reader.cancel() does NOT release the reader -- ReadableStreamDefaultReaderCancel
        is a thin wrapper over ReadableStreamCancel and never touches the lock.
        The stream stays locked to THIS reader until an explicit releaseLock(),
        same as a real browser: `await reader.cancel(); stream.locked` is still
        true. This used to unlock unconditionally and mark the reader released
        right here, which made a later releaseLock() a no-op (release()'s
        `if (released) return` guard) -- and that silently skipped the
        identity-replace `closed` needs on release (see getReader's own
        comment on `closedP`), which is exactly what
        templated.any.js's "(closed via cancel after getting reader):
        releasing the lock should cause closed to reject and change identity"
        checks. */
"      cancel: function (r) {\n"
"        if (released) return Promise.reject(new TypeError('Reader was released'));\n"
"        return rsCancel(s, r);\n"
"      },\n"
"      releaseLock: function () { release(); },\n"
"      get closed() { return closedP; }\n"
"    };\n"
"  },\n"
"  cancel: function (r) { return rsCancel(this, r); },\n"
"  tee: function () {\n"
"    var cs = [null, null];\n"
"    var out = [new G.ReadableStream({ start: function (c) { cs[0] = c; } }),\n"
"               new G.ReadableStream({ start: function (c) { cs[1] = c; } })];\n"
"    var rd = this.getReader();\n"
"    (function loop() {\n"
"      rd.read().then(function (r) {\n"
"        if (r.done) { cs[0].close(); cs[1].close(); return; }\n"
"        cs[0].enqueue(r.value); cs[1].enqueue(r.value); loop();\n"
"      }, function (e) { cs[0].error(e); cs[1].error(e); });\n"
"    })();\n"
"    return out;\n"
"  },\n"
"  pipeTo: function (w) {\n"
"    var rd = this.getReader(), wr = w.getWriter();\n"
"    return new Promise(function (res, rej) {\n"
"      (function loop() {\n"
"        rd.read().then(function (r) {\n"
"          if (r.done) { Promise.resolve(wr.close()).then(res, rej); return; }\n"
"          Promise.resolve(wr.write(r.value)).then(loop, rej);\n"
"        }, function (e) { try { wr.abort(e); } catch (x) {} rej(e); });\n"
"      })();\n"
"    });\n"
"  },\n"
"  pipeThrough: function (t) { this.pipeTo(t.writable); return t.readable; }\n"
"};\n"
"Object.defineProperty(G.ReadableStream.prototype, Symbol.toStringTag, { value: 'ReadableStream', configurable: true });\n"
"if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) {\n"
"  G.ReadableStream.prototype[Symbol.asyncIterator] = function () {\n"
"    var rd = this.getReader();\n"
     /* return() cancels AND releases -- spec's CreateReadableStreamAsyncIterator
        returnSteps do both (ReadableStreamReaderGenericCancel then
        ReadableStreamReaderGenericRelease), synchronously one after the
        other, not waiting for the cancel promise to settle. Before
        reader.cancel() stopped auto-releasing (see cancel()'s own comment
        above), this line got the release for free as cancel()'s side effect;
        now that cancel() no longer does that, a `for await` loop that
        `break`s would otherwise leave the stream locked forever. */
"    return { next: function () { return rd.read(); },\n"
"             'return': function () { rd.cancel(); rd.releaseLock(); return Promise.resolve({ done: true }); },\n"
"             '@@asyncIterator': function () { return this; } };\n"
"  };\n"
"}\n"
"G.WritableStream = function WritableStream(sink) { this._sink = sink || {}; this._locked = false; };\n"
"G.WritableStream.prototype = {\n"
"  constructor: G.WritableStream,\n"
"  get locked() { return this._locked; },\n"
"  getWriter: function () {\n"
"    var s = this; s._locked = true;\n"
"    return {\n"
"      write: function (c) { return Promise.resolve(s._sink.write ? s._sink.write(c) : undefined); },\n"
"      close: function () { return Promise.resolve(s._sink.close ? s._sink.close() : undefined); },\n"
"      abort: function (e) { return Promise.resolve(s._sink.abort ? s._sink.abort(e) : undefined); },\n"
"      releaseLock: function () { s._locked = false; },\n"
"      ready: Promise.resolve(), closed: Promise.resolve()\n"
"    };\n"
"  }\n"
"};\n"
"G.TransformStream = function TransformStream(t) {\n"
"  t = t || {};\n"
"  var rc = null;\n"
"  this.readable = new G.ReadableStream({ start: function (c) { rc = c; } });\n"
"  var tc = { enqueue: function (c) { rc.enqueue(c); },\n"
"             terminate: function () { rc.close(); },\n"
"             error: function (e) { rc.error(e); } };\n"
"  this.writable = new G.WritableStream({\n"
"    write: function (chunk) { return Promise.resolve(t.transform ? t.transform(chunk, tc) : tc.enqueue(chunk)); },\n"
"    close: function () { return Promise.resolve(t.flush ? t.flush(tc) : undefined).then(function () { rc.close(); }); },\n"
"    abort: function (e) { rc.error(e); }\n"
"  });\n"
"  if (typeof t.start === 'function') t.start(tc);\n"
"};\n"

/* ---- bytes <-> text: TextEncoder / TextDecoder ----
 * Four hundred lines of Encoding Standard, in its own file. The thirty that
 * used to be here decoded UTF-8 and ignored the label argument entirely --
 * see the header of js_encoding_prelude.inc for what that cost and for what
 * is still not implemented. */
#include "js_encoding_prelude.inc"

/* ---- document.fonts ----
 * Ranked second in the corpus by files killed (182), behind only `CSS`, and
 * every one of them dies on `document.fonts.ready.then(...)` before its first
 * assertion. See the file header for why a promise that resolves immediately
 * is the true answer in this engine and not a stub. */
#ifndef WEBAPI_FETCH_WORKER_PRELUDE
#include "js_fontface_prelude.inc"
#endif

/* ---- Blob / File ----
 * Real ones, not a fallback: see js_blob_prelude.inc's header for why this is
 * the file that gets to define G.Blob first. Needs TextEncoder (just
 * installed above) and __utf8/u8ab/rsOf (the last is a function DECLARATION
 * further down, hoisted to the top of this whole prelude function -- so
 * referencing it here, before its own text, is not a forward-reference bug). */
#include "js_blob_prelude.inc"

/* ---- AbortController ----
 * A real cancellation now that the socket ABI has a close: abort() shuts the
 * connection, it does not merely stop looking at what arrives. */
   /* A real DOMException, not a plain Error wearing a `.name` property that
      only LOOKS like one. `e instanceof DOMException` is the guard every
      abort-aware fetch wrapper writes (`catch (e) { if (e instanceof
      DOMException && e.name === 'AbortError') return; throw e; }`), and a
      plain Error fails it, so a deliberate cancellation rethrows as an
      uncaught error. `G.DOMException` is installed by js_platform.c, which
      this file's OWN comment above js_webapi_install (search "Layering,
      load-bearing") says runs AFTER js_webapi_install -- but that only
      matters for code that runs at INSTALL time; abortError() is a function
      body, not evaluated until a page actually calls .abort(), by which
      point every install is long done. `|| TypeError` is the same
      already-fallen-back-to-here shape as URL.createObjectURL's quota
      errors above, for the pathological case of a fetch that aborts before
      install finished. */
/* Correction 2026-09-10: the fallback above renamed a cancellation to
 * TypeError: Error constructors ignore the second DOMException name argument.
 * The WebAPI-only stream gate exposed this on all three abort boundaries.
 * Use one constructor policy; a real DOMException is still used when present. */
"function abortError() { return mkError('AbortError', 'The operation was aborted.'); }\n"
"G.AbortSignal = function AbortSignal() { this.aborted = false; this.reason = undefined;\n"
"  this.onabort = null; this._l = []; };\n"
"G.AbortSignal.prototype = {\n"
"  constructor: G.AbortSignal,\n"
"  addEventListener: function (t, f) { if (t === 'abort' && typeof f === 'function') this._l.push(f); },\n"
"  removeEventListener: function (t, f) { var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1); },\n"
"  throwIfAborted: function () { if (this.aborted) throw this.reason; },\n"
"  dispatchEvent: function () { return true; }\n"
"};\n"
"G.AbortController = function AbortController() { this.signal = new G.AbortSignal(); };\n"
"G.AbortController.prototype.abort = function (reason) {\n"
"  var s = this.signal;\n"
"  if (s.aborted) return;\n"
"  s.aborted = true;\n"
"  s.reason = reason !== undefined ? reason : abortError();\n"
"  var e = { type: 'abort', target: s };\n"
"  if (typeof s.onabort === 'function') s.onabort(e);\n"
"  s._l.slice().forEach(function (f) { f.call(s, e); });\n"
"};\n"

/* ---- the Body mixin, and extractBody ----
 * Shared between Response and Request (N2): before this, Response had
 * _drain/arrayBuffer/text/json/blob/clone and Request had NONE of it -- no
 * prototype at all, so `new Request(u).blob` was inherited from
 * Object.prototype and `new Request(u).blob()` threw "blob is not a
 * function". The two were never going to agree by construction once they
 * were two separately hand-maintained copies, so this is one copy installed
 * onto both prototypes. bytes() is new here too -- neither side had it. */
"function rsOf(chunk) { return new G.ReadableStream({ start: function (c) { c.enqueue(chunk); c.close(); } }); }\n"
"function bytesIndexOf(hay, needle, from) {\n"
"  outer: for (var i = from; i <= hay.length - needle.length; i++) {\n"
"    for (var j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;\n"
"    return i;\n"
"  }\n"
"  return -1;\n"
"}\n"
   /* multipart/form-data (RFC 7578) at the byte level: each part is framed by
      CRLF "--" boundary [CRLF | "--"], a CRLF-terminated header block, CRLF,
      then the payload, ending 2 bytes before the next delimiter (the CRLF
      that belongs to the delimiter, not the payload). Content-Disposition's
      name/filename are read with a bare quoted-string regex -- no backslash-
      unescaping -- which is the subset this draws the line at. */
   /* No opening delimiter at all (an empty body, or one that never contained
      the declared boundary) is a MALFORMED multipart body, not an empty-but-
      valid one -- it throws, which formData() (a .then() callback) turns
      into a rejected promise. An empty FormData is a legitimate RESULT of a
      well-formed body with zero parts; it is not what a missing delimiter
      means. */
"function parseMultipart(bytes, boundary, fd) {\n"
"  var db = new G.TextEncoder().encode('--' + boundary);\n"
"  var pos = bytesIndexOf(bytes, db, 0);\n"
"  if (pos < 0) throw new TypeError('formData: malformed multipart/form-data body');\n"
"  pos += db.length;\n"
"  for (;;) {\n"
"    if (bytes[pos] === 45 && bytes[pos + 1] === 45) return;\n"
"    if (bytes[pos] === 13 && bytes[pos + 1] === 10) pos += 2;\n"
"    var hdrEnd = bytesIndexOf(bytes, [13, 10, 13, 10], pos);\n"
"    if (hdrEnd < 0) return;\n"
"    var hdrText = bytes.length ? __utf8(u8ab(bytes.slice(pos, hdrEnd))) : '';\n"
"    pos = hdrEnd + 4;\n"
"    var nextDelim = bytesIndexOf(bytes, db, pos);\n"
"    if (nextDelim < 0) return;\n"
"    var bodyEnd = nextDelim;\n"
"    if (bytes[bodyEnd - 1] === 10 && bytes[bodyEnd - 2] === 13) bodyEnd -= 2;\n"
"    var partBytes = bytes.slice(pos, bodyEnd);\n"
"    var name = null, filename = null, partType = '';\n"
"    hdrText.split('\\r\\n').forEach(function (line) {\n"
"      var ci = line.indexOf(':');\n"
"      if (ci < 0) return;\n"
"      var hn = stripHTTPWS(line.slice(0, ci), true, true).toLowerCase();\n"
"      var hv = stripHTTPWS(line.slice(ci + 1), true, true);\n"
"      if (hn === 'content-disposition') {\n"
"        var nm = /;\\s*name=\"([^\"]*)\"/i.exec(hv); if (nm) name = nm[1];\n"
"        var fnm = /;\\s*filename=\"([^\"]*)\"/i.exec(hv); if (fnm) filename = fnm[1];\n"
"      } else if (hn === 'content-type') { partType = hv; }\n"
"    });\n"
"    if (name !== null) {\n"
"      if (filename !== null) fd.append(name, new G.File([partBytes], filename, { type: partType }), filename);\n"
"      else fd.append(name, partBytes.length ? __utf8(u8ab(partBytes)) : '');\n"
"    }\n"
"    pos = nextDelim + db.length;\n"
"  }\n"
"}\n"
   /* bodyUsed is a readonly IDL attribute on both interfaces -- a getter on
      the shared prototype, backed by a private field internal code writes
      directly (never through the public name, which has no setter). */
"function installBody(proto) {\n"
"  Object.defineProperty(proto, 'bodyUsed', {\n"
"    get: function () { return !!this._bodyUsed; }, enumerable: true, configurable: true });\n"
   /* bodyUsed's real definition (fetch #dom-body-bodyused) is "this's body is
      non-null AND its stream is disturbed" -- NOT "a consuming method was
      called". A bodyless Request/Response (no body: init member at all) can
      have .text()/.blob()/etc called on it any number of times and bodyUsed
      stays false throughout, because there was never a stream to disturb.
      Setting the flag unconditionally at the top of drain (rather than only
      once a real body exists) is exactly the bug the corpus's own
      request-consume-empty.any.js is built to catch: repeated `assert_false
      (request.bodyUsed)` after consuming a request that had no body. */
"  proto._drain = function () {\n"
"    if (this._bodyUsed) return Promise.reject(new TypeError('body already read'));\n"
"    var b = this.body;\n"
"    if (!b) return Promise.resolve([]);\n"
"    this._bodyUsed = true;\n"
"    var rd = b.getReader(), parts = [];\n"
"    return new Promise(function (res, rej) {\n"
"      (function loop() {\n"
"        rd.read().then(function (r) {\n"
"          if (r.done) { res(parts); return; }\n"
"          parts.push(r.value); loop();\n"
"        }, rej);\n"
"      })();\n"
"    });\n"
"  };\n"
"  proto.arrayBuffer = function () { return this._drain().then(joinParts); };\n"
"  proto.text = function () { return this._drain().then(function (p) {\n"
"    var ab = joinParts(p); return ab.byteLength ? __utf8(ab) : ''; }); };\n"
#if defined(JS_RUNTIME_DIAGNOSTICS) && !defined(WEBAPI_FETCH_WORKER_PRELUDE)
/* Inspect the already-consumed text through a private native parser. Never
 * walk the page-visible parse result or consult public status/header getters;
 * its network metadata was recorded before the page received the Response.
 * This still calls the original public text()/JSON.parse path exactly once. */
"  proto.json = function () { var response = this; return this.text().then(function (t) {\n"
"    var meta = xhrDiagGet(xhrResponseDiag, response);\n"
"    if (meta && meta.status >= 400 && meta.status < 500 && typeof t === 'string' && t.length <= 65536)\n"
"      __xhrDiag(5, 0, meta.fetch, meta.status, t);\n"
"    return JSON.parse(t);\n"
"  }); };\n"
#else
"  proto.json = function () { return this.text().then(function (t) { return JSON.parse(t); }); };\n"
#endif
   /* A real Blob, not an ArrayBuffer wearing a promise -- .blob()'s type must
      carry Content-Type, and code that does `blob instanceof Blob` (or feeds
      it straight back into `new Blob([...])`) needs the real class. The type
      goes through G.Blob's own constructor, which runs it through
      blobNormType (mimeParse + mimeSerialize, N3) -- so this already gets the
      spec's "parse the header, serialize the record" behaviour for free. */
"  proto.blob = function () { var self = this; return this._drain().then(function (p) {\n"
"    return new G.Blob([joinParts(p)], { type: self.headers.get('content-type') || '' }); }); };\n"
"  proto.bytes = function () { return this._drain().then(function (p) { return new Uint8Array(joinParts(p)); }); };\n"
   /* formData(): the two BodyInit encodings a form actually produces. Content-
      Type decides which -- application/x-www-form-urlencoded is decoded as
      text (it is one), multipart/form-data is decoded at the BYTE level (a
      part's payload is arbitrary binary, e.g. an uploaded file, and running
      it through a text decoder first would corrupt it before parseMultipart
      ever saw it). Neither branch is a stub: both build a real G.FormData
      with real entries, not an empty one that merely satisfies `in`. What is
      NOT here: content-transfer-encoding (quoted-printable/base64 parts) and
      backslash-escaped quotes inside a filename -- undescribed rather than
      silently wrong, because no caller in this tree's corpus needed them. */
"  proto.formData = function () {\n"
"    var self = this;\n"
"    return this._drain().then(function (parts) {\n"
"      var ct = self.headers.get('content-type') || '';\n"
"      var rec = mimeParse(ct);\n"
"      if (!rec) throw new TypeError('formData: missing or invalid Content-Type');\n"
"      var essence = rec.type + '/' + rec.subtype;\n"
"      var fd = new G.FormData();\n"
"      if (essence === 'application/x-www-form-urlencoded') {\n"
"        var ab = joinParts(parts);\n"
"        var text = ab.byteLength ? __utf8(ab) : '';\n"
"        if (text.length) text.split('&').forEach(function (pair) {\n"
"          if (!pair) return;\n"
"          var eq = pair.indexOf('=');\n"
"          var k = eq < 0 ? pair : pair.slice(0, eq), v = eq < 0 ? '' : pair.slice(eq + 1);\n"
"          fd.append(decodeURIComponent(k.replace(/\\+/g, ' ')), decodeURIComponent(v.replace(/\\+/g, ' ')));\n"
"        });\n"
"        return fd;\n"
"      }\n"
"      if (essence === 'multipart/form-data') {\n"
"        var boundary = null;\n"
"        for (var pi = 0; pi < rec.params.length; pi++) if (rec.params[pi][0] === 'boundary') boundary = rec.params[pi][1];\n"
"        if (!boundary) throw new TypeError('formData: multipart/form-data with no boundary');\n"
"        parseMultipart(new Uint8Array(joinParts(parts)), boundary, fd);\n"
"        return fd;\n"
"      }\n"
"      throw new TypeError('formData: unsupported Content-Type');\n"
"    });\n"
"  };\n"
"}\n"
   /* fetch #concept-bodyinit-extract, the subset this engine needs. Returns
      {stream, contentType}; contentType is null when the BodyInit carries no
      opinion (ReadableStream, ArrayBuffer/view) and the caller must set it
      itself only when the caller has not already set one -- same rule for
      Request and Response because it is the same spec step on both sides of
      the connection, which is the whole reason this is one function instead
      of two copies that could drift. */
"function extractBody(v) {\n"
"  if (v === undefined || v === null) return { stream: null, contentType: null };\n"
"  if (v instanceof G.ReadableStream) return { stream: v, contentType: null };\n"
   /* The body IS the snapshot already taken at Blob-construction time, so no
      re-copy is needed beyond the one .slice() that keeps the stream's chunk
      independent of the Blob's own storage. */
"  if (v instanceof G.Blob) return { stream: rsOf(v._b.slice()), contentType: v.type || null };\n"
"  if (typeof v === 'string')\n"
"    return { stream: rsOf(new G.TextEncoder().encode(v)), contentType: 'text/plain;charset=UTF-8' };\n"
"  if (G.URLSearchParams && v instanceof G.URLSearchParams)\n"
"    return { stream: rsOf(new G.TextEncoder().encode(v.toString())), contentType: 'application/x-www-form-urlencoded;charset=UTF-8' };\n"
   /* FormData -> multipart/form-data, the wire encoding formData() (installBody,
      above) decodes back -- the two are meant to round-trip. The boundary is
      random per call, as any implementation's must be: the whole reason a
      boundary exists is that it cannot collide with anything a part's own
      bytes could contain. */
"  if (G.FormData && v instanceof G.FormData) {\n"
"    var boundary = 'LogitFormBoundary' + Math.random().toString(36).slice(2) + Date.now().toString(36);\n"
"    var enc = new G.TextEncoder(), chunks = [];\n"
"    v.forEach(function (val, key) {\n"
"      chunks.push(enc.encode('--' + boundary + '\\r\\n'));\n"
"      if (val instanceof G.Blob) {\n"
"        var filename = val.name !== undefined ? val.name : 'blob';\n"
"        chunks.push(enc.encode('Content-Disposition: form-data; name=\"' + key + '\"; filename=\"' + filename + '\"\\r\\n'));\n"
"        chunks.push(enc.encode('Content-Type: ' + (val.type || 'application/octet-stream') + '\\r\\n\\r\\n'));\n"
"        chunks.push(val._b);\n"
"      } else {\n"
"        chunks.push(enc.encode('Content-Disposition: form-data; name=\"' + key + '\"\\r\\n\\r\\n'));\n"
"        chunks.push(enc.encode(String(val)));\n"
"      }\n"
"      chunks.push(enc.encode('\\r\\n'));\n"
"    });\n"
"    chunks.push(enc.encode('--' + boundary + '--\\r\\n'));\n"
"    var total = 0, ci;\n"
"    for (ci = 0; ci < chunks.length; ci++) total += chunks[ci].length;\n"
"    var all = new Uint8Array(total), o = 0;\n"
"    for (ci = 0; ci < chunks.length; ci++) { all.set(chunks[ci], o); o += chunks[ci].length; }\n"
"    return { stream: rsOf(all), contentType: 'multipart/form-data; boundary=' + boundary };\n"
"  }\n"
"  return { stream: rsOf(toU8(v)), contentType: null };\n"
"}\n"

"var NULL_BODY_STATUS = [204, 205, 304];\n"
/* ---- Response ----
 * The body is a ReadableStream in every case, including the one where C
 * already had all the bytes: one shape means text()/json() cannot accidentally
 * work only for buffered responses. */
/* Former claim: __allowStatus0 was "not part of the public interface".
 * Correction (2026-09-09): it was an ordinary ResponseInit key, so any page
 * could pass it (even to admit status 700), while the network producer forgot
 * it and rejected every opaque status-0 response. The shared host/guest
 * response-boundary.js reproduces both doors. A closure-private identity and
 * captured constructor now distinguish internal creation; another string key
 * would merely rename the same bug. Capture the constructor so replacing
 * window.Response cannot intercept the token or break native fetch creation.
 * Public ResponseInit reads only status/statusText/headers. This does not
 * finish Web IDL readonly attributes or replace Headers' existing guard model.
 */
"var responseToken = {}, ResponseCtor;\n"
"G.Response = ResponseCtor = function Response(body, init) {\n"
"  var internal = arguments[2] === responseToken;\n"
"  init = init || {};\n"
"  this.status = init.status === undefined ? 200 : (init.status | 0);\n"
#ifdef WEBAPI_RESPONSE_PUBLIC_INTERNALS
"  if (!internal && !init.__allowStatus0 && (this.status < 200 || this.status > 599))\n"
#else
"  if (!internal && (this.status < 200 || this.status > 599))\n"
#endif
"    throw new RangeError('Response: status must be in the range 200 to 599, inclusive');\n"
"  this.statusText = init.statusText === undefined ? '' : String(init.statusText);\n"
   /* A reason-phrase is HTAB / SP-~ / obs-text (RFC 7230) -- the exact set
      isHTTPQSChar already enforces for header VALUES, reused here rather than
      writing a second copy of the same byte-range check. "\\n" and "Ā"
      (U+0100, outside 0x00..0xFF entirely) are the corpus's own two cases. */
"  for (var __sti = 0; __sti < this.statusText.length; __sti++)\n"
"    if (!isHTTPQSChar(this.statusText.charCodeAt(__sti)))\n"
"      throw new TypeError('Response: statusText is not a valid reason phrase');\n"
   /* Always a FRESH Headers, even when init.headers is already one -- cloning
      a Response used to alias the same Headers object between clone and
      original (a mutation to one's headers silently reached the other)
      because this used to reuse init.headers when it was already a Headers
      instance instead of copying it. new G.Headers(headers) copies. */
"  this.headers = new G.Headers(init.headers);\n"
"  this.headers._guard = 'response';\n"
   /* A script-constructed Response is ALWAYS 'default' -- basic/cors/opaque/
      opaqueredirect are properties of a response that came off the NETWORK
      (set explicitly by mkResponse, below, from the real fetch machinery),
      never a default a bare `new Response()` should invent for itself. */
#ifdef WEBAPI_RESPONSE_PUBLIC_INTERNALS
"  this.url = init.url || ''; this.redirected = !!init.redirected;\n"
"  this.type = init.type || 'default';\n"
#else
"  this.url = internal ? (init.url || '') : '';\n"
"  this.redirected = internal && !!init.redirected;\n"
"  this.type = internal ? (init.type || 'default') : 'default';\n"
#endif
"  this.ok = this.status >= 200 && this.status < 300;\n"
"  this._bodyUsed = false;\n"
"  if (body === undefined || body === null) { this.body = null; }\n"
"  else {\n"
     /* fetch #null-body-status: 204/205/304 may never carry a body, script-
        constructed or not. */
"    if (NULL_BODY_STATUS.indexOf(this.status) >= 0)\n"
"      throw new TypeError('Response: a ' + this.status + ' response cannot have a body');\n"
"    var eb = extractBody(body);\n"
"    this.body = eb.stream;\n"
"    if (eb.contentType && !this.headers.has('content-type')) this.headers.set('content-type', eb.contentType);\n"
"  }\n"
/* Install immutable only AFTER body extraction has supplied Content-Type.
 * clone used to reset every header guard to 'response', making a cloned error
 * mutable; network/redirect producers also never selected immutable. */
#ifndef WEBAPI_RESPONSE_LOSE_GUARD
"  if (internal && init.guard) this.headers._guard = init.guard;\n"
#endif
"};\n"
"function makeResponse(body, init) { return new ResponseCtor(body, init, responseToken); }\n"
"G.Response.prototype = {\n"
"  constructor: G.Response,\n"
"  clone: function () {\n"
"    if (this.bodyUsed) throw new TypeError('body already read');\n"
"    var t = this.body ? this.body.tee() : [null, null];\n"
"    this.body = t[0];\n"
#if defined(JS_RUNTIME_DIAGNOSTICS) && !defined(WEBAPI_FETCH_WORKER_PRELUDE)
"    var copy = makeResponse(t[1], { status: this.status, statusText: this.statusText,\n"
#else
"    return makeResponse(t[1], { status: this.status, statusText: this.statusText,\n"
#endif
"      headers: this.headers, url: this.url, redirected: this.redirected, type: this.type,\n"
"      guard: this.headers._guard });\n"
#if defined(JS_RUNTIME_DIAGNOSTICS) && !defined(WEBAPI_FETCH_WORKER_PRELUDE)
"    var meta = xhrDiagGet(xhrResponseDiag, this);\n"
"    if (meta) xhrDiagSet(xhrResponseDiag, copy, meta);\n"
"    return copy;\n"
#endif
"  }\n"
"};\n"
"installBody(G.Response.prototype);\n"
"Object.defineProperty(G.Response.prototype, Symbol.toStringTag, { value: 'Response', configurable: true });\n"
   /* Response.error(): status 0, an IMMUTABLE Headers (append/set/delete all
      throw) and type 'error' -- the immutability is the one thing that
      cannot be reached any other way: response-static-error.any.js's only
      subtest is that `.headers.append(...)` throws on the object this
      returns. */
"G.Response.error = function () {\n"
"  var r = makeResponse(null, { status: 0, type: 'error' });\n"
"  r.headers._guard = 'immutable';\n"
"  return r;\n"
"};\n"
"G.Response.redirect = function (url, status) {\n"
"  var u;\n"
"  try { u = new G.URL(String(url), (G.document && G.document.baseURI) || (G.location && G.location.href)); }\n"
"  catch (e) { throw new TypeError(\"Response.redirect: '\" + url + \"' is not a valid URL\"); }\n"
"  status = status === undefined ? 302 : (status | 0);\n"
"  if ([301, 302, 303, 307, 308].indexOf(status) < 0)\n"
"    throw new RangeError('Response.redirect: status must be one of 301, 302, 303, 307, 308');\n"
"  var r = new G.Response(null, { status: status, type: 'default' });\n"
"  r.headers.set('Location', u.href);\n"
#ifndef WEBAPI_RESPONSE_LOSE_GUARD
"  r.headers._guard = 'immutable';\n"
#endif
"  return r;\n"
"};\n"
"G.Response.json = function (v, init) {\n"
"  init = init || {};\n"
"  if (init.status !== undefined && ((init.status | 0) < 200 || (init.status | 0) > 599))\n"
"    throw new RangeError('Response.json: status must be in the range 200 to 599, inclusive');\n"
"  var body = JSON.stringify(v);\n"
"  if (body === undefined) throw new TypeError('Response.json: value has no JSON representation');\n"
"  var h = new G.Headers(init.headers);\n"
"  if (!h.has('content-type')) h.set('content-type', 'application/json');\n"
"  return new G.Response(body, { status: init.status, statusText: init.statusText, headers: h, type: 'default' });\n"
"};\n"

/* ---- fetch ---- */
"function pairsOf(o) { var p = [], k; for (k in o) p.push([k, o[k]]); return p; }\n"
/* data: URLs. A fetch of one must never reach the socket: there is no host to
 * connect to, and before this it went through url parsing, came out as a
 * hostname of "text/plain;base64,..." and failed DNS -- which is a confusing
 * way to say "this URL contains its own answer".
 *
 * It matters beyond neatness because it is the transport for
 * URL.createObjectURL (js_platform.c): that returns a data: URL rather than a
 * blob: one precisely so that the thing it returns can be dereferenced, and a
 * fetch that could not read one would make that a lie.
 *
 * The response is a real Response, built through the same ReadableStream path
 * as a network one -- so `await (await fetch(u)).text()` behaves identically --
 * with status 200 and the declared Content-Type. RFC 2397: an omitted type is
 * text/plain;charset=US-ASCII, and ;base64 is the only supported encoding. */
   /* ASCII whitespace (Infra): TAB LF FF CR SPACE -- note this INCLUDES \\f,
      unlike the HTTP whitespace used by mimeParse above. That difference is
      not pedantry: the data: URL processor works on the raw path text, where
      a literal \\f in the source has already been PERCENT-ENCODED to the
      three bytes "%0c" by the URL parser before this function ever runs (a
      C0 control cannot survive into an opaque path unescaped) -- so the type
      string this function strips whitespace from never contains a raw \\f to
      strip in the first place, and the surviving "%0c" text passes straight
      through mimeParse as ordinary token bytes ('%' is a valid HTTP token
      character -- see the note on isHTTPTokenChar). Get that backwards and
      the "%0c" reads as needing decoding, which is exactly the trap this
      comment exists to name before someone reaches for decodeURIComponent. */
"function isASCIIWSc(c) { return c === 9 || c === 10 || c === 12 || c === 13 || c === 32; }\n"
"function stripASCIIWS(s, lead, trail) {\n"
"  var i = 0, j = s.length;\n"
"  if (lead) while (i < j && isASCIIWSc(s.charCodeAt(i))) i++;\n"
"  if (trail) while (j > i && isASCIIWSc(s.charCodeAt(j - 1))) j--;\n"
"  return s.slice(i, j);\n"
"}\n"
   /* Percent-decode to BYTES, not to a JS string via decodeURIComponent --
      decodeURIComponent throws on a %-triplet that is not valid UTF-8 (e.g.
      "data:,%FF"), and the spec's answer for that input is the single byte
      0xFF, not an exception and not the three literal characters "%FF". Runs
      of plain (non-percent-triplet) characters are batched and handed to
      TextEncoder together, which is what keeps a surrogate pair intact. */
"function isHexDigit(c) { return (c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102); }\n"
"function percentDecodeBytes(s) {\n"
   /* The overwhelmingly common large-data case is base64 with no %-triplet.
      Bilibili's safety SDK constructs its WASM payload that way. Walking it
      once to discover there is no escape, then pushing every encoded byte
      through a growable JS Array, consumed the page watchdog before the
      actual video script could load. TextEncoder already is the native,
      allocation-sized byte conversion for exactly this no-escape case. */
"  if (s.indexOf('%') < 0) return new G.TextEncoder().encode(s);\n"
"  var out = [], i = 0, n = s.length;\n"
"  while (i < n) {\n"
"    if (s.charCodeAt(i) === 37 && i + 2 < n && isHexDigit(s.charCodeAt(i + 1)) && isHexDigit(s.charCodeAt(i + 2))) {\n"
"      out.push(parseInt(s.substr(i + 1, 2), 16));\n"
"      i += 3;\n"
"    } else {\n"
"      var j = i;\n"
"      while (j < n && !(s.charCodeAt(j) === 37 && j + 2 < n && isHexDigit(s.charCodeAt(j + 1)) && isHexDigit(s.charCodeAt(j + 2)))) j++;\n"
"      var enc = new G.TextEncoder().encode(s.slice(i, j));\n"
"      for (var k = 0; k < enc.length; k++) out.push(enc[k]);\n"
"      i = j;\n"
"    }\n"
"  }\n"
"  return new Uint8Array(out);\n"
"}\n"
   /* Forgiving-base64 decode (Infra #forgiving-base64-decode). Every failure
      mode here is its own subtest in the corpus (a lone '=' pair when
      length%4 isn't 0, a length%4===1 remainder, a non-alphabet byte) rather
      than one blanket "invalid base64" check, which is what the old
      `.replace(/[^A-Za-z0-9+/]/g, '')` version collapsed them all into --
      deleting illegal characters instead of failing is why it silently
      accepted "abcd ===" and every other case in this cluster's evidence. */
"function forgivingBase64Decode(data) {\n"
   /* Large data URLs must not spend a guest watchdog slice in regexp and JS
      per-character loops. The native primitive implements these same Infra
      rules and stays private inside this closure. */
"  var decoded = __base64Decode(String(data));\n"
"  return decoded === null ? null : new Uint8Array(decoded);\n"
"}\n"
   /* fetch #data-url-processor. Keep the opaque payload in JS instead of
      round-tripping it through __urlParse: network URLs are capped at
      WURL_MAX by design, while an inline WASM data URL is routinely hundreds
      of KiB. That cap truncated Bilibili's player bootstrap to 1513 decoded
      bytes. The first literal '#' starts the fragment; encoded %23 remains
      payload. */
"function dataURL(url) {\n"
"  url = String(url);\n"
"  if (url.slice(0, 5).toLowerCase() !== 'data:') return null;\n"
"  var hashAt = url.indexOf('#');\n"
"  var full = hashAt < 0 ? url : url.slice(0, hashAt);\n"
"  var input = full.slice(5);\n"
"  var comma = input.indexOf(',');\n"
"  if (comma < 0) return null;\n"
"  var mimeType = stripASCIIWS(input.slice(0, comma), true, true);\n"
"  var encodedBody = input.slice(comma + 1);\n"
"  var bytes;\n"
   /* The ";base64" marker tolerates ASCII whitespace around the ';' and
      after "base64", but NOT inside the word "base64" itself --
      "data:; base64,WA" and "data:;  base64,WA" are still base64 (the
      corpus's own cases), but "data:;base 64,WA" (a space INSIDE the word)
      must stay literal. A blanket "strip every whitespace character, then
      compare" pass (the first thing this looked like it should be) matches
      "base 64" too, which is why it is a regex anchored on a literal,
      unbroken "base64" instead. Only the base64-detection path folds the
      surrounding whitespace away; the ordinary (non-base64) path keeps it
      exactly as written, because a quoted parameter value is allowed to
      contain it. */
"  var b64m = /;[\\t\\n\\f\\r ]*base64[\\t\\n\\f\\r ]*$/i.exec(mimeType);\n"
"  if (b64m) {\n"
   /* RFC 2397 percent-decodes before forgiving-base64 decode. With no '%'
      (the common case), encodedBody already is the exact ASCII string the
      decoder needs. Do not turn N characters into N JS numbers and then
      concatenate those N bytes back into a string. */
"    var s = encodedBody;\n"
"    if (encodedBody.indexOf('%') >= 0) {\n"
"      var bodyBytes = percentDecodeBytes(encodedBody), chunks = [];\n"
"      for (var bi = 0; bi < bodyBytes.length; bi += 8192)\n"
"        chunks.push(String.fromCharCode.apply(null, bodyBytes.subarray(bi, bi + 8192)));\n"
"      s = chunks.join('');\n"
"    }\n"
"    var decoded = forgivingBase64Decode(s);\n"
"    if (decoded === null) return null;\n"
"    bytes = decoded;\n"
"    mimeType = stripASCIIWS(mimeType.slice(0, b64m.index), false, true);\n"
"  } else {\n"
"    bytes = percentDecodeBytes(encodedBody);\n"
"  }\n"
"  if (mimeType.charAt(0) === ';') mimeType = 'text/plain' + mimeType;\n"
"  var rec = mimeParse(mimeType);\n"
"  var type = rec ? mimeSerialize(rec) : 'text/plain;charset=US-ASCII';\n"
"  var ctrl = null;\n"
"  var stream = new G.ReadableStream({ start: function (c) { ctrl = c; } });\n"
"  var r = makeResponse(stream, { status: 200, statusText: 'OK',\n"
"    headers: [['content-type', type], ['content-length', String(bytes.length)]],\n"
"    url: url, redirected: false, type: 'basic' });\n"
"  ctrl.enqueue(bytes);\n"
"  ctrl.close();\n"
"  return r;\n"
"}\n"
/* ---- the request queue ----
 * WHY: WF_MAX is 8, and before this a ninth concurrent fetch was REJECTED with
 * `TypeError: too many requests in flight`. That is not what a browser does
 * and it is not a limit a page can be expected to respect -- bing's own script
 * loader fires a burst of a dozen, and eight of them arriving and four failing
 * leaves the page in a state it has no code for. tests/qmp/qmp_bing.py is
 * where that showed up: the fixture served 13 resources and the serial log
 * filled with the rejection.
 *
 * So requests past the limit WAIT instead. The limit is 6 rather than WF_MAX
 * so a slot is always left for something the browser itself needs (a
 * stylesheet, an image) rather than a page's telemetry burst taking all of
 * them -- and because the kernel's socket table is the real scarce resource
 * underneath.
 *
 * The queue is FIFO, which is the only ordering a page can reason about; an
 * abort while queued rejects immediately and never dials. */
/* THE QUEUE WAS RIGHT AND ITS PERMIT WAS MEASURING THE WRONG THING.
 *
 * MEASURED on the real machine, kimi.com over the real network: 144 x
 * `Uncaught (in promise) TypeError: too many requests in flight`, with 30
 * sub-resources arriving out of 108 requests -- the page painted its shell and
 * none of its data, because the sidebar, the logo and the feature chips all
 * come from calls that were among the 144. One bug, not two.
 *
 * The reason a queue with an UNBOUNDED array still rejected: `st.p` settles
 * when the RESPONSE HEADERS arrive, which is what the spec says and what makes
 * streaming work -- but the C-side `struct wfetch` slot stays occupied until
 * the BODY finishes. So fqLive was released at header time, the queue admitted
 * the next request, and with eight slots and bodies still draining it
 * eventually asked for a ninth. fqLive counted permits; the slots were the
 * resource.
 *
 * Three changes, and the third is the one that makes the failure impossible
 * rather than unlikely:
 *   - admission is gated on __fetchSlots(), the real free-slot count, as well
 *     as on FQ_MAX;
 *   - a full slot table reports `busy` instead of rejecting (see
 *     js_fetch_start), and the entry goes back to the FRONT of the queue so
 *     FIFO order survives a retry;
 *   - the drain is re-armed on a timer while anything is waiting, because a
 *     slot frees when a BODY completes and that is not an event the JS side
 *     can otherwise observe. 16 ms, on the pump's own clock (__later), so it
 *     cannot fire between two steps of a transfer.
 *
 * The queue's depth is bounded by memory, as it should be: a page may
 * legitimately have hundreds of fetches outstanding, and kimi does. */
"var FQ_MAX = 6;\n"
"var fqLive = 0, fqQ = [], fqTimer = -1;\n"
"function fqRearm() {\n"
"  if (fqTimer >= 0 || !fqQ.length) return;\n"
"  fqTimer = __later(16, function () { fqTimer = -1; fqDrain(); });\n"
   /* __later has eight slots and EventSource reconnections can hold them. A
      queue that failed to re-arm would stall until some other fetch finished,
      and if none were live it would stall for ever -- so fall back to the
      page's own timer when there is one. */
"  if (fqTimer < 0 && typeof G.setTimeout === 'function')\n"
"    G.setTimeout(function () { fqDrain(); }, 16);\n"
"}\n"
"function fqCanStart() {\n"
"  if (fqLive >= FQ_MAX) return false;\n"
"  return __fetchSlots() > 0;\n"
"}\n"
"function fqStart(e) {\n"
"  var st = __fetchStart(e.url, e.method, e.pairs, e.body, e.opts);\n"
   /* Lost the race for the slot between fqCanStart and here, or called
      without one. Put it back at the FRONT -- it was next -- and wait. */
"  if (!st || !st.p) { fqQ.unshift(e); fqRearm(); return; }\n"
"  e.started = true;\n"
"  fqLive++;\n"
"  if (e.sig && typeof e.sig.addEventListener === 'function')\n"
"    e.sig.addEventListener('abort', function () { __fetchAbort(st.h); });\n"
"  if (e.sig && e.sig.aborted) __fetchAbort(st.h);\n"
"  var done = function () { fqLive--; fqDrain(); };\n"
"  st.p.then(function (v) { done(); e.res(v); }, function (x) { done(); e.rej(x); });\n"
"}\n"
"function fqDrain() {\n"
"  while (fqQ.length && fqCanStart()) {\n"
"    var e = fqQ.shift();\n"
"    if (e.cancelled) continue;\n"
"    var before = fqQ.length;\n"
"    fqStart(e);\n"
     /* fqStart put it back: no slot after all, so stop rather than spin. */
"    if (fqQ.length > before) break;\n"
"  }\n"
"  fqRearm();\n"
"}\n"
"function fqEnqueue(url, method, pairs, body, opts, sig) {\n"
"  return new Promise(function (res, rej) {\n"
"    var e = { url: url, method: method, pairs: pairs, body: body, opts: opts,\n"
"              sig: sig, res: res, rej: rej, cancelled: false };\n"
     /* The abort listener is attached whether or not the request starts
        immediately: a queued entry that is aborted must reject now and never
        dial, and one that started has its handle aborted in fqStart. */
"    if (sig && typeof sig.addEventListener === 'function')\n"
"      sig.addEventListener('abort', function () {\n"
"        if (e.cancelled || e.started) return;\n"
"        e.cancelled = true;\n"
"        rej(sig.reason || abortError());\n"
"      });\n"
"    if (fqCanStart()) { fqStart(e); return; }\n"
"    fqQ.push(e);\n"
"    fqRearm();\n"
"  });\n"
"}\n"
/* fetch #port-blocking. Neither fetch nor Request looked at the URL's port at
 * all -- a page could dial the local SMTP or IRC port and get a real
 * connection, which is the one item in this whole area with a security shape
 * rather than a merely-incomplete one. Port 0 is not on the spec's own table
 * (it is blocked because it is not a valid connection target, not because
 * the table names it) but the corpus's own test file puts it in the same
 * blocked list it drives the assertions from, so it is folded in here rather
 * than argued about. */
"var BAD_PORTS = [0, 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22, 23, 25, 37, 42, 43, 53, 69, 77, 79, 87, 95,\n"
"  101, 102, 103, 104, 109, 110, 111, 113, 115, 117, 119, 123, 135, 137, 139, 143, 161, 179, 389, 427, 465,\n"
"  512, 513, 514, 515, 526, 530, 531, 532, 540, 548, 554, 556, 563, 587, 601, 636, 989, 990, 993, 995,\n"
"  1719, 1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061, 6000, 6566, 6665, 6666, 6667, 6668, 6669, 6679,\n"
"  6697, 10080];\n"
"function isBadPort(u) {\n"
"  var scheme = u.protocol ? u.protocol.toLowerCase() : '';\n"
"  if (scheme !== 'http:' && scheme !== 'https:' && scheme !== 'ws:' && scheme !== 'wss:') return false;\n"
"  if (u.port === '') return false;\n"
"  return BAD_PORTS.indexOf(parseInt(u.port, 10)) >= 0;\n"
"}\n"
"function parsedURLOrNull(url) {\n"
"  try { return new G.URL(url, (G.document && G.document.baseURI) || (G.location && G.location.href)); }\n"
"  catch (e) { return null; }\n"
"}\n"
/* fetch of a blob: URL used to ignore Range entirely: always 200, a
 * malformed Range never failed. There is no socket on this path at all --
 * the whole thing is inside the browser -- so parsing the Range header and
 * answering 206 (or rejecting on a bad one) is pure computation, same shape
 * as the data: URL processor above. rangeParse returns null on ANY malformed
 * input, which the caller turns into a TypeError rejection rather than
 * silently falling back to a full 200 response -- the corpus enumerates each
 * malformed shape (no "bytes=", no "-", non-digit bounds, both bounds empty,
 * a trailing comma, more than one range) as its OWN subtest specifically to
 * catch an implementation that folds them all into one check. */
   /* Lenient about ASCII whitespace around "bytes", '=' and '-' -- the
      corpus's OWN "supported" fixtures include "bytes= \\t9-21", "bytes=5 -
      10", "bytes=-\\t 5" and "bytes \\t =\\t 6-" as cases that must still
      succeed, which is stricter-than-real-browsers territory the fetch
      spec's formal grammar does not actually require rejecting. What must
      still fail: the wrong keyword ("byte="), no '=' at all, more than one
      range (a literal ',' anywhere in the range-spec), and both bounds
      empty. */
"function rangeParse(h, size) {\n"
"  var i = 0, n = h.length;\n"
"  while (i < n && isASCIIWSc(h.charCodeAt(i))) i++;\n"
"  if (h.slice(i, i + 5) !== 'bytes') return null;\n"
"  i += 5;\n"
"  while (i < n && isASCIIWSc(h.charCodeAt(i))) i++;\n"
"  if (h.charAt(i) !== '=') return null;\n"
"  i++;\n"
"  while (i < n && isASCIIWSc(h.charCodeAt(i))) i++;\n"
"  var spec = h.slice(i);\n"
"  if (spec.indexOf(',') >= 0) return null;\n"
"  var dash = spec.indexOf('-');\n"
"  if (dash < 0) return null;\n"
"  var a = stripASCIIWS(spec.slice(0, dash), true, true), b = stripASCIIWS(spec.slice(dash + 1), true, true);\n"
"  if (a === '' && b === '') return null;\n"
"  if (a !== '' && !/^[0-9]+$/.test(a)) return null;\n"
"  if (b !== '' && !/^[0-9]+$/.test(b)) return null;\n"
"  var start, end;\n"
"  if (a === '') {\n"
       /* suffix range: bytes=-N -- the last N bytes. */
"    var suffix = parseInt(b, 10);\n"
"    if (suffix === 0) return null;\n"
"    start = Math.max(size - suffix, 0); end = size - 1;\n"
"  } else {\n"
"    start = parseInt(a, 10);\n"
"    if (start >= size) return null;\n"
"    end = b === '' ? size - 1 : Math.min(parseInt(b, 10), size - 1);\n"
"    if (end < start) return null;\n"
"  }\n"
"  return { start: start, end: end };\n"
"}\n"
"function fetchBlobURL(url, init, input) {\n"
"  var bo = Object.prototype.hasOwnProperty.call(__objURLs, url) ? __objURLs[url] : null;\n"
"  if (!bo) return Promise.reject(new TypeError('Failed to fetch: unknown or revoked blob: URL'));\n"
"  var reqHeaders = new G.Headers(init && init.headers !== undefined ? init.headers : (input && input.headers));\n"
"  var range = reqHeaders.get('range');\n"
   /* Content-Type is always PRESENT, even as '' for a typeless Blob -- the
      corpus checks headers.get('Content-Type') against `type || ''`, which
      only agrees with an implementation that sets the header unconditionally
      rather than omitting it when the Blob's type is the empty string. */
"  if (range === null) {\n"
"    var bh = [['content-type', bo.type || ''], ['content-length', String(bo.size)]];\n"
"    return Promise.resolve(makeResponse(rsOf(bo._b.slice()), { status: 200, statusText: 'OK',\n"
"      headers: bh, url: url, redirected: false, type: 'basic' }));\n"
"  }\n"
"  var r = rangeParse(range, bo.size);\n"
"  if (!r) return Promise.reject(new TypeError('fetch: malformed Range header'));\n"
"  var slice = bo._b.slice(r.start, r.end + 1);\n"
"  var rh = [['content-type', bo.type || ''], ['content-length', String(slice.length)],\n"
"            ['content-range', 'bytes ' + r.start + '-' + r.end + '/' + bo.size]];\n"
"  return Promise.resolve(makeResponse(rsOf(slice), { status: 206, statusText: 'Partial Content',\n"
"    headers: rh, url: url, redirected: false, type: 'basic' }));\n"
"}\n"
"G.fetch = function fetch(input, init) {\n"
"  init = init || {};\n"
"  var url = (input && typeof input === 'object' && input.url) ? input.url : String(input);\n"
"  if (url.slice(0, 5).toLowerCase() === 'data:') {\n"
"    var dr = dataURL(url);\n"
"    return dr ? Promise.resolve(dr)\n"
"              : Promise.reject(new TypeError('Failed to fetch: malformed data: URL'));\n"
"  }\n"
"  if (url.slice(0, 5).toLowerCase() === 'blob:') {\n"
"    return fetchBlobURL(url, init, input);\n"
"  }\n"
"  var __fpu = parsedURLOrNull(url);\n"
"  if (__fpu && isBadPort(__fpu)) return Promise.reject(new TypeError('fetch: blocked port ' + __fpu.port));\n"
"  var method = String(init.method || (input && input.method) || 'GET').toUpperCase();\n"
"  var hs = new G.Headers(init.headers || (input && input.headers));\n"
"  var body = init.body;\n"
"  if (body !== undefined && body !== null) {\n"
"    if (G.URLSearchParams && body instanceof G.URLSearchParams) {\n"
"      if (!hs.has('content-type')) hs.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');\n"
"      body = body.toString();\n"
       /* A Blob body's bytes are what go over the wire; its .type becomes
          Content-Type when the caller did not already set one -- same rule
          Response's constructor uses, because it is the same spec step
          (extractBody) on both sides of the connection. Before this branch
          existed a Blob fell to String(body) below and sent the literal text
          "[object Blob]" instead of the Blob's bytes. */
"    } else if (body instanceof G.Blob) {\n"
"      if (body.type && !hs.has('content-type')) hs.set('content-type', body.type);\n"
"      body = u8ab(body._b);\n"
"    } else if (ArrayBuffer.isView(body)) {\n"
"      body = body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength);\n"
"    } else if (!(body instanceof ArrayBuffer) && typeof body !== 'string') {\n"
"      body = String(body);\n"
"    }\n"
"  } else body = null;\n"
"  var pairs = []; hs.forEach(function (v, k) { pairs.push([k, v]); });\n"
"  var opts = { mode: init.mode, credentials: init.credentials };\n"
"  var sig = init.signal || (input && input.signal);\n"
"  if (sig && typeof sig === 'object' && sig.aborted)\n"
"    return Promise.reject(sig.reason || abortError());\n"
"  return fqEnqueue(url, method, pairs, body, opts, sig);\n"
"};\n"
/* ---- Request ----
 * Used to be a five-line object literal with no prototype at all -- Request
 * instances inherited straight from Object.prototype, so `.blob()`/`.text()`/
 * `.clone()` were all "is not a function". This is what N2 in the cluster
 * write-up closed: the shared Body mixin (installBody, above) plus the
 * plain readonly members request-structure.any.js enumerates by name, plus
 * the four throws the spec requires (forbidden method; a body on GET/HEAD; a
 * ReadableStream body without duplex:'half'; a blocked port, N6). */
"G.Request = function Request(input, init) {\n"
"  init = init || {};\n"
"  var src = (input && typeof input === 'object' && input instanceof G.Request) ? input : null;\n"
"  if (src && src.bodyUsed) throw new TypeError('Request: input has already been used');\n"
"  this._url = src ? src.url : String(input);\n"
"  var __ru = parsedURLOrNull(this._url);\n"
"  if (__ru && isBadPort(__ru)) throw new TypeError('Request: blocked port ' + __ru.port);\n"
"  var method = String(init.method !== undefined ? init.method : (src ? src.method : 'GET')).toUpperCase();\n"
"  if (isForbiddenMethod(method)) throw new TypeError(\"Request: method '\" + method + \"' is forbidden\");\n"
"  this._method = method;\n"
"  this._headers = new G.Headers(init.headers !== undefined ? init.headers : (src ? src.headers : (input && input.headers)));\n"
"  this._headers._guard = (init.mode || (src && src.mode)) === 'no-cors' ? 'request-no-cors' : 'request';\n"
"  this._mode = init.mode || (src ? src.mode : 'cors');\n"
"  this._credentials = init.credentials || (src ? src.credentials : 'same-origin');\n"
"  this._cache = init.cache || (src ? src.cache : 'default');\n"
"  this._redirect = init.redirect || (src ? src.redirect : 'follow');\n"
"  this._referrer = init.referrer !== undefined ? String(init.referrer) : (src ? src.referrer : 'about:client');\n"
"  this._referrerPolicy = init.referrerPolicy !== undefined ? init.referrerPolicy : (src ? src.referrerPolicy : '');\n"
"  this._integrity = init.integrity !== undefined ? String(init.integrity) : (src ? src.integrity : '');\n"
"  this.keepalive = init.keepalive !== undefined ? !!init.keepalive : (src ? !!src.keepalive : false);\n"
"  this._destination = '';\n"
"  this._isReloadNavigation = false;\n"
"  this._isHistoryNavigation = false;\n"
"  this.signal = init.signal !== undefined ? init.signal : (src ? src.signal : ((input && input.signal) || null));\n"
"  this._bodyUsed = false;\n"
"  this._duplex = init.duplex !== undefined ? init.duplex : (src ? src._duplex : undefined);\n"
"  if (init.body !== undefined) {\n"
"    var bodySrc = init.body;\n"
"    if (bodySrc !== null && (this._method === 'GET' || this._method === 'HEAD'))\n"
"      throw new TypeError('Request: a ' + this._method + ' request cannot have a body');\n"
"    if (bodySrc instanceof G.ReadableStream && init.duplex !== 'half')\n"
"      throw new TypeError(\"Request: a ReadableStream body requires duplex: 'half'\");\n"
"    if (bodySrc === null) { this.body = null; }\n"
"    else {\n"
"      var eb = extractBody(bodySrc);\n"
"      this.body = eb.stream;\n"
"      if (eb.contentType && !this._headers.has('content-type')) this._headers.set('content-type', eb.contentType);\n"
"    }\n"
   /* input was a Request and this Request is taking its body -- the source
      cannot be read from again (fetch spec: "set this's body to input's
      body", which moves ownership rather than copying). Both sides write the
      PRIVATE field: bodyUsed is a readonly getter (below) precisely so that
      script cannot forge it, which means internal code cannot go through the
      public name either. */
"  } else if (src) {\n"
"    this.body = src.body;\n"
"    if (src.body) { src._bodyUsed = true; src.body = null; }\n"
"  } else { this.body = null; }\n"
"};\n"
"G.Request.prototype = {\n"
"  constructor: G.Request,\n"
"  clone: function () {\n"
"    if (this.bodyUsed) throw new TypeError('body already read');\n"
"    var r = Object.create(G.Request.prototype);\n"
"    if (this.body) { var t = this.body.tee(); this.body = t[0]; r.body = t[1]; } else r.body = null;\n"
"    r._url = this._url; r._method = this._method;\n"
"    r._headers = new G.Headers(this._headers); r._headers._guard = this._headers._guard;\n"
"    r._duplex = this._duplex; r._bodyUsed = false;\n"
"    r._mode = this._mode; r._credentials = this._credentials; r._cache = this._cache; r._redirect = this._redirect;\n"
"    r._referrer = this._referrer; r._referrerPolicy = this._referrerPolicy; r._integrity = this._integrity;\n"
"    r.keepalive = this.keepalive; r._destination = this._destination;\n"
"    r._isReloadNavigation = this._isReloadNavigation; r._isHistoryNavigation = this._isHistoryNavigation;\n"
"    r.signal = this.signal;\n"
"    return r;\n"
"  }\n"
"};\n"
"installBody(G.Request.prototype);\n"
"Object.defineProperty(G.Request.prototype, Symbol.toStringTag, { value: 'Request', configurable: true });\n"
   /* request-structure.any.js's whole point: these fourteen are IDL attributes
      with a getter and NO setter, so `request.method = 'POST'` must be a
      silent no-op (sloppy-mode [[Set]] on an inherited accessor with no
      setter does not fall through to creating an own property). Getter-only
      accessors on the PROTOTYPE achieve that; a plain `this.method = ...` own
      data property, which is what this constructor used to write, does not --
      an own data property is always writable regardless of what the
      prototype says. */
"function roGet(field) { return function () { return this[field]; }; }\n"
"Object.defineProperties(G.Request.prototype, {\n"
"  method:             { get: roGet('_method'), enumerable: true, configurable: true },\n"
"  url:                { get: roGet('_url'), enumerable: true, configurable: true },\n"
"  headers:            { get: roGet('_headers'), enumerable: true, configurable: true },\n"
"  destination:        { get: roGet('_destination'), enumerable: true, configurable: true },\n"
"  referrer:           { get: roGet('_referrer'), enumerable: true, configurable: true },\n"
"  referrerPolicy:     { get: roGet('_referrerPolicy'), enumerable: true, configurable: true },\n"
"  mode:               { get: roGet('_mode'), enumerable: true, configurable: true },\n"
"  credentials:        { get: roGet('_credentials'), enumerable: true, configurable: true },\n"
"  cache:              { get: roGet('_cache'), enumerable: true, configurable: true },\n"
"  redirect:           { get: roGet('_redirect'), enumerable: true, configurable: true },\n"
"  integrity:          { get: roGet('_integrity'), enumerable: true, configurable: true },\n"
"  isReloadNavigation:  { get: roGet('_isReloadNavigation'), enumerable: true, configurable: true },\n"
"  isHistoryNavigation: { get: roGet('_isHistoryNavigation'), enumerable: true, configurable: true },\n"
   /* No init.duplex means 'half' is still the READ value even though nothing
      was stored -- 'half' is the only value this engine's bodies ever need,
      so there is no second mode to distinguish. */
"  duplex:             { get: function () { return this._duplex || 'half'; }, enumerable: true, configurable: true }\n"
"});\n"
