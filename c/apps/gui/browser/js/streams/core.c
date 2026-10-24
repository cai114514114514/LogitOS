/* Stream installation and pipes compose the readable, writable and transform
 * modules. The old pipe loop leaked endpoint locks and rejected only one side
 * of a failure; shutdown now owns cancellation, close/abort policy and release.
 *
 * Queue removes use a head index and clear consumed entries. Array.shift moves
 * every remaining chunk on every read; retaining consumed entries to avoid that
 * move would instead hold their buffers alive. Periodic compaction gives an
 * amortized constant-cost dequeue without that hidden memory retention.
 *
 * Original batch note: awaiting final combined verification.
 * Correction 2026-09-30: test-html-streams runs 79 new public boundaries through
 * the real WebAPI/Streams group on the host, all passing also under ASan/UBSan.
 * JS_STREAMS_READY_EARLY changes only ready's pressure producer and makes exactly
 * one of those 79 checks fail. This does not replace the still-required guest
 * proof or the existing Readable/network regression; no speed claim is made. */
#include "js/streams/core.h"
#include "js/streams/readable.h"
#include "js/streams/writable.h"
#include "js/streams/transform.h"

static const char SHARED[] =
"(function () {\n"
"'use strict';\n"
"const PromiseType = Promise;\n"
/* Call(callback, receiver, args) does not read callback.call/apply. Host fixture
 * 2026-09-30 showed a genuine sink write rejected after its author `apply`
 * property was replaced. Capture Reflect.apply once and share that primitive
 * with synchronous start/transform/iterator calls as well as promise callbacks. */
"const call = Reflect.apply;\n"
"const nativeThen = PromiseType.prototype.then;\n"
"const resolvePromise = PromiseType.resolve.bind(PromiseType);\n"
"const rejectPromise = PromiseType.reject.bind(PromiseType);\n"
"function then(promise, fulfilled, rejected) { return call(nativeThen, promise, [fulfilled, rejected]); }\n"
"function handled(promise) { then(promise, undefined, function () {}); return promise; }\n"
"function deferred() {\n"
"  let resolve, reject;\n"
"  const result = { settled: false, promise: new PromiseType(function (yes, no) { resolve = yes; reject = no; }) };\n"
"  result.resolve = function (value) { if (!result.settled) { result.settled = true; resolve(value); } };\n"
"  result.reject = function (error) { if (!result.settled) { result.settled = true; reject(error); } };\n"
"  return result;\n"
"}\n"
"class Queue {\n"
"  constructor() { this.items = []; this.head = 0; }\n"
"  get length() { return this.items.length - this.head; }\n"
"  set length(value) {\n"
"    if (value !== 0) throw new RangeError('An internal queue can only be cleared');\n"
"    this.items.length = 0; this.head = 0;\n"
"  }\n"
"  push(value) { this.items.push(value); }\n"
"  shift() {\n"
"    if (!this.length) return undefined;\n"
"    const value = this.items[this.head];\n"
"    this.items[this.head++] = undefined;\n"
"    if (this.head >= 64 && this.head * 2 >= this.items.length) {\n"
"      this.items = this.items.slice(this.head); this.head = 0;\n"
"    }\n"
"    return value;\n"
"  }\n"
"}\n"
"function method(object, name) {\n"
"  const fn = object[name];\n"
"  if (fn !== undefined && typeof fn !== 'function') throw new TypeError(name + ' must be a function');\n"
"  return fn;\n"
"}\n"
"function invoke(fn, self, args) {\n"
"  try { return resolvePromise(fn ? call(fn, self, args) : undefined); }\n"
"  catch (error) { return rejectPromise(error); }\n"
"}\n"
"function strategy(value, fallback) {\n"
"  value = value === undefined ? {} : value;\n"
"  if (value === null || (typeof value !== 'object' && typeof value !== 'function'))\n"
"    throw new TypeError('A queuing strategy must be an object');\n"
"  const highWaterMark = value.highWaterMark === undefined ? fallback : +value.highWaterMark;\n"
"  if (Number.isNaN(highWaterMark) || highWaterMark < 0) throw new RangeError('Invalid highWaterMark');\n"
"  const size = method(value, 'size');\n"
"  return { highWaterMark: highWaterMark, size: size || function () { return 1; } };\n"
"}\n"
"function chunkSize(size, chunk) {\n"
"  const result = +size(chunk);\n"
"  if (!Number.isFinite(result) || result < 0) throw new RangeError('Chunk size must be finite and nonnegative');\n"
"  return result;\n"
"}\n"
"function operation(object, name, fn, length) {\n"
"  Object.defineProperty(fn, 'name', { value: typeof name === 'symbol' ? '[' + (name.description || '') + ']' : name, configurable: true });\n"
"  if (length !== undefined) Object.defineProperty(fn, 'length', { value: length, configurable: true });\n"
"  Object.defineProperty(object, name, { value: fn, writable: true, configurable: true, enumerable: true });\n"
"}\n"
"function accessor(object, name, get) {\n"
"  Object.defineProperty(get, 'name', { value: 'get ' + name, configurable: true });\n"
"  Object.defineProperty(object, name, { get: get, configurable: true, enumerable: true });\n"
"}\n"
"function publish(name, constructor, length) {\n"
"  Object.defineProperty(constructor, 'length', { value: length, configurable: true });\n"
"  Object.defineProperty(constructor.prototype, Symbol.toStringTag, { value: name, configurable: true });\n"
"  globalThis[name] = constructor;\n"
"}\n"
"return { Queue: Queue, all: PromiseType.all.bind(PromiseType), then: then, resolve: resolvePromise, reject: rejectPromise, handled: handled,\n"
"  deferred: deferred, method: method, invoke: invoke, call: call, strategy: strategy,\n"
"  chunkSize: chunkSize, operation: operation, accessor: accessor, publish: publish };\n"
"})()\n";

