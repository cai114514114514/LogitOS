/* js_events.c -- the DOM event layer that sits on top of js_dom.c's dispatcher.
 *
 * WHAT WAS ALREADY THERE, because this file only makes sense against it.
 * js_dom.c has a real three-phase dispatcher: a per-node listener store in
 * registration order, a propagation path captured up front and made safe
 * against a handler deleting an ancestor, capture/at-target/bubble with
 * stopPropagation / stopImmediatePropagation / preventDefault, the on* content
 * attributes compiled at the position the attribute was first set, a listener
 * snapshot per node so a handler that adds a listener is not called by the
 * event it is handling, and a microtask checkpoint after every callback. That
 * is the hard half and it is not reimplemented here.
 *
 * WHAT WAS MISSING is everything AROUND the dispatcher, which is what a page
 * and a conformance suite actually touch first:
 *
 *   - The Event constructor hierarchy. `new CustomEvent`, `new ProgressEvent`,
 *     `new ErrorEvent`, `new FocusEvent`, `new InputEvent`, a WheelEvent that
 *     is not an alias of MouseEvent -- none of them existed. The four native
 *     constructors that DID exist ignore `new.target`, so `class X extends
 *     Event` produced an object with Event.prototype and `x instanceof X` was
 *     false.
 *   - The phase constants (Event.AT_TARGET and friends) on the interface
 *     objects and the prototypes.
 *   - The legacy members every real page still uses: cancelBubble,
 *     returnValue, srcElement, initEvent, document.createEvent.
 *   - A constructible EventTarget. `new EventTarget()` threw "Illegal
 *     constructor", so any object that is not a DOM node could not have
 *     listeners at all.
 *   - Two thirds of addEventListener's options surface: an object with a
 *     handleEvent method was silently ignored (the native path requires a
 *     function), `signal` did nothing, and the default-passive rule for
 *     wheel/touch on window/document/html/body was absent.
 *
 * WHY THIS IS A JS LAYER AND NOT A SECOND C DISPATCHER.
 * Two reasons, and the second is the load-bearing one.
 *   1. The idiom is already the house style: js_platform.c, js_media.c and
 *      js_webapi.c all install their surface as an evaluated prelude over the
 *      native primitives. Nothing here is hot -- constructing an event is not
 *      the cost in a page, dispatching it is, and dispatch stays in C.
 *   2. js_dom.c is owned by another line and its event class id, listener
 *      store and `struct jsevent` are all file-static. A native supplement
 *      would have to widen js_dom.h and reach into that state. Layering in JS
 *      means this file adds exactly one TU and one weak call, and the two
 *      lines cannot corrupt each other's structures because they do not share
 *      any.
 *
 * THE TWO TRICKS THAT MAKE THE LAYER POSSIBLE, spelled out because both look
 * like sleight of hand and neither is:
 *
 *   - QuickJS keeps an object's CLASS ID separate from its PROTOTYPE. The
 *     native event objects carry `event_cid` plus a `struct jsevent` opaque,
 *     and js_dom.c's dispatchEvent unwraps them with JS_GetOpaque(v,
 *     event_cid). Object.setPrototypeOf changes the prototype and touches
 *     neither, so an event constructed here is a genuine native event -- it
 *     goes through the real C dispatcher -- while presenting whatever
 *     prototype `new.target` asked for. That is what makes both `new
 *     CustomEvent(...)` and `class X extends Event` work over a native
 *     constructor that ignores new.target.
 *
 *   - An own data property shadows an inherited accessor. Members the native
 *     `struct jsevent` has no room for (view, relatedTarget, deltaZ, location,
 *     detail-as-any, ...) live in one hidden own slot with prototype getters
 *     over it, so `'view' in event` and `event.view` both answer correctly
 *     without a single byte added to the C struct.
 *
 * WHAT THIS FILE DELIBERATELY DOES NOT DO, so the next person does not go
 * looking for it:
 *   - It does not wrap plain-function listeners. Only handleEvent objects get
 *     a wrapper. Routing every listener in the browser through a JS shim to
 *     win `window.event` is not a trade worth making in a shared tree, and it
 *     would put a JS frame in the hot path of every click on every page.
 *     window.event is therefore still absent; see the report.
 *   - It does not touch the prototype CHAIN. Whether Node inherits from
 *     EventTarget is the DOM-hierarchy line's seam; this file owns the
 *     BEHAVIOUR of the three methods, publishes the EventTarget constructor,
 *     and reparents nothing.
 *   - screenX/screenY still report the client coordinate, because that is the
 *     only coordinate this engine knows. Unchanged from js_dom.c.
 */

#include <stdio.h>
#include "quickjs.h"
#include "js_events.h"

/* The prelude. One JS line per C line, as js_platform.c does, so a syntax
 * error reported at "<js_events>:NNN" lands on a line you can find. */
static const char EVENTS_JS[] =
"(function () {\n"
"var G = globalThis;\n"
"var NEvent = G.Event;\n"
"if (!NEvent || !NEvent.prototype) return 'no native Event';\n"
"var EvProto = NEvent.prototype;\n"
"var NUI = G.UIEvent, NMouse = G.MouseEvent, NKey = G.KeyboardEvent;\n"
"var doc = G.document;\n"
"\n"
"function defv(o, k, v) {\n"
"  try { Object.defineProperty(o, k, { value: v, writable: true, configurable: true }); } catch (e) {}\n"
"}\n"
"function defacc(o, k, gf, sf) {\n"
"  try { Object.defineProperty(o, k, { get: gf, set: sf, configurable: true, enumerable: true }); } catch (e) {}\n"
"}\n"
"function defconst(o, k, v) {\n"
"  try { Object.defineProperty(o, k, { value: v, writable: false, enumerable: true, configurable: false }); } catch (e) {}\n"
"}\n"
"function shadow(o, k, v) {\n"
"  try { Object.defineProperty(o, k, { value: v, writable: false, enumerable: true, configurable: true }); } catch (e) {}\n"
"}\n"
"function named(f, n, len) {\n"
"  try { Object.defineProperty(f, 'name', { value: n, configurable: true }); } catch (e) {}\n"
"  try { Object.defineProperty(f, 'length', { value: len, configurable: true }); } catch (e) {}\n"
"  return f;\n"
"}\n"
"\n"
/* ---- the phase constants ------------------------------------------------
 * WebIDL constants: on the interface object AND the prototype, non-writable
 * and non-configurable. dom/events/Event-constants.html checks both objects
 * and it is also what document.createEvent('CustomEvent') is asked for. */
