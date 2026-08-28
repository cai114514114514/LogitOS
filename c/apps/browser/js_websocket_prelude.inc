/* The JS-visible half of WebSocket: constructor validation (every throw here
 * is SYNCHRONOUS, per spec -- a malformed URL, scheme, port or subprotocol
 * list never reaches the network), readyState/url/protocol/extensions/
 * bufferedAmount/binaryType, send()/close(), and the four events wired
 * through G.EventTarget (js_events.c) and the G.CloseEvent/G.MessageEvent
 * classes it already publishes.
 *
 * Three C primitives, passed as arguments so they are not reachable as
 * globals a page could shadow (same idiom as js_webapi.c's __fetchStart):
 *   __wsOpen(host, port, tls, target, protocolsCsv, protocolsLowerCsv,
 *            origin, self) -> id
 *   __wsSend(id, isBinary, data)
 *   __wsClose(id, code, reason)
 * This file returns { fire: __wsFire } so js_websocket_install can capture
 * __wsFire -- the one function C calls back into to actually build and
 * dispatch an event, so js_websocket.c never touches G.Event/CloseEvent/
 * MessageEvent construction itself. */
#define WS_PRELUDE_STR \
"(function (__wsOpen, __wsSend, __wsClose) {\n" \
"'use strict';\n" \
"var G = globalThis;\n" \
"if (typeof G.EventTarget !== 'function') return { fire: null };\n" \
/* The Fetch spec's bad-port list (https://fetch.spec.whatwg.org/#port-blocking),
 * reused here per RFC 6455 4.1's own deference to it -- ports a browser
 * refuses to fetch are ports it refuses to open a raw TCP WebSocket to for
 * the identical reason (a WebSocket handshake looks like plausible bytes to
 * an SMTP/NNTP/etc. server on the other end of one of these). */ \