static const char PIPE[] =
"(function (shared, readable, writable, abort, transform) {\n"
"'use strict';\n"
"const G = globalThis, then = shared.then, resolve = shared.resolve, reject = shared.reject;\n"
"const operation = shared.operation, accessor = shared.accessor, deferred = shared.deferred;\n"
"function pipeOptions(options) {\n"
"  if (options == null) options = {};\n"
"  if (typeof options !== 'object' && typeof options !== 'function') throw new TypeError('Pipe options must be an object');\n"
"  const result = { preventAbort: !!options.preventAbort, preventCancel: !!options.preventCancel,\n"
"    preventClose: !!options.preventClose, signal: options.signal };\n"
"  if (result.signal !== undefined) abort.check(result.signal);\n"
"  return result;\n"
"}\n"
"function startPipe(source, destination, options) {\n"
"  const input = readable.state(source), output = writable.state(destination);\n"
"  if (input.reader || output.writer) throw new TypeError('A pipe endpoint is locked');\n"
"  const readerState = readable.acquire(source);\n"
"  let writerState;\n"
"  try { writerState = writable.acquire(destination); }\n"
"  catch (reason) { readable.release(readerState); throw reason; }\n"
/* A pipe disturbs the source as soon as both locks have been acquired, even
 * when HWM=0 blocks its first read or a pre-aborted signal prevents all I/O.
 * Deferring this to read() made Response.bodyUsed incorrectly stay false. */