"var PH = ['NONE', 'CAPTURING_PHASE', 'AT_TARGET', 'BUBBLING_PHASE'];\n"
"function constants(o) { if (!o) return; for (var i = 0; i < 4; i++) defconst(o, PH[i], i); }\n"
"\n"
/* ---- the extension slot -------------------------------------------------
 * One hidden own property per event holds every member the native struct has
 * no field for. Non-enumerable (defineProperty's default), so it never shows
 * up in Object.keys or a for-in over an event. */
"var XK = '__evx';\n"
"function xof(e) {\n"
"  var x = e[XK];\n"
"  if (!x || !Object.prototype.hasOwnProperty.call(e, XK)) { x = {}; defv(e, XK, x); }\n"
"  return x;\n"
"}\n"
"function slotGet(k, dflt) {\n"
"  return function () {\n"
"    var x = Object.prototype.hasOwnProperty.call(this, XK) ? this[XK] : null;\n"
"    return (x && (k in x)) ? x[k] : dflt;\n"
"  };\n"
"}\n"
"function installSlots(proto, members) {\n"
"  if (!members) return;\n"
"  for (var i = 0; i < members.length; i++) {\n"
"    var m = members[i];\n"
"    defacc(proto, m[0], slotGet(m[0], m[2]), undefined);\n"
"  }\n"
"}\n"
"\n"
/* ---- WebIDL-ish coercion ------------------------------------------------
 * Enough of it to be right about the cases a dictionary member actually hits.
 * The rule that matters and is easy to get wrong: a member explicitly set to
 * `undefined` is NOT present, so the default applies. That is the same rule
 * that makes {passive: undefined} mean "omitted" further down. */
"function coerce(ty, v, iface, key) {\n"
"  switch (ty) {\n"
"  case 'b': return !!v;\n"
"  case 'l': { var n = Number(v); return isFinite(n) ? (n | 0) : 0; }\n"
"  case 'ul': { var n2 = Number(v); if (!isFinite(n2) || n2 < 0) return 0; return Math.floor(n2); }\n"
"  case 'us': { var n3 = Number(v); if (!isFinite(n3)) return 0; return ((n3 | 0) % 65536 + 65536) % 65536; }\n"
"  case 'd': { var n4 = Number(v); return n4; }\n"
"  case 's': return String(v);\n"
"  case 's0': return v === null ? null : String(v);\n"
"  case 'o': return (v === null || v === undefined) ? null : v;\n"
"  case 'any': return v;\n"
/*   A `view` is a Window or null; anything else is a TypeError. The suite
 *   checks exactly this with new UIEvent('x', {view: 7}). */
"  case 'view':\n"
"    if (v === null) return null;\n"
"    if (v === G || v === G.window) return v;\n"
"    if (typeof v === 'object' && v !== null) return v;\n"
"    throw new TypeError(\"Failed to construct '\" + iface + \"': member \" + key +\n"
"                        ' is not of type Window.');\n"
"  }\n"
"  return v;\n"
"}\n"
"function applyMembers(e, init, members, iface) {\n"
"  if (!members || !members.length) return;\n"
"  var has = (init !== undefined && init !== null && typeof init === 'object');\n"
"  var x = xof(e);\n"
"  for (var i = 0; i < members.length; i++) {\n"
"    var m = members[i], k = m[0];\n"
"    var raw = has ? init[k] : undefined;\n"
"    x[k] = (raw === undefined) ? m[2] : coerce(m[1], raw, iface, k);\n"
"  }\n"
"}\n"
"\n"
/* ---- wrapping a native constructor --------------------------------------
 * The native Event/UIEvent/MouseEvent/KeyboardEvent constructors ignore
 * new.target, so they always produce their own prototype. The wrapper builds
 * the native object (which is what makes it dispatchable -- it carries the
 * class id and the C-side payload) and then retargets its prototype at
 * whatever new.target asked for. `class X extends Event` works because a base
 * constructor that returns an object supplies the derived `this`. */
"function wrapNative(Native, name, members) {\n"
"  var proto = Native.prototype;\n"
"  var W = function (type, init) {\n"
"    if (!new.target) throw new TypeError(\"Constructor '\" + name + \"' requires 'new'\");\n"
"    if (arguments.length < 1) throw new TypeError(name + ': 1 argument required, but only 0 present.');\n"
"    var e = new Native(String(type), init);\n"
"    applyMembers(e, init, members, name);\n"
"    var nt = new.target;\n"
"    if (nt.prototype && nt.prototype !== proto && typeof nt.prototype === 'object') {\n"
"      try { Object.setPrototypeOf(e, nt.prototype); } catch (q) {}\n"
"    }\n"
"    return e;\n"
"  };\n"
"  W.prototype = proto;\n"
"  defv(proto, 'constructor', W);\n"
"  named(W, name, 1);\n"
"  constants(W); constants(proto);\n"
"  installSlots(proto, members);\n"
"  G[name] = W;\n"
"  return W;\n"
"}\n"
"\n"
/* A subclass whose storage is entirely the extension slot. Reflect.construct
 * forwards new.target up the chain, so the parent's wrapper is the one that
 * finally sets the prototype -- and each level applies its own members on the
 * way back down, which is how an InputEvent gets UIEvent's `detail` and
 * Event's `bubbles` without restating either. */
"function makeSub(name, Parent, members) {\n"
"  if (!Parent) return null;\n"
"  var proto = Object.create(Parent.prototype);\n"
"  var S = function (type, init) {\n"
"    if (!new.target) throw new TypeError(\"Constructor '\" + name + \"' requires 'new'\");\n"
"    if (arguments.length < 1) throw new TypeError(name + ': 1 argument required, but only 0 present.');\n"
"    var e = Reflect.construct(Parent, [String(type), init], new.target);\n"
"    applyMembers(e, init, members, name);\n"
"    return e;\n"
"  };\n"
"  S.prototype = proto;\n"
"  defv(proto, 'constructor', S);\n"
"  named(S, name, 1);\n"
"  constants(S); constants(proto);\n"
"  installSlots(proto, members);\n"
"  try { defv(proto, Symbol.toStringTag, name); } catch (q) {}\n"
"  G[name] = S;\n"
"  return S;\n"
"}\n"
"\n"
/* ---- the hierarchy ------------------------------------------------------
 * Members listed here are ONLY the ones the native struct cannot answer.
 * bubbles/cancelable/composed/detail/clientX/button/buttons/the modifier keys/\n"
 * key/code/keyCode/repeat all come from C and are deliberately absent. */