"var BLOCKED_PORTS = [1,7,9,11,13,15,17,19,20,21,22,23,25,37,42,43,53,69,77,79,\n" \
"  87,95,101,102,103,104,109,110,111,113,115,117,119,123,135,137,138,139,143,\n" \
"  161,179,389,427,465,512,513,514,515,526,530,531,532,540,548,554,556,563,\n" \
"  587,601,636,989,990,993,995,1719,1720,1723,2049,3659,4045,5060,5061,6000,\n" \
"  6566,6665,6666,6667,6668,6669,6697,10080];\n" \
"function named(f, n, len) {\n" \
"  try { Object.defineProperty(f, 'name', { value: n, configurable: true }); } catch (e) {}\n" \
"  try { Object.defineProperty(f, 'length', { value: len, configurable: true }); } catch (e) {}\n" \
"  return f;\n" \
"}\n" \
"function utf8ByteLen(s) {\n" \
"  var n = 0;\n" \
"  for (var i = 0; i < s.length; i++) {\n" \
"    var c = s.charCodeAt(i);\n" \
"    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {\n" \
"      var c2 = s.charCodeAt(i + 1);\n" \
"      if (c2 >= 0xDC00 && c2 <= 0xDFFF) { n += 4; i++; continue; }\n" \
"    }\n" \
"    if (c < 0x80) n += 1; else if (c < 0x800) n += 2; else n += 3;\n" \
"  }\n" \
"  return n;\n" \
"}\n" \
"var TOKEN_RE = /^[\\x21\\x23-\\x27\\x2a\\x2b\\x2d\\x2e\\x30-\\x39\\x41-\\x5a\\x5e-\\x7a\\x7c\\x7e]+$/;\n" \
"\n" \
"function WebSocket(url, protocols) {\n" \
"  if (!new.target) throw new TypeError(\"Constructor 'WebSocket' requires 'new'\");\n" \
"  if (arguments.length < 1) throw new TypeError('WebSocket: 1 argument required, but only 0 present.');\n" \
"  var self = Reflect.construct(G.EventTarget, [], new.target || WebSocket);\n" \
"  var u;\n" \
"  var base = (G.location && G.location.href) || undefined;\n" \
"  try { u = new G.URL(String(url), base); }\n" \
"  catch (e) { throw new G.DOMException(\"'\" + url + \"' is not a valid URL\", 'SyntaxError'); }\n" \
"  if (u.protocol !== 'ws:' && u.protocol !== 'wss:')\n" \
"    throw new G.DOMException(\"The URL's scheme must be either 'ws' or 'wss'. '\" + u.protocol + \"' is not allowed.\", 'SyntaxError');\n" \
"  if (u.username || u.password)\n" \
"    throw new G.DOMException('The URL contains credentials', 'SyntaxError');\n" \
"  if (u.hash)\n" \
"    throw new G.DOMException('The URL contains a fragment identifier', 'SyntaxError');\n" \
"\n" \
"  var protoList = [];\n" \
"  if (protocols !== undefined && protocols !== null) {\n" \
"    protoList = Array.isArray(protocols) ? protocols.map(String) : [String(protocols)];\n" \
"    for (var i = 0; i < protoList.length; i++) {\n" \
"      var p = protoList[i];\n" \
"      if (!p || !TOKEN_RE.test(p))\n" \
"        throw new G.DOMException(\"An invalid or duplicate subprotocol was specified: '\" + p + \"'\", 'SyntaxError');\n" \
"      for (var j = 0; j < i; j++)\n" \
"        if (protoList[j].toLowerCase() === p.toLowerCase())\n" \
"          throw new G.DOMException(\"An invalid or duplicate subprotocol was specified: '\" + p + \"'\", 'SyntaxError');\n" \
"    }\n" \
"  }\n" \
"\n" \
"  var tls = u.protocol === 'wss:';\n" \
"  var port = u.port ? (u.port | 0) : (tls ? 443 : 80);\n" \
"  if (BLOCKED_PORTS.indexOf(port) >= 0)\n" \
"    throw new G.DOMException(\"Port \" + port + \" is not allowed\", 'SecurityError');\n" \
"\n" \
"  var host = u.hostname;\n" \
"  var target = (u.pathname || '/') + (u.search || '');\n" \
"  var origin = (G.location && G.location.protocol && G.location.host) ?\n" \
"    (G.location.protocol + '//' + G.location.host) : 'null';\n" \
"\n" \
"  self.readyState = 0;\n" \
"  self.url = u.protocol + '//' + host + (u.port ? ':' + u.port : '') + target;\n" \
"  self.protocol = '';\n" \
"  self.extensions = '';\n" \
"  self.bufferedAmount = 0;\n" \
"  var binType = 'blob';\n" \
"  Object.defineProperty(self, 'binaryType', {\n" \
"    get: function () { return binType; },\n" \
"    set: function (v) {\n" \
"      if (v !== 'blob' && v !== 'arraybuffer')\n" \
"        throw new G.DOMException(\"binaryType must be 'blob' or 'arraybuffer'\", 'SyntaxError');\n" \
"      binType = v;\n" \
"    }, configurable: true\n" \
"  });\n" \
"\n" \
"  var protoCsv = protoList.join(', ');\n" \
"  var protoLcCsv = ',' + protoList.map(function (s) { return s.toLowerCase(); }).join(',') + ',';\n" \
"  var id = __wsOpen(host, port, tls ? 1 : 0, target, protoCsv, protoList.length ? protoLcCsv : '', origin, self);\n" \
"  Object.defineProperty(self, '_wsid', { value: id, configurable: false });\n" \
"  return self;\n" \
"}\n" \
"WebSocket.prototype = Object.create(G.EventTarget.prototype);\n" \
"Object.defineProperty(WebSocket.prototype, 'constructor', { value: WebSocket, writable: true, configurable: true });\n" \
"named(WebSocket, 'WebSocket', 1);\n" \
"WebSocket.CONNECTING = 0; WebSocket.OPEN = 1; WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;\n" \
"WebSocket.prototype.CONNECTING = 0; WebSocket.prototype.OPEN = 1;\n" \
"WebSocket.prototype.CLOSING = 2; WebSocket.prototype.CLOSED = 3;\n" \
"\n" \
/* The IDL "event handler content attribute" idiom: setting .onmessage adds
 * (and replaces) exactly one addEventListener registration, matching how
 * every other on* property in this browser behaves. */ \