"  input.disturbed = true;\n"
"  const result = deferred();\n"
"  let stopping = false, reading = false, currentWrite = null, unfollow = null, closingByPipe = false;\n"
"  function finish(reason, failed) {\n"
"    if (unfollow) { unfollow(); unfollow = null; }\n"
"    readable.release(readerState); writable.release(writerState);\n"
"    if (failed) result.reject(reason); else result.resolve(undefined);\n"
"  }\n"
"  function finishActions(reason, kind) {\n"
"    const actions = [];\n"
"    try {\n"
"      if (kind === 'close' && !options.preventClose && output.state !== 'closed' && !output.closeQueued) {\n"
"        closingByPipe = true; actions.push(writable.close(output));\n"
"      }\n"
"      if ((kind === 'source' || kind === 'abort') && !options.preventAbort &&\n"
"          output.state !== 'closed' && output.state !== 'errored')\n"
"        actions.push(writable.abort(output, reason));\n"
"      if ((kind === 'destination' || kind === 'abort') && !options.preventCancel &&\n"
"          input.state === 'readable')\n"
"        actions.push(readable.cancel(input, reason));\n"
"    } catch (actionError) { finish(actionError, true); return; }\n"
"    then(shared.all(actions), function () { finish(reason, kind !== 'close'); },\n"
"      function (actionError) { finish(actionError, true); });\n"
"  }\n"
/* The previous Promise.all included currentWrite alongside cancel/abort calls.
 * That retained locks but had ALREADY invoked cancellation before the delivered
 * write settled. §4.9.1 requires waiting before performing those actions while
 * the destination is writable. The host fixture observed source.cancel early.
 * An already-erroring destination does not receive this writable-only wait. */
"  function shutdown(reason, kind) {\n"
"    if (stopping) return; stopping = true;\n"
"    if (unfollow) { unfollow(); unfollow = null; }\n"
"    if (currentWrite && output.state === 'writable' && !output.closeQueued) {\n"
"      const afterWrite = function () { finishActions(reason, kind); };\n"
"      then(currentWrite, afterWrite, afterWrite);\n"
"    } else finishActions(reason, kind);\n"
"  }\n"
/* Closing must bypass a pending writer.ready (HWM=0 has no natural wake-up).
 * The obvious unconditional reader.closed handler is ALSO wrong: consuming the
 * last queued chunk closes the reader BEFORE that read's fulfillment callback.
 * Wait for that callback to deliver its chunk or it is silently dropped. The
 * public fixture checks both cases and endpoint error precedence. */
"  function inspect() {\n"
"    if (input.state === 'errored') shutdown(input.error, 'source');\n"
"    else if (output.state === 'errored') shutdown(output.error, 'destination');\n"
"    else if (input.state === 'closed') shutdown(undefined, 'close');\n"
"    else if (output.state === 'closed' || output.closeQueued)\n"
"      shutdown(new TypeError('Pipe destination is closing or closed'), 'destination');\n"
"    return stopping;\n"
"  }\n"
"  then(readerState.closed.promise, function () { if (!reading) inspect(); }, function (reason) { shutdown(reason, 'source'); });\n"
"  then(writerState.closed.promise, function () {\n"
"    if (!closingByPipe) shutdown(new TypeError('Pipe destination closed before the source'), 'destination');\n"
"  }, function (reason) { shutdown(reason, 'destination'); });\n"
"  if (options.signal !== undefined)\n"
"    unfollow = abort.follow(options.signal, function (reason) { shutdown(reason, 'abort'); });\n"
"  if (!stopping) inspect();\n"
"  function pump() {\n"
"    if (stopping || inspect()) return;\n"
"    then(writerState.ready.promise, function () {\n"
"      if (stopping) return;\n"
"      reading = true;\n"
"      then(readable.read(readerState), function (entry) {\n"
"        reading = false;\n"
"        if (stopping) return;\n"
"        if (entry.done) { shutdown(undefined, 'close'); return; }\n"
"        currentWrite = writable.write(output, entry.value);\n"
"        then(currentWrite, pump, function (reason) { shutdown(reason, 'destination'); });\n"
"      }, function (reason) { reading = false; shutdown(reason, 'source'); });\n"
"    }, function (reason) { shutdown(reason, 'destination'); });\n"
"  }\n"
"  pump(); return result.promise;\n"
"}\n"
"operation(G.ReadableStream.prototype, 'pipeTo', function (destination, options) {\n"
"  try { return startPipe(this, destination, pipeOptions(options)); }\n"
"  catch (reason) { return reject(reason); }\n"
"});\n"
"operation(G.ReadableStream.prototype, 'pipeThrough', function (pair, options) {\n"
"  if (pair == null || (typeof pair !== 'object' && typeof pair !== 'function'))\n"
"    throw new TypeError('A readable/writable pair is required');\n"
"  const destination = pair.writable, output = pair.readable;\n"
/* The returned readable endpoint must be unlocked too. Validating it only as
 * a stream let pipeThrough acquire the source/writable locks and hand back an
 * unreadable locked result; reject before startPipe can acquire either lock. */
