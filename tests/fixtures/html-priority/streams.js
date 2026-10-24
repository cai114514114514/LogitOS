/* Public Streams/Encoding/Fetch boundaries added by this feature batch.
 * check(boolean, label) is supplied by the host or guest runner. Deferred
 * callbacks are actual underlying source/sink algorithms; no stream algorithm
 * or constructor is replaced by the apparatus. Microtask bounds identify a
 * promise that cannot progress, and never count elapsed host time as evidence. */
async function testHTMLStreams(check) {
  function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
  }
  function watch(promise) {
    const result = { state: 'pending', value: undefined };
    Promise.resolve(promise).then(
      value => { result.state = 'fulfilled'; result.value = value; },
      reason => { result.state = 'rejected'; result.value = reason; }
    );
    return result;
  }
  async function ticks(count = 12) { for (let i = 0; i < count; i++) await Promise.resolve(); }
  async function settle(promise) {
    const result = watch(promise);
    for (let i = 0; i < 256 && result.state === 'pending'; i++) await Promise.resolve();
    return result;
  }
  async function run(name, action) {
    try { await action(); }
    catch (reason) { check(false, name + ' unexpected exception: ' + String(reason)); }
  }
  function throws(action, pattern) {
    try { action(); return false; }
    catch (reason) { return reason instanceof TypeError && (!pattern || pattern.test(reason.message)); }
  }
  async function collect(stream) {
    const values = [], reader = stream.getReader();
    try {
      while (true) {
        const entry = await reader.read();
        if (entry.done) return values;
        values.push(entry.value);
      }
    } finally { reader.releaseLock(); }
  }
  function byteString(chunks) {
    const bytes = [];
    for (const chunk of chunks) for (const byte of chunk) bytes.push(byte);
    return bytes.join(',');
  }

  await run('Writable queue', async function () {
    const first = deferred(), second = deferred(), start = deferred(), calls = [];
    const stream = new WritableStream({
      start() { return start.promise; },
      write(chunk) { calls.push('write:' + chunk); return chunk === 'ab' ? first.promise : second.promise; },
      close() { calls.push('close'); }
    }, { highWaterMark: 3, size(chunk) { return chunk.length; } });
    const writer = stream.getWriter(), initialReady = writer.ready;
    const one = writer.write('ab'), two = writer.write('cd');
    check(writer.desiredSize === -1, 'Writable desiredSize counts queued and in-flight strategy sizes');
    const ready = watch(writer.ready);
    await ticks();
    check(calls.length === 0, 'Writable writes wait for asynchronous start');
    check(ready.state === 'pending' && writer.ready !== initialReady,
      'Writable ready remains pending while queue reaches highWaterMark');
    start.resolve();
    await ticks();
    check(calls.join(',') === 'write:ab' && watch(two).state === 'pending',
      'Writable sink writes are serialized across an unresolved write');
    first.resolve();
    await settle(one);
    await ticks();
    check(calls.join(',') === 'write:ab,write:cd' && writer.desiredSize === 1,
      'Writable completion removes only the completed chunk size');
    check((await settle(writer.ready)).state === 'fulfilled', 'Writable ready resolves when queue drops below highWaterMark');
    const closing = writer.close(), closed = watch(writer.closed);
    await ticks();
    check(calls.length === 2 && closed.state === 'pending', 'Writable close waits for preceding write and keeps closed pending');
    second.resolve();
    check((await settle(Promise.all([two, closing, writer.closed]))).state === 'fulfilled' &&
      calls.join(',') === 'write:ab,write:cd,close' && writer.desiredSize === 0,
      'Writable close runs once after writes and settles closed');
    writer.releaseLock();
    check(!stream.locked && (await settle(writer.ready)).value instanceof TypeError,
      'Released Writable writer rejects its current ready promise');

    const zero = new WritableStream({}, { highWaterMark: 0 }).getWriter();
    const zeroReady = watch(zero.ready);
    await ticks();
    check(zeroReady.state === 'pending' && zero.desiredSize === 0, 'Writable zero highWaterMark begins with backpressure');
    await settle(zero.close());
    check((await settle(zero.ready)).state === 'fulfilled', 'Writable close releases ready even at zero highWaterMark');
    zero.releaseLock();
  });

  await run('Writable abort/error/release', async function () {
    const pending = deferred(), reason = { abort: true }, calls = [];
    let controller;
    const stream = new WritableStream({
      start(value) { controller = value; },
      write(chunk) { calls.push('write:' + chunk); return pending.promise; },
      abort(value) { calls.push('abort'); check(value === reason, 'Writable sink abort preserves the original reason'); }
    });
    const writer = stream.getWriter(), one = writer.write(1), two = watch(writer.write(2));
    await ticks();
    const aborted = watch(writer.abort(reason));
    await ticks();
    check(controller.signal.aborted && controller.signal.reason === reason && writer.desiredSize === null,
      'Writable abort synchronously updates controller signal and erroring desiredSize');
    check(aborted.state === 'pending' && calls.join(',') === 'write:1',
      'Writable abort waits for the in-flight sink operation');
    pending.resolve();
    await settle(one);
    await ticks();
    check(aborted.state === 'fulfilled' && calls.join(',') === 'write:1,abort' && two.state === 'rejected' && two.value === reason,
      'Writable abort rejects queued writes without invoking their sink callbacks');
    check((await settle(writer.closed)).value === reason && (await settle(writer.ready)).value === reason,
      'Writable closed and ready reject with the stream error');
    writer.releaseLock();

    const gate = deferred(), written = [];
    const released = new WritableStream({ write(value) { written.push(value); return gate.promise; } });
    const old = released.getWriter(), retained = old.write('retained');
    await ticks();
    old.releaseLock();
    const newer = released.getWriter(), later = newer.write('later');
    gate.resolve();
    check((await settle(Promise.all([retained, later]))).state === 'fulfilled' && written.join(',') === 'retained,later',
      'Writable releaseLock preserves queued writes for a subsequent writer');
    await settle(newer.close()); newer.releaseLock();

    const error = { size: true };
    const invalid = new WritableStream({}, { size() { throw error; } }).getWriter();
    check((await settle(invalid.write(1))).value === error && invalid.desiredSize === null,
      'Writable strategy failure errors the stream with the original exception');
    invalid.releaseLock();

    const sinkWrite = function () { return 7; };
    sinkWrite.apply = function () { throw new Error('author apply property'); };
    const callWriter = new WritableStream({ write: sinkWrite }).getWriter();
    check((await settle(callWriter.write('data'))).state === 'fulfilled',
      'Writable invokes callback internal Call rather than an author apply property');
    await settle(callWriter.close()); callWriter.releaseLock();
    let startCalled = false, startStream;
    const sinkStart = function () { startCalled = true; };
    sinkStart.call = function () { throw new Error('author call property'); };
    try { startStream = new WritableStream({ start: sinkStart }); } catch (ignored) {}
    check(startCalled && !!startStream, 'Writable start invokes callback without its author call property');
    if (startStream) await settle(startStream.close());
  });

  await run('Readable lifetime', async function () {
    const cancelled = deferred(), reason = { cancel: true };
    let controller, received;
    const stream = new ReadableStream({
      start(value) { controller = value; },
      cancel(value) { received = value; return cancelled.promise; }
    }, { highWaterMark: 2, size(chunk) { return chunk.size; } });
    check(controller.desiredSize === 2, 'Readable controller starts with strategy highWaterMark');
    controller.enqueue({ size: 3 });
    check(controller.desiredSize === -1, 'Readable controller desiredSize reflects custom queue sizes');
    const reader = stream.getReader();
    await settle(reader.read());
    const read = watch(reader.read()), closed = watch(reader.closed), cancel = watch(reader.cancel(reason));
    await ticks();
    check(received === reason && read.state === 'fulfilled' && read.value.done && closed.state === 'fulfilled' && cancel.state === 'pending',
      'Readable cancel closes reads immediately but waits for source cancellation');
    cancelled.resolve();
    await ticks();
    check(cancel.state === 'fulfilled' && controller.desiredSize === 0,
      'Readable cancellation promise resolves after the source promise');
    reader.releaseLock();
    check(!stream.locked, 'Readable cancel retains its lock until reader release');

    let releaseController;
    const released = new ReadableStream({ start(value) { releaseController = value; } });
    const old = released.getReader(), pendingRead = watch(old.read()), oldClosed = watch(old.closed);
    old.releaseLock();
    await ticks();
    check(!released.locked && pendingRead.state === 'rejected' && pendingRead.value instanceof TypeError &&
      oldClosed.state === 'rejected' && oldClosed.value instanceof TypeError,
      'Readable releaseLock rejects pending read and closed promises');
    releaseController.enqueue('next'); releaseController.close();
    const next = released.getReader();
    check((await settle(next.read())).value.value === 'next' && (await settle(next.read())).value.done,
      'Readable releaseLock leaves the source consumable by another reader');
    next.releaseLock();

    let optionStream = new ReadableStream();
    const optionError = { option: true };
    let originalError = false;
    try { optionStream.values({ get preventCancel() { throw optionError; } }); }
    catch (value) { originalError = value === optionError; }
    check(originalError && !optionStream.locked, 'Readable iterator options are converted before acquiring a reader');
    const primitiveOptions = new ReadableStream();
    check(throws(() => primitiveOptions.values(1)) && !primitiveOptions.locked,
      'Readable iterator rejects primitive options without acquiring a lock');
    check(throws(() => new ReadableStream({ type: 'bytes' }), /not implemented/) &&
      throws(() => new ReadableStream().getReader({ mode: 'byob' }), /BYOB/),
      'Readable byte sources and BYOB are explicitly refused');
    let startCalled = false, startStream;
    const sourceStart = function (value) { startCalled = true; value.close(); };
    sourceStart.call = function () { throw new Error('author call property'); };
    try { startStream = new ReadableStream({ start: sourceStart }); } catch (ignored) {}
    check(startCalled && !!startStream, 'Readable start invokes callback without its author call property');
  });

  await run('Readable serialized pull/from/iterator', async function () {
    const gate = deferred(), start = deferred();
    let pulls = 0, active = 0, maximum = 0;
    const stream = new ReadableStream({
      start() { return start.promise; },
      pull(controller) {
        pulls++; active++; maximum = Math.max(maximum, active);
        controller.enqueue(pulls);
        if (pulls === 1) return gate.promise.then(() => { active--; });
        active--; controller.close();
      }
    }, { highWaterMark: 0 });
    const reader = stream.getReader(), first = reader.read(), second = reader.read();
    await ticks();
    check(pulls === 0, 'Readable pulls wait for asynchronous start');
    start.resolve(); await ticks();
    check(pulls === 1 && maximum === 1, 'Readable does not overlap asynchronous pull callbacks');
    gate.resolve();
    const results = await settle(Promise.all([first, second]));
    check(results.state === 'fulfilled' && results.value[0].value === 1 && results.value[1].value === 2 && maximum === 1,
      'Readable pending demand resumes one pull after the previous pull settles');
    reader.releaseLock();

    const from = await settle(collect(ReadableStream.from([Promise.resolve('a'), 'b'])));
    check(from.state === 'fulfilled' && from.value.join(',') === 'a,b', 'ReadableStream.from unwraps values from sync iterables');
    let steps = 0, returned;
    const iterable = { [Symbol.asyncIterator]() { return {
      next() { steps++; return Promise.resolve({ value: 'async', done: false }); },
      return(value) { returned = value; return Promise.resolve({ done: true }); }
    }; } };
    const fromAsync = ReadableStream.from(iterable);
    await ticks();
    check(steps === 0, 'ReadableStream.from does not prefetch an async iterable at zero highWaterMark');
    const iterator = fromAsync.values();
    const entry = await settle(iterator.next());
    const returnedEntry = await settle(iterator.return('stop'));
    check(entry.value.value === 'async' && returned === 'stop' && returnedEntry.value.done && !fromAsync.locked,
      'Readable async iterator return calls source return and releases its lock');
    const retained = ReadableStream.from([1, 2]), retainedIterator = retained.values({ preventCancel: true });
    await settle(retainedIterator.next()); await settle(retainedIterator.return());
    const remainder = await settle(collect(retained));
    check(remainder.state === 'fulfilled' && remainder.value.join(',') === '2' && !retained.locked,
      'Readable values preventCancel preserves remaining source chunks');
    const loop = [];
    for await (const value of ReadableStream.from([3, 4])) loop.push(value);
    check(loop.join(',') === '3,4', 'Readable Symbol.asyncIterator drives for-await iteration');
    const iteration = function () { return ['native-call'][Symbol.iterator](); };
    iteration.call = function () { throw new Error('author iterator call property'); };
    let called;
    try { called = ReadableStream.from({ [Symbol.iterator]: iteration }); } catch (ignored) {}
    check(!!called && (await settle(collect(called))).value.join(',') === 'native-call',
      'ReadableStream.from invokes iterator method without its author call property');
  });

  await run('pipe lifecycle/policies', async function () {
    let sourceController, destinationController, closeCalls = 0;
    const source = new ReadableStream({ start(value) { sourceController = value; } });
    sourceController.close();
    const destination = new WritableStream({ start(value) { destinationController = value; }, close() { closeCalls++; } }, { highWaterMark: 0 });
    const zeroPipe = source.pipeTo(destination), zeroResult = await settle(zeroPipe);
    check(zeroResult.state === 'fulfilled' && closeCalls === 1 && !source.locked && !destination.locked,
      'pipeTo closes an already-closed source even when destination ready is pending');
    if (zeroResult.state === 'pending') {
      // Dispose the deliberately observed deadlock through a real endpoint
      // failure, so later cases can run without substituting a pipe algorithm.
      destinationController.error(new Error('observed pipe cleanup'));
      await settle(zeroPipe);
    }
    const tail = [];
    const queuedClosed = new ReadableStream({ start(value) { value.enqueue('last'); value.close(); } });
    check((await settle(queuedClosed.pipeTo(new WritableStream({ write(value) { tail.push(value); }, close() { tail.push('close'); } })))).state === 'fulfilled' &&
      tail.join(',') === 'last,close', 'pipeTo writes the last queued chunk before reacting to reader closed');
    const storedError = { alreadyErrored: true };
    const erroredDestination = new WritableStream({ start(value) { value.error(storedError); } });
    const closedSource = new ReadableStream({ start(value) { value.close(); } });
    await ticks();
    check((await settle(closedSource.pipeTo(erroredDestination, { preventClose: true }))).value === storedError,
      'pipeTo propagates destination error before a closed source with preventClose');

    let keptClose = 0;
    const keptDestination = new WritableStream({ close() { keptClose++; } });
    const keptSource = ReadableStream.from([1]);
    check((await settle(keptSource.pipeTo(keptDestination, { preventClose: true }))).state === 'fulfilled' &&
      keptClose === 0 && !keptSource.locked && !keptDestination.locked,
      'pipeTo preventClose leaves destination open and releases both locks');
    await settle(keptDestination.close());

    const sourceReason = { source: true };
    let sourceAbort;
    const badSource = new ReadableStream({ start(value) { value.error(sourceReason); } });
    const abortDestination = new WritableStream({ abort(reason) { sourceAbort = reason; } });
    check((await settle(badSource.pipeTo(abortDestination))).value === sourceReason && sourceAbort === sourceReason &&
      !badSource.locked && !abortDestination.locked, 'pipeTo forwards source error to destination abort and unlocks');
    let avoidedAbort = 0;
    const openDestination = new WritableStream({ abort() { avoidedAbort++; } });
    check((await settle(new ReadableStream({ start(value) { value.error(sourceReason); } }).pipeTo(openDestination,
      { preventAbort: true }))).value === sourceReason && avoidedAbort === 0 && !openDestination.locked,
      'pipeTo preventAbort preserves destination on source failure');
    await settle(openDestination.close());

    const destinationReason = { sink: true };
    let cancelled;
    const incoming = new ReadableStream({ start(value) { value.enqueue(1); value.enqueue(2); }, cancel(reason) { cancelled = reason; } });
    const failing = new WritableStream({ write() { throw destinationReason; } });
    check((await settle(incoming.pipeTo(failing))).value === destinationReason && cancelled === destinationReason &&
      !incoming.locked && !failing.locked, 'pipeTo forwards destination failure to source cancellation and unlocks');
    let avoidedCancel = 0;
    const preserved = new ReadableStream({ start(value) { value.enqueue(1); value.enqueue(2); }, cancel() { avoidedCancel++; } });
    check((await settle(preserved.pipeTo(new WritableStream({ write() { throw destinationReason; } }),
      { preventCancel: true }))).value === destinationReason && avoidedCancel === 0 && !preserved.locked,
      'pipeTo preventCancel preserves the source on destination failure');
    await settle(preserved.cancel());
    const lockedPair = new TransformStream(), held = lockedPair.readable.getReader();
    const throughSource = ReadableStream.from([1]);
    let refused = false;
    try { throughSource.pipeThrough(lockedPair); } catch (reason) { refused = reason instanceof TypeError; }
    check(refused && !throughSource.locked && !lockedPair.writable.locked,
      'pipeThrough refuses a locked readable pair before acquiring endpoint locks');
    await settle(held.cancel('cleanup')); held.releaseLock();
  });

  await run('pipe AbortSignal and tee', async function () {
    const reason = { signal: true }, gate = deferred(), signal = new AbortController();
    let cancelled, aborted, writes = 0;
    const source = new ReadableStream({ start(value) { value.enqueue(1); value.enqueue(2); }, cancel(value) { cancelled = value; } });
    const destination = new WritableStream({ write() { writes++; return gate.promise; }, abort(value) { aborted = value; } });
    const pipe = watch(source.pipeTo(destination, { signal: signal.signal }));
    await ticks(); signal.abort(reason); await ticks();
    check(writes === 1 && pipe.state === 'pending' && source.locked && destination.locked,
      'pipeTo signal shutdown retains locks until the in-flight write settles');
    check(cancelled === undefined && aborted === undefined,
      'pipeTo shutdown waits for delivered writes before running cancel and abort actions');
    gate.resolve(); await ticks(40);
    check(pipe.state === 'rejected' && pipe.value === reason && cancelled === reason && aborted === reason &&
      !source.locked && !destination.locked, 'pipeTo signal abort forwards one reason and releases both locks');
    const pre = AbortSignal.abort(reason);
    let skippedWrites = 0, skippedCancel = 0, skippedAbort = 0;
    const retainedSource = new ReadableStream({ start(value) { value.enqueue(3); }, cancel() { skippedCancel++; } });
    const retainedDestination = new WritableStream({ write() { skippedWrites++; }, abort() { skippedAbort++; } });
    check((await settle(retainedSource.pipeTo(retainedDestination,
      { signal: pre, preventAbort: true, preventCancel: true }))).value === reason &&
      skippedWrites === 0 && skippedCancel === 0 && skippedAbort === 0 && !retainedSource.locked && !retainedDestination.locked,
      'pipeTo pre-aborted signal honors preventAbort/preventCancel without writing');
    await settle(retainedSource.cancel()); await settle(retainedDestination.close());

    const sourceCancel = deferred();
    let cancelReasons, cancellations = 0;
    const original = new ReadableStream({ cancel(value) { cancellations++; cancelReasons = value; return sourceCancel.promise; } });
    const branches = original.tee(), left = watch(branches[0].cancel('left'));
    await ticks();
    check(left.state === 'pending' && cancellations === 0, 'tee first branch cancellation waits for its peer');
    const right = watch(branches[1].cancel('right'));
    await ticks();
    check(cancellations === 1 && cancelReasons.join(',') === 'left,right' && left.state === 'pending' && right.state === 'pending',
      'tee combines both reasons and waits for one underlying cancellation');
    sourceCancel.resolve(); await ticks();
    check(left.state === 'fulfilled' && right.state === 'fulfilled', 'tee resolves both branch cancellations after the source settles');
  });

  await run('Transform pressure/flush/termination', async function () {
    const calls = [];
    const transform = new TransformStream({
      transform(value, controller) { calls.push(value); controller.enqueue(value * 2); },
      flush(controller) { controller.enqueue(99); }
    }, undefined, { highWaterMark: 1 });
    const writer = transform.writable.getWriter(), one = writer.write(1), two = watch(writer.write(2));
    await ticks(30);
    check(calls.join(',') === '1' && two.state === 'pending', 'Transform readable queue backpressure blocks the next transform');
    const reader = transform.readable.getReader();
    const first = await settle(reader.read());
    await ticks(30);
    check(first.value.value === 2 && calls.join(',') === '1,2' && two.state === 'fulfilled',
      'Transform pull releases write backpressure and preserves chunk order');
    await settle(one);
    check((await settle(writer.close())).state === 'fulfilled', 'Transform flush runs during writable close');
    const remaining = await settle(Promise.all([reader.read(), reader.read(), reader.read(), reader.closed]));
    check(remaining.state === 'fulfilled' && remaining.value[0].value === 4 && remaining.value[1].value === 99 && remaining.value[2].done,
      'Transform flush output remains readable before the readable side closes');
    reader.releaseLock(); writer.releaseLock();

    let controller;
    const terminated = new TransformStream({ start(value) { controller = value; value.enqueue('last'); } });
    const terminalWriter = terminated.writable.getWriter();
    controller.terminate();
    const ended = await settle(collect(terminated.readable));
    check(ended.state === 'fulfilled' && ended.value.join(',') === 'last' &&
      (await settle(terminalWriter.closed)).value instanceof TypeError,
      'Transform terminate preserves queued output and errors writable lifetime');
    terminalWriter.releaseLock();

    const reason = { blocked: true }, blocked = new TransformStream(), blockedWriter = blocked.writable.getWriter();
    const write = watch(blockedWriter.write('blocked'));
    await ticks();
    const abortPromise = blockedWriter.abort(reason), abort = watch(abortPromise);
    await ticks();
    // An already-in-flight transform waiting for demand stays pending on abort
    // (Streams §6.4.3). A reader's demand then releases its backpressure gate;
    // the sink must recheck writable erroring state BEFORE calling transform.
    check(abort.state === 'pending' && write.state === 'pending',
      'Transform abort waits for the backpressured in-flight write');
    const abortReader = blocked.readable.getReader(), abortRead = watch(abortReader.read());
    await settle(abortPromise);
    check(abort.state === 'fulfilled' && write.state === 'rejected' && write.value === reason &&
      abortRead.state === 'rejected' && abortRead.value === reason,
      'Transform abort skips transformation after readable demand releases backpressure');
    abortReader.releaseLock();
    blockedWriter.releaseLock();
    check(throws(() => new TransformStream({ readableType: 'bytes' })) ||
      (() => { try { new TransformStream({ readableType: 'bytes' }); return false; } catch (error) { return error instanceof RangeError; } })(),
      'Transform typed readable/writable extensions are explicitly refused');
    const transformCallback = function (value, valueController) { valueController.enqueue(value); };
    transformCallback.call = function () { throw new Error('author transform call property'); };
    const called = await settle(collect(ReadableStream.from([5]).pipeThrough(new TransformStream({ transform: transformCallback }))));
    check(called.state === 'fulfilled' && called.value.join(',') === '5',
      'Transform invokes callback without its author call property');
  });

  await run('Text stream chunks', async function () {
    const encoder = new TextEncoderStream();
    const encoded = await settle(collect(ReadableStream.from(['\ud83d', '\ude00', '\ud83d']).pipeThrough(encoder)));
    check(encoded.state === 'fulfilled' && byteString(encoded.value) === '240,159,152,128,239,191,189' && encoder.encoding === 'utf-8',
      'TextEncoderStream retains split surrogate pairs and flushes an unmatched high surrogate');
    let hint;
    const stringObject = { [Symbol.toPrimitive](value) { hint = value; return 'A'; } };
    const converted = await settle(collect(ReadableStream.from([stringObject]).pipeThrough(new TextEncoderStream())));
    check(converted.state === 'fulfilled' && byteString(converted.value) === '65' && hint === 'string',
      'TextEncoderStream converts each chunk with string-hint ToString');
    const symbol = await settle(collect(ReadableStream.from([Symbol('text')]).pipeThrough(new TextEncoderStream())));
    check(symbol.state === 'rejected' && symbol.value instanceof TypeError,
      'TextEncoderStream rejects Symbol rather than stringifying it');
    const decoder = new TextDecoderStream('utf-8');
    const decoded = await settle(collect(ReadableStream.from([
      new Uint8Array([240, 159]), new Uint8Array([152]), new Uint8Array([128])
    ]).pipeThrough(decoder)));
    check(decoded.state === 'fulfilled' && decoded.value.join('') === '😀' && decoder.encoding === 'utf-8' && !decoder.fatal,
      'TextDecoderStream retains an incremental multibyte UTF-8 sequence');
    const bom = await settle(collect(ReadableStream.from([
      new Uint8Array([239]), new Uint8Array([187]), new Uint8Array([191, 65])
    ]).pipeThrough(new TextDecoderStream())));
    check(bom.state === 'fulfilled' && bom.value.join('') === 'A', 'TextDecoderStream consumes a BOM split across chunks');
    const incomplete = await settle(collect(ReadableStream.from([new Uint8Array([226, 130])]).pipeThrough(new TextDecoderStream())));
    check(incomplete.state === 'fulfilled' && incomplete.value.join('') === '\ufffd', 'TextDecoderStream flush replaces an incomplete final sequence');
    const fatal = await settle(collect(ReadableStream.from([new Uint8Array([255])]).pipeThrough(new TextDecoderStream('utf-8', { fatal: true }))));
    check(fatal.state === 'rejected' && fatal.value instanceof TypeError, 'TextDecoderStream fatal errors propagate through readable and pipe shutdown');
  });

  await run('Fetch body disturbance', async function () {
    const response = new Response(new Uint8Array([65, 66]));
    const reader = response.body.getReader();
    check(!response.bodyUsed && response.body.locked, 'Fetch reader acquisition locks body without marking it disturbed');
    const reading = reader.read();
    check(response.bodyUsed, 'Fetch direct reader read synchronously marks bodyUsed');
    const entry = await settle(reading);
    reader.releaseLock();
    check(entry.state === 'fulfilled' && byteString([entry.value.value]) === '65,66' && !response.body.locked && response.bodyUsed,
      'Fetch reader release unlocks body without clearing disturbance');
    check((await settle(response.text())).value instanceof TypeError, 'Fetch Body consumer refuses a directly disturbed stream');
    const locked = new Response('locked'), holding = locked.body.getReader();
    check((await settle(locked.text())).value instanceof TypeError && !locked.bodyUsed,
      'Fetch locked body rejects consumption without falsely marking disturbance');
    holding.releaseLock();
    check((await settle(locked.text())).value === 'locked' && !locked.body.locked,
      'Fetch successful body consumption releases its internal reader');
    const pipedBody = new Response('pipe'), gate = new AbortController();
    const destination = new WritableStream({}, { highWaterMark: 0 });
    const pipe = pipedBody.body.pipeTo(destination, { signal: gate.signal });
    check(pipedBody.bodyUsed && pipedBody.body.locked,
      'Fetch pipeTo marks the body disturbed before a backpressured read can start');
    gate.abort('cleanup'); await settle(pipe);
  });
}