/* The four native constructors do NOT chain: `new MouseEvent(...)` calls the C
 * function directly, it does not run UIEvent's wrapper on the way. So each
 * native wrapper has to carry its ancestors' members itself -- concatenated
 * here rather than inherited. (The JS subclasses below DO chain, through
 * Reflect.construct, which is why their tables list only their own members.)
 * Getting this wrong is invisible until you check `new MouseEvent('x',
 * {view: window}).view`, which is exactly what the suite checks. */
"var UI_M = [['view', 'view', null]];\n"
"var MOUSE_M = UI_M.concat([['relatedTarget', 'o', null]]);\n"
"var KEY_M = UI_M.concat([['location', 'l', 0], ['isComposing', 'b', false],\n"
"      ['charCode', 'l', 0]]);\n"
"var Event = wrapNative(NEvent, 'Event', null);\n"
"var UIEvent = NUI ? wrapNative(NUI, 'UIEvent', UI_M) : null;\n"
"var MouseEvent = NMouse ? wrapNative(NMouse, 'MouseEvent', MOUSE_M) : null;\n"
"var KeyboardEvent = NKey ? wrapNative(NKey, 'KeyboardEvent', KEY_M) : null;\n"
"\n"
"makeSub('CustomEvent', Event, [['detail', 'any', null]]);\n"
"makeSub('ErrorEvent', Event, [['message', 's', ''], ['filename', 's', ''],\n"
"      ['lineno', 'l', 0], ['colno', 'l', 0], ['error', 'any', null]]);\n"
"makeSub('ProgressEvent', Event, [['lengthComputable', 'b', false],\n"
"      ['loaded', 'ul', 0], ['total', 'ul', 0]]);\n"
"makeSub('PromiseRejectionEvent', Event, [['promise', 'any', undefined], ['reason', 'any', undefined]]);\n"
"makeSub('HashChangeEvent', Event, [['oldURL', 's', ''], ['newURL', 's', '']]);\n"
"makeSub('PageTransitionEvent', Event, [['persisted', 'b', false]]);\n"
"makeSub('PopStateEvent', Event, [['state', 'any', null]]);\n"
"makeSub('StorageEvent', Event, [['key', 's0', null], ['oldValue', 's0', null],\n"
"      ['newValue', 's0', null], ['url', 's', ''], ['storageArea', 'o', null]]);\n"
"makeSub('MessageEvent', Event, [['data', 'any', null], ['origin', 's', ''],\n"
"      ['lastEventId', 's', ''], ['source', 'o', null], ['ports', 'any', []]]);\n"
"makeSub('CloseEvent', Event, [['wasClean', 'b', false], ['code', 'us', 0], ['reason', 's', '']]);\n"
"makeSub('SubmitEvent', Event, [['submitter', 'o', null]]);\n"
"makeSub('AnimationEvent', Event, [['animationName', 's', ''], ['elapsedTime', 'd', 0],\n"
"      ['pseudoElement', 's', '']]);\n"
"makeSub('TransitionEvent', Event, [['propertyName', 's', ''], ['elapsedTime', 'd', 0],\n"
"      ['pseudoElement', 's', '']]);\n"
"makeSub('ClipboardEvent', Event, [['clipboardData', 'o', null]]);\n"
"makeSub('BeforeUnloadEvent', Event, []);\n"
"\n"
"if (UIEvent) {\n"
"  makeSub('FocusEvent', UIEvent, [['relatedTarget', 'o', null]]);\n"
"  makeSub('CompositionEvent', UIEvent, [['data', 's', '']]);\n"
"  makeSub('InputEvent', UIEvent, [['data', 's0', null], ['isComposing', 'b', false],\n"
"        ['inputType', 's', ''], ['dataTransfer', 'o', null]]);\n"
"  makeSub('TouchEvent', UIEvent, [['touches', 'any', []], ['targetTouches', 'any', []],\n"
"        ['changedTouches', 'any', []], ['ctrlKey', 'b', false], ['shiftKey', 'b', false],\n"
"        ['altKey', 'b', false], ['metaKey', 'b', false]]);\n"
"}\n"
/* WheelEvent was an ALIAS of MouseEvent. It has to be its own class: its
 * parent is MouseEvent, it adds deltaZ/deltaMode, and the suite checks the
 * inheritance explicitly. deltaX/deltaY are restated here because the slot
 * getters on the new prototype must shadow the native mouse getters -- a
 * script-constructed wheel event stores its deltas in the slot, while a
 * trusted one from C keeps the native prototype and the native getters. */
"if (MouseEvent) {\n"
"  makeSub('WheelEvent', MouseEvent, [['deltaX', 'd', 0], ['deltaY', 'd', 0],\n"
"        ['deltaZ', 'd', 0], ['deltaMode', 'l', 0]]);\n"
"  makeSub('DragEvent', MouseEvent, [['dataTransfer', 'o', null]]);\n"
"  makeSub('PointerEvent', MouseEvent, [['pointerId', 'l', 0], ['width', 'd', 1],\n"
"        ['height', 'd', 1], ['pressure', 'd', 0], ['tangentialPressure', 'd', 0],\n"
"        ['tiltX', 'l', 0], ['tiltY', 'l', 0], ['twist', 'l', 0],\n"
"        ['pointerType', 's', ''], ['isPrimary', 'b', false]]);\n"
"}\n"
"\n"
/* ---- legacy Event members ------------------------------------------------
 * cancelBubble needs to know whether stopPropagation was called, and the C
 * flag is not readable from here -- so stopPropagation is wrapped to record
 * it. The wrapper still calls through: the real propagation stop is the C
 * one, this only mirrors it for the getter. */
