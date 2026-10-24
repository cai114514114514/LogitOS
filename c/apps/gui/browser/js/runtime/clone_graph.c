/* SPDX-License-Identifier: MIT */
/* HTML §2.7, https://html.spec.whatwg.org/multipage/structured-data.html
 *
 * Correction beside the old platform.c claim: the previous "real deep clone"
 * handled cycles, but tested instanceof, called x.constructor/x.forEach, and
 * visited array indices with `i in x`. That invoked author replacements,
 * copied inherited indices, and lost enumerable non-index array properties.
 * JS_WriteObject is deliberately not the HTML serializer either: its array
 * path reads every index, its object path rejects getters, and its class switch
 * omits Map, Set, RegExp, Error and DataView. Using it here would make the
 * visible result depend on a bytecode storage format instead of HTML §2.7.
 *
 * This module keeps graph semantics together; clone.c owns the only operations
 * that need native slots or irreversible detachment. Built-in payloads ignore
 * expando properties as required. Shared memory and native host classes without
 * serialization hooks remain honest DataCloneErrors. Resizable buffers are
 * absent in this QuickJS build: if the engine adds their slots, copy/commit must
 * preserve max-length state rather than silently treating them as fixed buffers.
 * The implementation-only phase claimed no conformance measurement.
 * Correction 2026-09-30: tests/html_clone.mk now runs the public fixture through
 * real WebAPI Blob/File hooks on the host: 58 checks, zero failures, also under
 * ASan/UBSan. Disabling only detach with JS_CLONE_KEEP_BUFFER ran all 58 and
 * failed exactly eight source/view detachment checks. This is host evidence;
 * it does not prove Window bootstrap, MessagePort packets, or guest behavior. */
#include "js/runtime/clone_graph.h"

const char js_clone_graph[] =
"(function (brand, copyBuffer, readView, commitBuffers, toStringValue, suppliedHooks) {\n"
"  'use strict';\n"
"  var G = globalThis, apply = Reflect.apply, define = Object.defineProperty, create = Object.create;\n"
"  var own = Object.getOwnPropertyDescriptor, keys = Object.keys;\n"
"  var hasOwn = Object.prototype.hasOwnProperty;\n"
"  var ArrayCtor = Array, MapCtor = Map, SetCtor = Set, ObjectCtor = Object;\n"
"  var StringCtor = String, TypeErrorCtor = TypeError, DOMError = G.DOMException;\n"
"  var mapHas = Map.prototype.has, mapGet = Map.prototype.get, mapSet = Map.prototype.set;\n"
"  var mapEach = Map.prototype.forEach, setEach = Set.prototype.forEach;\n"
"  var setAdd = Set.prototype.add;\n"
"  var types = create(null), hooks = [];\n"
"  var objectKind = brand({}), arrayKind = brand([]);\n"
"  var bufferKind = brand(new ArrayBuffer(0)), errorKind = brand(new Error());\n"
"  var dateTime = Date.prototype.getTime;\n"
"  var regexpSource = own(RegExp.prototype, 'source').get;\n"
"  var flagNames = ['hasIndices', 'global', 'ignoreCase', 'multiline', 'dotAll', 'unicode', 'unicodeSets', 'sticky'];\n"
"  var flagChars = ['d', 'g', 'i', 'm', 's', 'u', 'v', 'y'], flagGetters = [];\n"
"  var errors = create(null);\n"
"  function put(o, key, value, enumerable) {\n"
/* ToPropertyDescriptor also reads inherited `get`/`set`. A descriptor literal
 * would break this internal operation after Object.prototype pollution; its
 * own value field does not shield it from an inherited accessor field. */