"function handlerSlot(type) {\n" \
"  var m = new WeakMap();\n" \
"  Object.defineProperty(WebSocket.prototype, 'on' + type, {\n" \
"    get: function () { return m.get(this) || null; },\n" \
"    set: function (v) {\n" \
"      var cur = m.get(this);\n" \
"      if (cur) this.removeEventListener(type, cur);\n" \
"      if (typeof v === 'function') { m.set(this, v); this.addEventListener(type, v); }\n" \
"      else m.delete(this);\n" \
"    }, configurable: true, enumerable: true\n" \
"  });\n" \
"}\n" \
"['open', 'message', 'error', 'close'].forEach(handlerSlot);\n" \
"\n" \
"WebSocket.prototype.send = named(function (data) {\n" \
"  if (!(this instanceof WebSocket)) throw new TypeError('not a WebSocket');\n" \
"  if (arguments.length < 1) throw new TypeError('send: 1 argument required, but only 0 present.');\n" \
"  if (this.readyState === 0) throw new G.DOMException('Still in CONNECTING state.', 'InvalidStateError');\n" \
"  var isBinary, payload;\n" \
"  if (typeof data === 'string') { isBinary = false; payload = data; }\n" \
"  else if (typeof G.Blob === 'function' && data instanceof G.Blob) {\n" \
"    isBinary = true;\n" \
"    var b = data._b;\n" \
"    payload = b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);\n" \
"  } else if (data instanceof ArrayBuffer) { isBinary = true; payload = data.slice(0); }\n" \
"  else if (ArrayBuffer.isView(data)) {\n" \
"    isBinary = true;\n" \
"    payload = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);\n" \
"  } else { isBinary = false; payload = String(data); }\n" \
"  __wsSend(this._wsid, isBinary ? 1 : 0, payload);\n" \
"}, 'send', 1);\n" \
"\n" \
"WebSocket.prototype.close = named(function (code, reason) {\n" \
"  if (!(this instanceof WebSocket)) throw new TypeError('not a WebSocket');\n" \
"  var hasCode = arguments.length > 0 && code !== undefined;\n" \
"  var c = 1000;\n" \
"  if (hasCode) {\n" \
"    c = Number(code);\n" \
"    if (!Number.isInteger(c) || (c !== 1000 && (c < 3000 || c > 4999)))\n" \
"      throw new G.DOMException('close code out of range', 'InvalidAccessError');\n" \
"  }\n" \
"  var r = reason === undefined ? '' : String(reason);\n" \
"  if (utf8ByteLen(r) > 123)\n" \
"    throw new G.DOMException(\"'reason' cannot be larger than 123 bytes\", 'SyntaxError');\n" \
"  if (this.readyState === 0 || this.readyState === 1)\n" \
"    __wsClose(this._wsid, hasCode ? c : 1000, r);\n" \
"}, 'close', 0);\n" \
"\n" \
/* Fired FROM C (js_websocket.c's ws_fire_* family). Not reachable from
 * script -- it is returned in the hooks object, never assigned to G. */ \
"function __wsFire(self, type, a, b, c) {\n" \
"  var ev;\n" \
"  if (type === 'message') {\n" \
"    var data = a;\n" \
"    if (c /* isBinary */) {\n" \
"      if (self.binaryType === 'arraybuffer') { /* a is already an ArrayBuffer */ }\n" \
"      else if (typeof G.Blob === 'function') data = new G.Blob([a]);\n" \
"    }\n" \
"    ev = new G.MessageEvent('message', { data: data, origin: self.url });\n" \
"  } else if (type === 'close') {\n" \
"    ev = new G.CloseEvent('close', { wasClean: !!a, code: b, reason: c });\n" \
"  } else {\n" \
"    ev = new G.Event(type);\n" \
"  }\n" \
"  self.dispatchEvent(ev);\n" \
"}\n" \
"\n" \
"G.WebSocket = WebSocket;\n" \
"return { fire: __wsFire };\n" \
"})\n"

#define WS_PRELUDE WS_PRELUDE_STR