"  if (readable.state(output).reader) throw new TypeError('Transform readable is locked');\n"
"  const result = startPipe(this, destination, pipeOptions(options));\n"
"  shared.handled(result);\n"
"  return output;\n"
"});\n"
"const byteStrategies = new WeakMap(), countStrategies = new WeakMap();\n"
"function initialMark(options) {\n"
"  if (options == null || (typeof options !== 'object' && typeof options !== 'function') ||\n"
"      options.highWaterMark === undefined) throw new TypeError('highWaterMark is required');\n"
"  return +options.highWaterMark;\n"
"}\n"
"function markOf(map, value) {\n"
"  if (!map.has(value)) throw new TypeError('Illegal invocation: expected a queuing strategy');\n"
"  return map.get(value);\n"
"}\n"
"function ByteLengthQueuingStrategy(options) {\n"
"  if (!new.target) throw new TypeError(\"ByteLengthQueuingStrategy requires 'new'\");\n"
"  byteStrategies.set(this, initialMark(options));\n"
"}\n"
"function CountQueuingStrategy(options) {\n"
"  if (!new.target) throw new TypeError(\"CountQueuingStrategy requires 'new'\");\n"
"  countStrategies.set(this, initialMark(options));\n"
"}\n"
"const byteSize = function size(chunk) { return chunk.byteLength; };\n"
"const countSize = function size() { return 1; };\n"
"accessor(ByteLengthQueuingStrategy.prototype, 'highWaterMark', function () { return markOf(byteStrategies, this); });\n"
"accessor(ByteLengthQueuingStrategy.prototype, 'size', function () { markOf(byteStrategies, this); return byteSize; });\n"
"accessor(CountQueuingStrategy.prototype, 'highWaterMark', function () { return markOf(countStrategies, this); });\n"
"accessor(CountQueuingStrategy.prototype, 'size', function () { markOf(countStrategies, this); return countSize; });\n"
"shared.publish('ByteLengthQueuingStrategy', ByteLengthQueuingStrategy, 1);\n"
"shared.publish('CountQueuingStrategy', CountQueuingStrategy, 1);\n"
"return {\n"
"  disturbed: function (stream) { return readable.state(stream).disturbed; },\n"
"  is: function (value) { return readable.is(value) || writable.is(value) || transform.is(value) ||\n"
"    byteStrategies.has(value) || countStrategies.has(value); },\n"
"  bytes: function (stream) { return readable.state(stream).bytes; },\n"
"  installText: transform.installText\n"
"};\n"
"})\n";

JSValue js_streams_install(JSContext *ctx, JSValueConst abort)
{
    JSValue shared = JS_Eval(ctx, SHARED, sizeof SHARED - 1, "<streams-common>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(shared)) return shared;
    JSValue readable = js_readable_install(ctx, shared);
    JSValue writable = JS_UNDEFINED, transform = JS_UNDEFINED, result = JS_EXCEPTION;
    if (JS_IsException(readable)) goto done;
    writable = js_writable_install(ctx, shared);
    if (JS_IsException(writable)) goto done;
    transform = js_transform_install(ctx, shared, readable, writable);
    if (JS_IsException(transform)) goto done;
    JSValue fn = JS_Eval(ctx, PIPE, sizeof PIPE - 1, "<streams-pipe>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(fn)) {
        JSValueConst args[5] = { shared, readable, writable, abort, transform };
        result = JS_Call(ctx, fn, JS_UNDEFINED, 5, args);
    }
    JS_FreeValue(ctx, fn);
done:
    JS_FreeValue(ctx, transform);
    JS_FreeValue(ctx, writable);
    JS_FreeValue(ctx, readable);
    JS_FreeValue(ctx, shared);
    return result;
}