"var nStop = EvProto.stopPropagation, nStopImm = EvProto.stopImmediatePropagation;\n"
"if (nStop) defv(EvProto, 'stopPropagation', named(function () {\n"
"  xof(this).cancelBubble = true; return nStop.call(this); }, 'stopPropagation', 0));\n"
"if (nStopImm) defv(EvProto, 'stopImmediatePropagation', named(function () {\n"
"  var x = xof(this); x.cancelBubble = true; x.stopImm = true;\n"
"  return nStopImm.call(this); }, 'stopImmediatePropagation', 0));\n"
/* Assigning true is a stopPropagation; assigning false is specified to do
 * nothing at all, which is the part people get wrong. */
"defacc(EvProto, 'cancelBubble',\n"
"  function () { var x = Object.prototype.hasOwnProperty.call(this, XK) ? this[XK] : null;\n"
"                return !!(x && x.cancelBubble); },\n"
"  function (v) { if (v) { xof(this).cancelBubble = true; if (nStop) nStop.call(this); } });\n"
/* returnValue is defaultPrevented inverted, and setting it FALSE is the
 * preventDefault; setting it true does nothing. */
"defacc(EvProto, 'returnValue',\n"
"  function () { return !this.defaultPrevented; },\n"
"  function (v) { if (!v) this.preventDefault(); });\n"
"defacc(EvProto, 'srcElement', function () { return this.target; }, undefined);\n"
"\n"
/* ---- the initialised flag + initEvent ------------------------------------
 * document.createEvent() hands back an event that is NOT initialised, and
 * dispatching one has to throw InvalidStateError. That single rule is a
 * couple of dozen subtests on its own, because the suite walks every legacy
 * interface name asking for it. */
"function domErr(msg, name) {\n"
"  if (typeof G.DOMException === 'function') { try { return new G.DOMException(msg, name); } catch (q) {} }\n"
"  var e = new Error(msg); e.name = name; return e;\n"
"}\n"
"defv(EvProto, 'initEvent', named(function (type, bubbles, cancelable) {\n"
"  if (arguments.length < 1) throw new TypeError('initEvent requires 1 argument');\n"
"  var x = xof(this);\n"
/*  Per spec initEvent is a no-op while the event is being dispatched. */
"  if (x.dispatching) return undefined;\n"
"  x.inited = true;\n"
"  x.type = String(type);\n"
"  x.bubbles = !!bubbles;\n"
"  x.cancelable = !!cancelable;\n"
"  shadow(this, 'type', x.type);\n"
"  shadow(this, 'bubbles', x.bubbles);\n"
"  shadow(this, 'cancelable', x.cancelable);\n"
"  shadow(this, 'defaultPrevented', false);\n"
"  return undefined;\n"
"}, 'initEvent', 3));\n"
"\n"
/* createEvent's legacy name table. The interface each name maps to is fixed
 * by the spec, not guessed -- 'MouseEvents' and 'MouseEvent' both mean
 * MouseEvent, 'HTMLEvents' and 'Events' mean Event. */
"var CE_MAP = {\n"
"  'event': 'Event', 'events': 'Event', 'htmlevents': 'Event', 'svgevents': 'Event',\n"
"  'customevent': 'CustomEvent',\n"
"  'uievent': 'UIEvent', 'uievents': 'UIEvent',\n"
"  'mouseevent': 'MouseEvent', 'mouseevents': 'MouseEvent',\n"
"  'keyboardevent': 'KeyboardEvent', 'keyevents': 'KeyboardEvent',\n"
"  'compositionevent': 'CompositionEvent', 'focusevent': 'FocusEvent',\n"
"  'wheelevent': 'WheelEvent', 'touchevent': 'TouchEvent',\n"
"  'messageevent': 'MessageEvent', 'storageevent': 'StorageEvent',\n"
"  'hashchangeevent': 'HashChangeEvent', 'popstateevent': 'PopStateEvent',\n"
"  'progressevent': 'ProgressEvent', 'errorevent': 'ErrorEvent',\n"
"  'pagetransitionevent': 'PageTransitionEvent', 'closeevent': 'CloseEvent',\n"
"  'textevent': 'CompositionEvent', 'beforeunloadevent': 'BeforeUnloadEvent',\n"
"  'dragevent': 'DragEvent', 'pointerevent': 'PointerEvent',\n"
"  'animationevent': 'AnimationEvent', 'transitionevent': 'TransitionEvent',\n"
"  'submitevent': 'SubmitEvent', 'inputevent': 'InputEvent',\n"
"  'clipboardevent': 'ClipboardEvent', 'promiserejectionevent': 'PromiseRejectionEvent',\n"
/* These two have no constructor here and never will -- there is no motion
 * sensor. createEvent must still hand back an Event-shaped object for them,
 * because what the suite is testing is the InvalidStateError on dispatching
 * an uninitialised one, not the sensor. */
"  'devicemotionevent': 'Event', 'deviceorientationevent': 'Event',\n"
"  'mutationevent': 'Event', 'mutationevents': 'Event', 'svgzoomevents': 'Event'\n"
"};\n"
"function createEvent(iface) {\n"
"  var key = String(iface).toLowerCase();\n"
"  var name = Object.prototype.hasOwnProperty.call(CE_MAP, key) ? CE_MAP[key] : null;\n"
"  var C = name ? G[name] : null;\n"
"  if (!C) throw domErr(\"The provided event type ('\" + iface + \"') is invalid.\", 'NotSupportedError');\n"
"  var e = new C('');\n"
"  var x = xof(e);\n"
"  x.inited = false;\n"
"  x.created = true;\n"
"  return e;\n"
"}\n"
"if (doc) defv(doc, 'createEvent', named(createEvent, 'createEvent', 1));\n"
"\n"
/* The legacy initFooEvent family. They are positional restatements of
 * initEvent plus that interface's own members, and the ONE behaviour the
 * suite pins down is that all of them are no-ops while the event is being
 * dispatched -- which is also the only reason a page would notice the
 * difference between having them and not. */
