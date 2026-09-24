/* The platform's SHAPE, as a script sees it -- run unchanged in this browser and
 * in a reference browser, and diffed (tests/qmp/webidl_shape_diff.py).
 *
 * Not "does feature X work" but "is the object model laid out like a
 * browser's": which interfaces exist, each prototype's chain, which members
 * sit on which prototype and as what (method / accessor / data), what the
 * singletons own, what Object.prototype.toString says, whether platform
 * functions stringify as native code. Anti-tamper scripts (Cloudflare's
 * challenge among them) read exactly these things: `new Proxy(
 * Document.prototype.querySelectorAll, ...)` is what found the first gap.
 *
 * Output: one JSON object (returned by shapeDump()). ES2017, no DOM mutation,
 * no network, reads only. */
function shapeDump() {
  var G = globalThis;
  var IFACES = ['EventTarget', 'Node', 'Element', 'HTMLElement', 'HTMLDivElement', 'HTMLBodyElement',
    'HTMLHtmlElement', 'HTMLHeadElement', 'HTMLAnchorElement', 'HTMLImageElement', 'HTMLInputElement',
    'HTMLButtonElement', 'HTMLFormElement', 'HTMLScriptElement', 'HTMLIFrameElement', 'HTMLCanvasElement',
    'HTMLStyleElement', 'HTMLLinkElement', 'HTMLSpanElement', 'HTMLTemplateElement', 'HTMLUnknownElement',
    'SVGElement', 'SVGSVGElement', 'Document', 'HTMLDocument', 'XMLDocument', 'DocumentFragment',
    'ShadowRoot', 'CharacterData', 'Text', 'Comment', 'Attr', 'DocumentType', 'NodeList',
    'HTMLCollection', 'DOMTokenList', 'NamedNodeMap', 'Window', 'Navigator', 'Screen',
    'ScreenOrientation', 'History', 'Location', 'Performance', 'PerformanceEntry',
    'PerformanceNavigationTiming', 'PerformanceResourceTiming', 'PerformancePaintTiming',
    'PerformanceObserver', 'PerformanceTiming', 'PerformanceNavigation', 'Event', 'UIEvent',
    'MouseEvent', 'PointerEvent', 'KeyboardEvent', 'FocusEvent', 'WheelEvent', 'InputEvent',
    'CustomEvent', 'MessageEvent', 'ErrorEvent', 'PromiseRejectionEvent', 'Storage', 'Crypto',
    'SubtleCrypto', 'CSSStyleDeclaration', 'CSSStyleSheet', 'StyleSheetList', 'MediaQueryList',
    'MutationObserver', 'IntersectionObserver', 'ResizeObserver', 'CanvasRenderingContext2D',
    'ImageData', 'XMLHttpRequest', 'Request', 'Response', 'Headers', 'URL', 'URLSearchParams',
    'Blob', 'File', 'FileReader', 'FormData', 'AbortController', 'AbortSignal', 'Worker',
    'MessageChannel', 'MessagePort', 'BroadcastChannel', 'TextEncoder', 'TextDecoder', 'DOMParser',
    'XMLSerializer', 'Range', 'Selection', 'TreeWalker', 'NodeIterator', 'DOMRect', 'DOMRectReadOnly',
    'DOMException', 'PluginArray', 'MimeTypeArray', 'Plugin', 'MimeType', 'Permissions',
    'VisualViewport', 'WebSocket', 'EventSource', 'Image', 'Option', 'Audio', 'AudioContext',
    'OfflineAudioContext', 'WebGLRenderingContext', 'WebGL2RenderingContext', 'OffscreenCanvas',
    'ImageBitmap', 'IDBFactory', 'CacheStorage', 'ServiceWorkerContainer', 'Clipboard',
    'CustomElementRegistry', 'FontFace', 'FontFaceSet', 'Notification', 'Intl'];

  // Name every prototype object that belongs to a global constructor, so a
  // chain can be printed as interface names rather than as opaque objects.
  var protoName = new Map();
  var gnames = Object.getOwnPropertyNames(G);
  for (var i = 0; i < gnames.length; i++) {
    var v;
    try { v = G[gnames[i]]; } catch (e) { continue; }
    if (typeof v === 'function' && v.prototype && typeof v.prototype === 'object' &&
        !protoName.has(v.prototype)) protoName.set(v.prototype, gnames[i]);
  }
  function nameOf(p) {
    if (p === null) return 'null';
    if (protoName.has(p)) return protoName.get(p);
    return '?';
  }
  function chain(o) {
    var out = [];
    try {
      for (var p = Object.getPrototypeOf(o), n = 0; p && n < 16; p = Object.getPrototypeOf(p), n++) out.push(nameOf(p));
    } catch (e) { out.push('!' + e.name); }
    return out;
  }
  function kindOf(d) {
    if (!d) return 'none';
    if ('value' in d) return typeof d.value === 'function' ? 'm' : 'd';
    return (d.get ? 'g' : '') + (d.set ? 's' : '');
  }
  function members(o) {
    var out = {};
    var keys;
    try { keys = Reflect.ownKeys(o); } catch (e) { return { '!': e.name }; }
    for (var i = 0; i < keys.length; i++) {
      var k = keys[i];
      var kn = typeof k === 'symbol' ? '@' + (k.description || '') : k;
      var d;
      try { d = Object.getOwnPropertyDescriptor(o, k); } catch (e) { out[kn] = '!'; continue; }
      out[kn] = kindOf(d) + (d && d.enumerable ? 'e' : '') + (d && d.configurable ? 'c' : '');
    }
    return out;
  }
  function tag(o) { try { return Object.prototype.toString.call(o); } catch (e) { return '!' + e.name; } }
  function srcKind(f) {
    try {
      var s = Function.prototype.toString.call(f);
      return /\{\s*\[native code\]\s*\}\s*$/.test(s) ? 'native' : 'source';
    } catch (e) { return '!' + e.name; }
  }

  var out = { ifaces: {}, singletons: {}, native: {} };
  for (var j = 0; j < IFACES.length; j++) {
    var n = IFACES[j], C;
    try { C = G[n]; } catch (e) { C = undefined; }
    if (C === undefined) { out.ifaces[n] = null; continue; }
    var rec = { type: typeof C };
    if (typeof C === 'function') {
      rec.fnsrc = srcKind(C);
      rec.hasProto = !!C.prototype;
      if (C.prototype) {
        rec.chain = chain(C.prototype);
        rec.self = nameOf(C.prototype);           // '?' or another name = shared/unnamed prototype
        rec.members = members(C.prototype);
        rec.tag = tag(C.prototype);
        rec.ctorOk = C.prototype.constructor === C;
      }
      try { rec.parent = nameOf(Object.getPrototypeOf(C)) === '?' ? String(Object.getPrototypeOf(C).name || '?') : nameOf(Object.getPrototypeOf(C)); } catch (e) { rec.parent = '!'; }
    }
    out.ifaces[n] = rec;
  }
  var S = {
    window: G, document: G.document, navigator: G.navigator, screen: G.screen,
    performance: G.performance, history: G.history, location: G.location,
    localStorage: (function () { try { return G.localStorage; } catch (e) { return undefined; } })(),
    body: G.document && G.document.body, div: G.document && G.document.createElement('div'),
    textNode: G.document && G.document.createTextNode('x'), crypto: G.crypto, visualViewport: G.visualViewport
  };
  for (var s in S) {
    var o = S[s];
    if (o === undefined || o === null) { out.singletons[s] = null; continue; }
    out.singletons[s] = { chain: chain(o), tag: tag(o), own: s === 'window' ? Object.getOwnPropertyNames(o).length : Object.getOwnPropertyNames(o) };
  }
  var probes = {
    'Document.prototype.querySelector': function () { return Document.prototype.querySelector; },
    'Element.prototype.querySelector': function () { return Element.prototype.querySelector; },
    'EventTarget.prototype.addEventListener': function () { return EventTarget.prototype.addEventListener; },
    'Node.prototype.appendChild': function () { return Node.prototype.appendChild; },
    'document.createElement': function () { return document.createElement; },
    'setTimeout': function () { return setTimeout; }, 'fetch': function () { return fetch; },
    'navigator.userAgent getter': function () { var d = Object.getOwnPropertyDescriptor(Navigator.prototype, 'userAgent'); return d && d.get; },
    'HTMLCanvasElement.prototype.getContext': function () { return HTMLCanvasElement.prototype.getContext; },
    'HTMLCanvasElement.prototype.toDataURL': function () { return HTMLCanvasElement.prototype.toDataURL; },
    'Function.prototype.toString': function () { return Function.prototype.toString; }
  };
  for (var p in probes) {
    var f; try { f = probes[p](); } catch (e) { f = undefined; }
    out.native[p] = typeof f === 'function' ? srcKind(f) + (('prototype' in f) ? '+proto' : '') : String(typeof f);
  }
  return out;
}