"    var descriptor = create(null);\n"
"    descriptor.value = value; descriptor.writable = true;\n"
"    descriptor.enumerable = enumerable; descriptor.configurable = true;\n"
"    define(o, key, descriptor);\n"
"  }\n"
"  function append(list, value) { put(list, list.length, value, true); }\n"
"  function get(map, key) { return apply(mapGet, map, [key]); }\n"
"  function has(map, key) { return apply(mapHas, map, [key]); }\n"
"  function remember(map, key, value) { apply(mapSet, map, [key, value]); return value; }\n"
"  function fail() { throw new DOMError('The value cannot be cloned.', 'DataCloneError'); }\n"
"  function register(sample, kind, ctor, unbox) {\n"
"    put(types, brand(sample), { kind: kind, ctor: ctor, unbox: unbox }, true);\n"
"  }\n"
"  register(new Date(0), 'date', Date);\n"
"  register(new RegExp(''), 'regexp', RegExp);\n"
"  register(new MapCtor(), 'map', MapCtor);\n"
"  register(new SetCtor(), 'set', SetCtor);\n"
"  register(new Boolean(false), 'boxed', null, Boolean.prototype.valueOf);\n"
"  register(new Number(0), 'boxed', null, Number.prototype.valueOf);\n"
"  register(new StringCtor(''), 'boxed', null, String.prototype.valueOf);\n"
"  if (typeof BigInt === 'function')\n"
"    register(ObjectCtor(BigInt(0)), 'boxed', null, BigInt.prototype.valueOf);\n"
"  var viewNames = ['Uint8ClampedArray', 'Int8Array', 'Uint8Array', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array', 'Float32Array', 'Float64Array'];\n"
"  for (var i = 0; i < viewNames.length; i++) {\n"
"    var View = G[viewNames[i]];\n"
"    if (typeof View === 'function') register(new View(0), 'view', View);\n"
"  }\n"
"  register(new DataView(new ArrayBuffer(0)), 'dataview', DataView);\n"
"  for (i = 0; i < flagNames.length; i++) {\n"
"    var flag = own(RegExp.prototype, flagNames[i]);\n"
"    append(flagGetters, flag ? flag.get : null);\n"
"  }\n"
"  var errorNames = ['Error', 'EvalError', 'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError'];\n"
"  for (i = 0; i < errorNames.length; i++) put(errors, errorNames[i], G[errorNames[i]], true);\n"
"  if (suppliedHooks !== undefined && suppliedHooks !== null) {\n"
"    for (i = 0; i < suppliedHooks.length; i++) {\n"
"      var hook = suppliedHooks[i];\n"
"      if (!hook || typeof hook[0] !== 'function') throw new TypeErrorCtor('Invalid private clone brand');\n"
"      append(hooks, [hook[0], hook[1]]);\n"
"    }\n"
"  }\n"
/* WebIDL sequence<object> reads the caller's iterator by design. This is
 * argument conversion, not a type test of the data being cloned. It finishes
 * before any transfer validation or graph getter, and IteratorClose runs on a
 * failing element conversion through the language's for-of machinery. */
"  function transferSequence(options) {\n"
"    if (options === undefined || options === null) return [];\n"
"    if (typeof options !== 'object' && typeof options !== 'function')\n"
"      throw new TypeErrorCtor('structuredClone options must be an object');\n"
"    var supplied = options.transfer;\n"
"    if (supplied === undefined) return [];\n"
"    if (supplied === null || (typeof supplied !== 'object' && typeof supplied !== 'function'))\n"
"      throw new TypeErrorCtor('transfer must be an iterable object');\n"
"    var list = [];\n"
"    for (var item of supplied) {\n"
"      if (item === null || (typeof item !== 'object' && typeof item !== 'function'))\n"
"        throw new TypeErrorCtor('transfer elements must be objects');\n"
"      append(list, item);\n"
"    }\n"
"    return list;\n"
"  }\n"
"  function structuredClone(value, options) {\n"
"    if (arguments.length === 0) throw new TypeErrorCtor('structuredClone requires a value');\n"
"    var transfers = transferSequence(options), seen = new MapCtor(), targets = [];\n"
"    var transferSet = new MapCtor();\n"
"    for (var t = 0; t < transfers.length; t++) {\n"
"      var source = transfers[t];\n"
"      if (brand(source) !== bufferKind || has(transferSet, source)) fail();\n"
"      remember(transferSet, source, true);\n"
"    }\n"
/* Allocate private receiving buffers before data traversal. The native final
 * commit copies the THEN-current bytes, after author getters have finished:
 * copying at first encounter would miss a later getter's write to a transferred
 * buffer. Every view resolves through `seen` to this single receiving buffer. */
"    for (t = 0; t < transfers.length; t++) {\n"
"      var target = copyBuffer(transfers[t], true);\n"
"      append(targets, target);\n"
"      remember(seen, transfers[t], target);\n"
"    }\n"
"    function copyProperties(input, output) {\n"
"      var names = keys(input);\n"
"      for (var p = 0; p < names.length; p++) {\n"
"        var key = names[p];\n"
"        if (apply(hasOwn, input, [key])) put(output, key, clone(input[key]), true);\n"
"      }\n"
"      return output;\n"
"    }\n"
"    function clone(input) {\n"
"      var primitive = typeof input;\n"
"      if (primitive === 'symbol' || primitive === 'function') fail();\n"
"      if (input === null) return input;\n"
"      if (primitive !== 'object') {\n"
"        if (primitive === 'undefined' || primitive === 'boolean' || primitive === 'number' || primitive === 'bigint' || primitive === 'string') return input;\n"
"        fail();\n"
"      }\n"
"      if (has(seen, input)) return get(seen, input);\n"
"      var kind = brand(input), type = types[kind], output;\n"
/* Host brands precede the Object fallback. A registered hook must only read
 * private instance state; constructor names, instanceof and toStringTag are
 * all author-controlled and would mistake forged prototypes for real brands. */