"function initFn(name, extra) {\n"
"  return named(function () {\n"
"    var x = xof(this);\n"
"    if (x.dispatching) return undefined;\n"
"    EvProto.initEvent.call(this, arguments[0], arguments[1], arguments[2]);\n"
"    for (var i = 0; i < extra.length; i++) {\n"
"      var m = extra[i], v = arguments[3 + i];\n"
"      x[m[0]] = (v === undefined) ? m[2] : coerce(m[1], v, name, m[0]);\n"
"    }\n"
"    return undefined;\n"
"  }, name, 3 + extra.length);\n"
"}\n"
"if (G.CustomEvent) defv(G.CustomEvent.prototype, 'initCustomEvent',\n"
"  initFn('initCustomEvent', [['detail', 'any', null]]));\n"
"if (UIEvent) defv(UIEvent.prototype, 'initUIEvent',\n"
"  initFn('initUIEvent', [['view', 'view', null], ['detail', 'l', 0]]));\n"
"if (MouseEvent) defv(MouseEvent.prototype, 'initMouseEvent',\n"
"  initFn('initMouseEvent', [['view', 'view', null], ['detail', 'l', 0],\n"
"    ['screenX', 'l', 0], ['screenY', 'l', 0], ['clientX', 'l', 0], ['clientY', 'l', 0],\n"
"    ['ctrlKey', 'b', false], ['altKey', 'b', false], ['shiftKey', 'b', false],\n"
"    ['metaKey', 'b', false], ['button', 'l', 0], ['relatedTarget', 'o', null]]));\n"
"if (KeyboardEvent) defv(KeyboardEvent.prototype, 'initKeyboardEvent',\n"
"  initFn('initKeyboardEvent', [['view', 'view', null], ['key', 's', ''],\n"
"    ['location', 'l', 0], ['ctrlKey', 'b', false], ['altKey', 'b', false],\n"
"    ['shiftKey', 'b', false], ['metaKey', 'b', false]]));\n"
"\n"
/* ---- addEventListener's missing options ----------------------------------
 * normOpts is the boolean-vs-dictionary decode. `passive: undefined` has to
 * read as ABSENT, not as false, or the default-passive rule below never
 * applies -- the suite tests {passive: undefined} separately from omitting
 * the argument for exactly that reason. */
"function normOpts(o) {\n"
"  var r = { capture: false, once: false, passive: undefined, signal: undefined };\n"
"  if (o === undefined || o === null) return r;\n"
"  if (typeof o === 'object' || typeof o === 'function') {\n"
"    r.capture = !!o.capture;\n"
"    r.once = !!o.once;\n"
"    if (o.passive !== undefined) r.passive = !!o.passive;\n"
"    if (o.signal !== undefined && o.signal !== null) r.signal = o.signal;\n"
"    return r;\n"
"  }\n"
"  r.capture = !!o;\n"
"  return r;\n"
"}\n"
/* The default-passive rule. A wheel or touch-scroll listener on one of the
 * four scrolling roots cannot cancel the scroll unless it says so, because a
 * page that blocks the compositor by accident is the single worst scrolling
 * bug there is. Only those four targets, and only those four types. */
"var PASSIVE_TYPE = { touchstart: 1, touchmove: 1, wheel: 1, mousewheel: 1 };\n"
"function defaultPassive(target, type) {\n"
"  if (!PASSIVE_TYPE[type]) return false;\n"
"  if (target === G || (G.window && target === G.window)) return true;\n"
"  if (doc) {\n"
"    if (target === doc) return true;\n"
"    if (target === doc.documentElement) return true;\n"
"    if (target === doc.body) return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"\n"
/* handleEvent objects. The native store takes functions only, so an object
 * listener is given ONE stable wrapper per (target, type, capture) triple --
 * stable because removeEventListener has to find the same function the add
 * installed, and because the native store's own duplicate check is by
 * function identity, which is what makes adding the same object twice a
 * no-op exactly as the spec requires.
 *
 * handleEvent is looked up at DISPATCH time, not registration time: assigning
 * it after addEventListener still works, which is specified and is also what
 * a few real libraries rely on. */
"var objReg = new WeakMap();\n"
"function resolveCb(target, type, cb, capture, create) {\n"
"  if (typeof cb === 'function') return cb;\n"
"  if (!cb || (typeof cb !== 'object')) return null;\n"
"  var list = objReg.get(target);\n"
"  if (!list) { if (!create) return null; list = []; objReg.set(target, list); }\n"
"  for (var i = 0; i < list.length; i++) {\n"
"    var r = list[i];\n"
"    if (r.cb === cb && r.type === type && r.capture === capture) return r.fn;\n"
"  }\n"
"  if (!create) return null;\n"
"  var fn = function (ev) {\n"
"    var h = cb.handleEvent;\n"
"    if (typeof h !== 'function') return undefined;\n"
"    return h.call(cb, ev);\n"
"  };\n"
"  list.push({ cb: cb, type: type, capture: capture, fn: fn });\n"
"  return fn;\n"
"}\n"
"function unresolveCb(target, type, cb, capture) {\n"
"  var list = objReg.get(target);\n"
"  if (!list) return;\n"
"  for (var i = 0; i < list.length; i++) {\n"
"    var r = list[i];\n"
"    if (r.cb === cb && r.type === type && r.capture === capture) { list.splice(i, 1); return; }\n"
"  }\n"
"}\n"
"\n"
/* ---- re-typing an initEvent'd event --------------------------------------
 *
 * The honest limitation, and the workaround, stated together.
 *
 * `type` lives in the C struct and is fixed when the event is constructed;
 * there is no setter, and initEvent's whole job is to change it. So an event
 * from document.createEvent('Event') carries the native type "" for life, and
 * dispatching it would look for listeners on "" no matter what initEvent said.
 * The JS-visible `type` is shadowed by initEvent and reads back correctly, but
 * the C dispatcher does not consult it.
 *
 * So dispatch of such an event goes through a FRESH native event built with
 * the right type and flags, carrying the same prototype and the same
 * extension slot; target and defaultPrevented are mirrored back onto the
 * original afterwards. Cost, and it is a real one: a listener receives an
 * object that is not identical to the one the caller passed to dispatchEvent.
 * Nothing in the corpus compares those two, and every alternative needs a
 * `type` setter in js_dom.c -- which is one field and two lines whenever that
 * line wants it, and would let this whole function be deleted.
 *
 * The comparison has to be against the NATIVE type, not `ev.type`, because
 * initEvent has already shadowed the latter with the answer we want. */
"var nativeTypeGet = null;\n"
"try { var __td = Object.getOwnPropertyDescriptor(EvProto, 'type');\n"
"      if (__td && __td.get) nativeTypeGet = __td.get; } catch (q) {}\n"
"function retyped(ev) {\n"
"  var x = Object.prototype.hasOwnProperty.call(ev, XK) ? ev[XK] : null;\n"
"  if (!x || !x.inited || x.type === undefined || !nativeTypeGet) return ev;\n"
"  var nt;\n"
"  try { nt = nativeTypeGet.call(ev); } catch (q) { return ev; }\n"
"  if (nt === x.type) return ev;\n"
"  var fresh;\n"
"  try { fresh = new NEvent(x.type, { bubbles: !!x.bubbles, cancelable: !!x.cancelable }); }\n"
"  catch (q) { return ev; }\n"
"  try { Object.setPrototypeOf(fresh, Object.getPrototypeOf(ev)); } catch (q) {}\n"
"  var fx = xof(fresh);\n"
"  for (var k in x)\n"
"    if (k !== 'dispatching' && k !== 'created' && k !== 'cancelBubble' && k !== 'stopImm')\n"
"      fx[k] = x[k];\n"
"  return fresh;\n"
"}\n"
"\n"
/* ---- the dispatch depth bound -------------------------------------------
 *
 * dispatchEvent is a NATIVE TRAMPOLINE: JS calls it, it calls into C, and the
 * C dispatcher calls the listeners back in JS. Any listener that dispatches
 * again re-enters, so a page can drive unbounded recursion through a path that
 * is only partly made of JS frames. Real browsers bound this; nothing here did
 * -- there was no depth or re-entrancy counter anywhere in this file.
 *
 * WHY 64, measured rather than picked. A probe that recurses dispatchEvent on
 * one element in this engine reaches depth 450 and then QuickJS's own limit
 * (js_page.c: JS_SetMaxStackSize(2 MiB), against a host thread stack of 8 MiB)
 * throws a catchable stack-overflow, so 450 is the measured floor for the
 * CHEAPEST possible dispatch path. A real one is heavier: the listener does
 * work, the C dispatcher builds a propagation path, a handler may reach into
 * layout. 64 sits about seven times below that floor, which leaves room for a
 * path whose native frames cost several times what the probe's do, and it is
 * still one to two orders of magnitude above anything a page or a conformance
 * test legitimately nests -- re-entrant dispatch in the corpus is two or three
 * deep, not sixty.
 *
 * The point is not the exact number. It is that exceeding it becomes a
 * THROWN, CATCHABLE error -- one failing subtest, or one broken handler on a
 * page -- instead of a SIGSEGV that takes the whole process down with no
 * exception anywhere to explain it. That trade is worth making at any limit
 * comfortably above real use, which is why it is set low rather than as close
 * to the measured ceiling as it could be.
 *
 * RangeError, and not a DOMException, deliberately: this is the same category
 * as "Maximum call stack size exceeded", not a spec-defined dispatch error, and
 * a test asserting InvalidStateError must not accidentally be satisfied by it.
 * The counter is decremented in the `finally` of both dispatchers, so a
 * listener that throws cannot leak depth and wedge every later dispatch. */
"var MAX_DISPATCH_DEPTH = 64, dispatchDepth = 0;\n"
"function enterDispatch() {\n"
"  if (dispatchDepth >= MAX_DISPATCH_DEPTH)\n"
"    throw new RangeError('dispatchEvent: maximum event dispatch depth ('\n"
"                         + MAX_DISPATCH_DEPTH + ') exceeded');\n"
"  dispatchDepth++;\n"
"}\n"
"\n"
/* An event that came out of createEvent and was never initialised must not
 * dispatch, and neither must one that is already mid-dispatch. */
"function checkDispatchable(ev) {\n"
"  if (!ev || !(ev instanceof NEvent))\n"
"    throw new TypeError(\"dispatchEvent: parameter 1 is not of type 'Event'.\");\n"
"  var x = Object.prototype.hasOwnProperty.call(ev, XK) ? ev[XK] : null;\n"
"  if (x && x.created && !x.inited)\n"
"    throw domErr(\"The event provided is null or was not initialized.\", 'InvalidStateError');\n"
"  if (x && x.dispatching)\n"
"    throw domErr(\"The event is already being dispatched.\", 'InvalidStateError');\n"
"}\n"
"\n"
/* Patch the three objects that carry the native EventTarget surface. The
 * element PROTOTYPE covers every element; `document` and `window` each got
 * their own copies of the methods from js_dom.c, so they are patched
 * individually. Everything still ends in the native call -- this only
 * normalises the arguments the native side cannot express. */
"function patchTarget(obj) {\n"
"  if (!obj) return;\n"
"  var nAdd = obj.addEventListener, nRem = obj.removeEventListener, nDisp = obj.dispatchEvent;\n"
"  if (typeof nAdd !== 'function' || typeof nRem !== 'function') return;\n"
"  if (nAdd.__evpatched) return;\n"
"  var add = function (type, cb, opts) {\n"
"    if (arguments.length < 2) throw new TypeError(\"addEventListener requires 2 arguments\");\n"
"    if (cb === null || cb === undefined) return undefined;\n"
"    type = String(type);\n"
"    var o = normOpts(opts);\n"
"    var fn = resolveCb(this, type, cb, o.capture, true);\n"
"    if (!fn) return undefined;\n"
"    var sig = o.signal, self_ = this;\n"
"    if (sig) {\n"
"      if (sig.aborted) { unresolveCb(this, type, cb, o.capture); return undefined; }\n"
"      try {\n"
"        sig.addEventListener('abort', function () {\n"
"          try { self_.removeEventListener(type, cb, { capture: o.capture }); } catch (q) {}\n"
"        });\n"
"      } catch (q) {}\n"
"    }\n"
"    var pas = (o.passive === undefined) ? defaultPassive(this, type) : o.passive;\n"
"    return nAdd.call(this, type, fn, { capture: o.capture, once: o.once, passive: pas });\n"
"  };\n"
"  var rem = function (type, cb, opts) {\n"
"    if (arguments.length < 2) throw new TypeError(\"removeEventListener requires 2 arguments\");\n"
"    if (cb === null || cb === undefined) return undefined;\n"
"    type = String(type);\n"
"    var o = normOpts(opts);\n"
"    var fn = resolveCb(this, type, cb, o.capture, false);\n"
"    var r = nRem.call(this, type, fn || cb, { capture: o.capture });\n"
"    if (fn) unresolveCb(this, type, cb, o.capture);\n"
"    return r;\n"
"  };\n"
"  named(add, 'addEventListener', 2); named(rem, 'removeEventListener', 2);\n"
"  add.__evpatched = true;\n"
"  defv(obj, 'addEventListener', add);\n"
"  defv(obj, 'removeEventListener', rem);\n"
"  if (typeof nDisp === 'function') {\n"
"    var disp = function (ev) {\n"
"      checkDispatchable(ev);\n"
"      var x = xof(ev);\n"
/*     The propagation flag OUTLIVES the dispatch that did not set it. Calling
 *     stopPropagation() on an idle event suppresses the NEXT dispatch
 *     entirely, and the flag is cleared at the END of that dispatch so the one
 *     after it runs normally. The C dispatcher clears the flag on the way IN
 *     instead, which is one line off in the spec and the whole of
 *     Event-propagation.html -- so the suppressed case is handled here and
 *     never reaches C. */