"      for (var h = 0; h < hooks.length; h++) {\n"
"        if (apply(hooks[h][0], undefined, [input])) {\n"
"          if (typeof hooks[h][1] !== 'function') fail();\n"
"          output = apply(hooks[h][1], undefined, [input, clone, function (result) {\n"
"            return remember(seen, input, result);\n"
"          }]);\n"
"          return remember(seen, input, output);\n"
"        }\n"
"      }\n"
"      if (kind === bufferKind) return remember(seen, input, copyBuffer(input));\n"
"      if (kind === arrayKind) {\n"
"        output = new ArrayCtor(own(input, 'length').value);\n"
"        remember(seen, input, output);\n"
"        return copyProperties(input, output);\n"
"      }\n"
"      if (kind === errorKind) {\n"
"        var name = input.name;\n"
"        var ErrorCtor = typeof name === 'string' && errors[name] ? errors[name] : errors.Error;\n"
"        var message = own(input, 'message');\n"
"        output = message && apply(hasOwn, message, ['value'])\n"
"          ? new ErrorCtor(toStringValue(message.value)) : new ErrorCtor();\n"
"        remember(seen, input, output);\n"
/* QuickJS stores its captured backtrace as an own stack data property. Preserve
 * that string without executing a replacement stack accessor. HTML explicitly
 * reads name but only a data descriptor for message; cause is accompanying data
 * and likewise copies only its own data value, including self references. */
"        var stack = own(input, 'stack');\n"
"        if (stack && apply(hasOwn, stack, ['value']) && typeof stack.value === 'string')\n"
"          put(output, 'stack', stack.value, false);\n"
"        else delete output.stack;\n"
"        var cause = own(input, 'cause');\n"
"        if (cause && apply(hasOwn, cause, ['value'])) put(output, 'cause', clone(cause.value), false);\n"
"        return output;\n"
"      }\n"
"      if (type) {\n"
"        if (type.kind === 'boxed') output = ObjectCtor(apply(type.unbox, input, []));\n"
"        else if (type.kind === 'date') output = new type.ctor(apply(dateTime, input, []));\n"
"        else if (type.kind === 'regexp') {\n"
"          var flags = '';\n"
"          for (var f = 0; f < flagGetters.length; f++)\n"
"            if (flagGetters[f] && apply(flagGetters[f], input, [])) flags += flagChars[f];\n"
"          output = new type.ctor(apply(regexpSource, input, []), flags);\n"
"        } else if (type.kind === 'view' || type.kind === 'dataview') {\n"
"          var view = readView(input), buffer = clone(view[0]);\n"
"          output = new type.ctor(buffer, view[1], type.kind === 'dataview' ? view[2] : view[2] / view[3]);\n"
"        } else if (type.kind === 'map' || type.kind === 'set') {\n"
"          output = new type.ctor();\n"
"          remember(seen, input, output);\n"
/* HTML snapshots the Map/Set records BEFORE recursive serialization. Calling
 * native forEach while recursively cloning would observe entries appended by
 * a key/value getter, including an unbounded stream of freshly-added entries. */
"          var entries = [];\n"
"          if (type.kind === 'map') {\n"
"            apply(mapEach, input, [function (v, k) { append(entries, [k, v]); }]);\n"
"            for (var e = 0; e < entries.length; e++)\n"
"              apply(mapSet, output, [clone(entries[e][0]), clone(entries[e][1])]);\n"
"          } else {\n"
"            apply(setEach, input, [function (v) { append(entries, v); }]);\n"
"            for (e = 0; e < entries.length; e++) apply(setAdd, output, [clone(entries[e])]);\n"
"          }\n"
"          return output;\n"
"        }\n"
"        return remember(seen, input, output);\n"
"      }\n"
"      if (kind !== objectKind) fail();\n"
"      output = {};\n"
"      remember(seen, input, output);\n"
"      return copyProperties(input, output);\n"
"    }\n"
"    var result = clone(value);\n"
"    commitBuffers(transfers, targets);\n"
"    return result;\n"
"  }\n"
"  define(structuredClone, 'length', { value: 1, configurable: true });\n"
"  return structuredClone;\n"
"})\n";

const size_t js_clone_graph_size = sizeof js_clone_graph - 1;