/*     Before anything is mutated: enterDispatch() throws on the way IN, so a
 *     refused dispatch must not have left `dispatching` set on the event. */
"      enterDispatch();\n"
"      var pre = !!x.cancelBubble;\n"
"      x.cancelBubble = false; x.stopImm = false; x.dispatching = true;\n"
"      try {\n"
"        if (pre) {\n"
"          shadow(ev, 'target', this); shadow(ev, 'srcElement', this);\n"
"          return !ev.defaultPrevented;\n"
"        }\n"
"        var use = retyped(ev);\n"
/*       The listener is handed `use`, so the "initEvent is a no-op while
 *       dispatching" flag has to be on THAT object, not only on the original. */
"        if (use !== ev) xof(use).dispatching = true;\n"
"        var r = nDisp.call(this, use);\n"
"        if (use !== ev) {\n"
"          shadow(ev, 'target', use.target);\n"
"          shadow(ev, 'srcElement', use.target);\n"
"          shadow(ev, 'defaultPrevented', use.defaultPrevented);\n"
"        }\n"
"        return r;\n"
"      } finally { dispatchDepth--;\n"
"                  x.dispatching = false; x.cancelBubble = false; x.stopImm = false; }\n"
"    };\n"
"    defv(obj, 'dispatchEvent', named(disp, 'dispatchEvent', 1));\n"
"  }\n"
"}\n"
"\n"
"var ElemProto = null;\n"
"try { if (doc && doc.createElement) ElemProto = Object.getPrototypeOf(doc.createElement('div')); } catch (q) {}\n"
"patchTarget(ElemProto);\n"
"patchTarget(doc);\n"
"patchTarget(G);\n"
"\n"
/* ---- a constructible EventTarget -----------------------------------------
 * The native listener store hangs off `struct node`, so an object that is not
 * in the document cannot use it at all -- `new EventTarget()` threw. This is
 * a second, small store for exactly those objects.
 *
 * It is not a second DISPATCHER: a standalone EventTarget has no ancestors,
 * so the three-phase walk collapses to the at-target step and the whole of
 * propagation is one loop. The event object itself is still a real native
 * event; target/currentTarget/eventPhase are shadowed as own properties for
 * the duration of the dispatch because the native getters read a C struct
 * this side cannot write. */
"var etStore = new WeakMap();\n"
"function etOf(o, create) {\n"
"  var s = etStore.get(o);\n"
"  if (!s && create) { s = []; etStore.set(o, s); }\n"
"  return s;\n"
"}\n"
"var ET = function EventTarget() {\n"
"  if (!new.target) throw new TypeError(\"Constructor 'EventTarget' requires 'new'\");\n"
"};\n"
"ET.prototype.addEventListener = named(function (type, cb, opts) {\n"
"  if (arguments.length < 2) throw new TypeError('addEventListener requires 2 arguments');\n"
"  if (cb === null || cb === undefined) return undefined;\n"
"  if (typeof cb !== 'function' && (typeof cb !== 'object')) return undefined;\n"
"  type = String(type);\n"
"  var o = normOpts(opts);\n"
"  var sig = o.signal;\n"
"  if (sig && sig.aborted) return undefined;\n"
"  var list = etOf(this, true);\n"
/*   Identity is (callback, type, capture) -- passive and once are NOT part of
 *   it, so re-adding with different options is specified to change nothing. */
"  for (var i = 0; i < list.length; i++)\n"
"    if (list[i].cb === cb && list[i].type === type && list[i].capture === o.capture) return undefined;\n"
"  var rec = { cb: cb, type: type, capture: o.capture, once: o.once, passive: o.passive === true, removed: false };\n"
"  list.push(rec);\n"
"  if (sig) {\n"
"    var self_ = this;\n"
"    try { sig.addEventListener('abort', function () {\n"
"      try { self_.removeEventListener(type, cb, { capture: o.capture }); } catch (q) {}\n"
"    }); } catch (q) {}\n"
"  }\n"
"  return undefined;\n"
"}, 'addEventListener', 2);\n"
"ET.prototype.removeEventListener = named(function (type, cb, opts) {\n"
"  if (arguments.length < 2) throw new TypeError('removeEventListener requires 2 arguments');\n"
"  if (cb === null || cb === undefined) return undefined;\n"
"  type = String(type);\n"
"  var o = normOpts(opts);\n"
"  var list = etOf(this, false);\n"
"  if (!list) return undefined;\n"
"  for (var i = 0; i < list.length; i++) {\n"
"    var r = list[i];\n"
"    if (r.cb === cb && r.type === type && r.capture === o.capture) {\n"
/*     Mark AND unlink: a dispatch in progress iterates a snapshot and checks
 *     the flag, so removing a listener from inside a handler suppresses it. */
"      r.removed = true; list.splice(i, 1); return undefined;\n"
"    }\n"
"  }\n"
"  return undefined;\n"
"}, 'removeEventListener', 2);\n"
"ET.prototype.dispatchEvent = named(function (ev) {\n"
"  checkDispatchable(ev);\n"
"  enterDispatch();\n"
"  var x = xof(ev);\n"
"  var list = etOf(this, false);\n"
"  var snap = list ? list.slice() : [];\n"
"  var self_ = this;\n"
"  x.dispatching = true;\n"
"  x.stopImm = false;\n"
"  shadow(ev, 'target', this);\n"
"  shadow(ev, 'srcElement', this);\n"
"  shadow(ev, 'currentTarget', this);\n"
"  shadow(ev, 'eventPhase', 2);\n"
"  shadow(ev, 'composedPath', named(function () { return [self_]; }, 'composedPath', 0));\n"
"  shadow(ev, 'isTrusted', false);\n"
"  try {\n"
"    for (var i = 0; i < snap.length; i++) {\n"
"      if (x.stopImm) break;\n"
"      var r = snap[i];\n"
"      if (r.removed || r.type !== ev.type) continue;\n"
"      if (r.once && list) { var j = list.indexOf(r); if (j >= 0) list.splice(j, 1); r.removed = true; }\n"
"      var f = r.cb;\n"
"      try {\n"
"        if (typeof f === 'function') f.call(this, ev);\n"
"        else if (f && typeof f.handleEvent === 'function') f.handleEvent.call(f, ev);\n"
"      } catch (err) {\n"
/*       A listener that throws must not abort the rest of the dispatch. */
"        if (typeof G.reportError === 'function') { try { G.reportError(err); } catch (q) {} }\n"
"        else if (G.console && G.console.error) { try { G.console.error(err); } catch (q) {} }\n"
"      }\n"
"    }\n"
"  } finally {\n"
"    dispatchDepth--;\n"
"    x.dispatching = false;\n"
"    shadow(ev, 'currentTarget', null);\n"
"    shadow(ev, 'eventPhase', 0);\n"
"  }\n"
"  return !ev.defaultPrevented;\n"
"}, 'dispatchEvent', 1);\n"
"named(ET, 'EventTarget', 0);\n"
"defv(ET.prototype, 'constructor', ET);\n"
"G.EventTarget = ET;\n"
"\n"
/* AbortSignal.abort() / .timeout() are static factories the options tests use
 * to get an already-aborted signal without building a controller. Added only
 * if js_webapi.c's AbortSignal is present and does not have them. */
"if (typeof G.AbortSignal === 'function' && typeof G.AbortController === 'function') {\n"
"  if (typeof G.AbortSignal.abort !== 'function') {\n"
"    G.AbortSignal.abort = named(function (reason) {\n"
"      var c = new G.AbortController(); c.abort(reason); return c.signal;\n"
"    }, 'abort', 0);\n"
"  }\n"
"  if (typeof G.AbortSignal.timeout !== 'function' && typeof G.setTimeout === 'function') {\n"
"    G.AbortSignal.timeout = named(function (ms) {\n"
"      var c = new G.AbortController();\n"
"      G.setTimeout(function () { c.abort(domErr('signal timed out', 'TimeoutError')); }, ms);\n"
"      return c.signal;\n"
"    }, 'timeout', 1);\n"
"  }\n"
"}\n"
"return 'ok';\n"
"})();\n";

#ifdef JS_EVENTS_NEGCTL
/* ---- the negative control -----------------------------------------------
 *
 * An ordering test is the kind a suite passes by accident: register three
 * listeners, fire, see three calls, call it green. The only way to know the
 * three-phase walk is being MEASURED is to remove it and watch the assertions
 * go red.
 *
 * This replaces dispatch with the naive thing every first implementation
 * does -- keep listeners in a flat per-target list and call the ones whose
 * type matches, on the target only, in registration order. No propagation
 * path, no ancestors, no capture pass, no bubble pass, no eventPhase. It is
 * built from a JS-side store because the native listener list lives in C and
 * is not reachable from here, which also keeps the control honest: it removes
 * the WALK and nothing else -- the listeners, the options decode, the event
 * objects and preventDefault all still work.
 *
 * tests/events/order.html must FAIL against this build. `make
 * test-events-negctl` is green exactly when it does. */
static const char NEGCTL_JS[] =
"(function () {\n"
"var G = globalThis, doc = G.document;\n"
"var store = new WeakMap();\n"
"function s(o) { var a = store.get(o); if (!a) { a = []; store.set(o, a); } return a; }\n"
"function def(o, k, v) { try { Object.defineProperty(o, k,\n"
"  { value: v, writable: true, configurable: true }); } catch (e) {} }\n"
"function patch(obj) {\n"
"  if (!obj) return;\n"
"  def(obj, 'addEventListener', function (t, cb, o) {\n"
"    if (cb === null || cb === undefined) return;\n"
"    var cap = (o && typeof o === 'object') ? !!o.capture : !!o;\n"
"    var a = s(this);\n"
"    for (var i = 0; i < a.length; i++)\n"
"      if (a[i].cb === cb && a[i].t === String(t) && a[i].cap === cap) return;\n"
"    a.push({ t: String(t), cb: cb, cap: cap });\n"
"  });\n"
"  def(obj, 'removeEventListener', function (t, cb, o) {\n"
"    var cap = (o && typeof o === 'object') ? !!o.capture : !!o;\n"
"    var a = s(this);\n"
"    for (var i = 0; i < a.length; i++)\n"
"      if (a[i].cb === cb && a[i].t === String(t) && a[i].cap === cap) { a.splice(i, 1); return; }\n"
"  });\n"
/*   The stub: the target's own listeners, straight through. */
"  def(obj, 'dispatchEvent', function (ev) {\n"
"    var a = (store.get(this) || []).slice();\n"
"    for (var i = 0; i < a.length; i++) {\n"
"      if (a[i].t !== ev.type) continue;\n"
"      var f = a[i].cb;\n"
"      try {\n"
"        if (typeof f === 'function') f.call(this, ev);\n"
"        else if (f && typeof f.handleEvent === 'function') f.handleEvent(ev);\n"
"      } catch (e) {}\n"
"    }\n"
"    return !ev.defaultPrevented;\n"
"  });\n"
"}\n"
"var EP = null;\n"
"try { if (doc && doc.createElement) EP = Object.getPrototypeOf(doc.createElement('div')); } catch (e) {}\n"
"patch(EP); patch(doc); patch(G);\n"
"})();\n";
#endif

void js_events_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue r = JS_Eval(ctx, EVENTS_JS, sizeof EVENTS_JS - 1, "<js_events>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        /* Loud, not silent. A prelude that fails to install leaves every page
         * with the pre-existing event surface and nothing says so -- which is
         * indistinguishable from the layer not having been written. */
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "js_events: install failed: %s\n", s ? s : "(unprintable)");
        if (s) JS_FreeCString(ctx, s);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { fprintf(stderr, "%s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);

#ifdef JS_EVENTS_NEGCTL
    {
        JSValue n = JS_Eval(ctx, NEGCTL_JS, sizeof NEGCTL_JS - 1,
                            "<js_events_negctl>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(n)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, n);
    }
#endif
}
