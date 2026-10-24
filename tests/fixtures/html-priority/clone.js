/* HTML §2.7 public behavior. The runner supplies check(boolean, label), awaits
 * this function, and counts its real assertions. No internal clone helpers or
 * engine class numbers are part of the oracle. JS_CLONE_KEEP_BUFFER disables
 * only native detachment; the transfer source/view checks below must fail. */
async function testHTMLClone(check) {
  const NativeDOMException = DOMException;
  const clone = structuredClone;
  const blobBuffer = Blob.prototype.arrayBuffer;
  const apply = Reflect.apply;
  const hasOwn = Object.prototype.hasOwnProperty;

  function owns(value, key) { return apply(hasOwn, value, [key]); }

  function refuses(value, options, name = 'DataCloneError') {
    try {
      clone(value, options);
      return false;
    } catch (error) {
      return error.name === name &&
        (name !== 'DataCloneError' || error instanceof NativeDOMException);
    }
  }

  function poison(value, name) {
    Object.defineProperty(value, name, {
      get() { throw new Error('author property consulted: ' + String(name)); },
      configurable: true
    });
  }

  function bytesEqual(bytes, expected) {
    if (bytes.length !== expected.length) return false;
    for (let i = 0; i < bytes.length; i++) {
      if (bytes[i] !== expected[i]) return false;
    }
    return true;
  }

  const child = { answer: 42 };
  const graph = { first: child, second: child };
  graph.self = graph;
  child.parent = graph;
  const graphCopy = clone(graph);
  check(graphCopy !== graph && graphCopy.first !== child, 'clone creates independent objects');
  check(graphCopy.first === graphCopy.second, 'clone preserves aliases');
  check(graphCopy.self === graphCopy && graphCopy.first.parent === graphCopy,
    'clone preserves graph cycles');
  check(Object.is(clone(-0), -0) && Number.isNaN(clone(NaN)), 'clone preserves numeric primitives');
  check(clone(undefined) === undefined && clone(1n) === 1n, 'clone preserves undefined and BigInt');
  check(refuses(Symbol('data')) && refuses({ operation() {} }), 'clone rejects symbols and functions');

  const array = new Array(4);
  let inheritedReads = 0;
  const arrayPrototype = Object.create(Array.prototype);
  Object.defineProperty(arrayPrototype, '0', {
    get() { inheritedReads++; return 'inherited'; }, configurable: true
  });
  Object.setPrototypeOf(array, arrayPrototype);
  array[2] = child;
  array.note = child;
  Object.defineProperty(array, '__proto__', { value: child, enumerable: true, configurable: true });
  array[Symbol('ignored')] = () => {};
  const arrayCopy = clone(array);
  check(Array.isArray(arrayCopy) && arrayCopy.length === 4 && !(0 in arrayCopy) && !(1 in arrayCopy),
    'array clone preserves holes and length');
  check(inheritedReads === 0, 'array clone ignores inherited numeric getters');
  check(arrayCopy[2] === arrayCopy.note && arrayCopy.__proto__ === arrayCopy.note,
    'array clone copies own string expandos and aliases');
  check(Object.getPrototypeOf(arrayCopy) === Array.prototype && Object.getOwnPropertySymbols(arrayCopy).length === 0,
    'array clone uses target intrinsic prototype and ignores symbol keys');

  let getterReads = 0;
  const changing = Object.create({ removed: 'inherited fallback' });
  Object.defineProperty(changing, 'first', {
    get() { getterReads++; delete changing.removed; return { value: 7 }; }, enumerable: true
  });
  changing.removed = 'own before first getter';
  const changingCopy = clone(changing);
  check(getterReads === 1 && changingCopy.first.value === 7 && !owns(changingCopy, 'removed'),
    'object clone evaluates own getters once and rechecks removed keys');
  const descriptor = Object.getOwnPropertyDescriptor(changingCopy, 'first');
  check(descriptor.writable && descriptor.enumerable && descriptor.configurable && !descriptor.get,
    'object clone converts accessor results to ordinary data properties');

  const originalGet = Object.getOwnPropertyDescriptor(Object.prototype, 'get');
  let pollutedCopy;
  try {
    Object.defineProperty(Object.prototype, 'get', { value() {}, configurable: true });
    pollutedCopy = clone({ item: 5 });
  } finally {
    delete Object.prototype.get;
    if (originalGet) Object.defineProperty(Object.prototype, 'get', originalGet);
  }
  check(pollutedCopy.item === 5, 'clone internal descriptors survive Object.prototype pollution');

  const mapKey = { key: 1 };
  const map = new Map([[mapKey, child], ['self', null]]);
  map.set('self', map);
  poison(map, 'constructor');
  poison(map, 'forEach');
  poison(map, Symbol.iterator);
  poison(map, Symbol.toStringTag);
  const mapCopy = clone({ map, key: mapKey, child });
  check(mapCopy.map instanceof Map && mapCopy.map.get(mapCopy.key) === mapCopy.child,
    'Map clone preserves key/value identity without author methods');
  check(mapCopy.map.get('self') === mapCopy.map, 'Map clone preserves self cycles');
  const mutableMap = new Map();
  const mutatingKey = {};
  Object.defineProperty(mutatingKey, 'touch', {
    get() { mutableMap.set('late', 2); return 1; }, enumerable: true
  });
  mutableMap.set(mutatingKey, 'initial');
  const mapSnapshot = clone(mutableMap);
  check(mutableMap.size === 2 && mapSnapshot.size === 1 && !mapSnapshot.has('late'),
    'Map records are snapshotted before recursive getters');

  const set = new Set([child]);
  set.add(set);
  poison(set, 'constructor');
  poison(set, 'forEach');
  poison(set, Symbol.iterator);
  const setCopy = clone({ set, child });
  check(setCopy.set instanceof Set && setCopy.set.has(setCopy.child) && setCopy.set.has(setCopy.set),
    'Set clone preserves values and cycles without author iteration');

  const date = new Date(123456);
  poison(date, 'constructor');
  poison(date, 'getTime');
  const dateCopy = clone(date);
  check(dateCopy instanceof Date && dateCopy.getTime() === 123456 && Number.isNaN(clone(new Date(NaN)).getTime()),
    'Date clone reads native time including invalid dates');
  const regexp = /a+b/dgimsy;
  regexp.lastIndex = 9;
  poison(regexp, 'constructor');
  poison(regexp, 'source');
  poison(regexp, 'flags');
  poison(regexp, 'global');
  const regexpCopy = clone(regexp);
  check(regexpCopy instanceof RegExp && regexpCopy.source === 'a+b' && regexpCopy.flags === 'dgimsy' && regexpCopy.lastIndex === 0,
    'RegExp clone reads native pattern/flags and resets lastIndex');

  const boxes = [new Boolean(false), new Number(-0), new String('text'), Object(123n)];
  for (const box of boxes) {
    poison(box, 'constructor');
    poison(box, 'valueOf');
  }
  const boxCopies = clone(boxes);
  check(boxCopies[0] instanceof Boolean && boxCopies[0].valueOf() === false,
    'clone preserves Boolean wrapper payload');
  check(boxCopies[1] instanceof Number && Object.is(boxCopies[1].valueOf(), -0),
    'clone preserves Number wrapper payload');
  check(boxCopies[2] instanceof String && boxCopies[2].valueOf() === 'text' && boxCopies[3].valueOf() === 123n,
    'clone preserves String and BigInt wrapper payloads');
  check(refuses(Object(Symbol('boxed'))), 'clone rejects Symbol wrappers');

  const error = new RangeError('range');
  poison(error, 'constructor');
  error.stack = 'saved source stack';
  error.cause = { error, shared: child };
  error.extra = () => {};
  const errorCopy = clone({ error, child });
  check(errorCopy.error instanceof RangeError && errorCopy.error.name === 'RangeError' && errorCopy.error.message === 'range',
    'Error clone preserves standard name/message without constructor lookup');
  check(errorCopy.error.stack === 'saved source stack' && errorCopy.error.cause.error === errorCopy.error &&
    errorCopy.error.cause.shared === errorCopy.child && !owns(errorCopy.error, 'extra'),
    'Error clone preserves data stack/cause cycles and ignores expandos');
  const accessorError = new Error('old');
  poison(accessorError, 'message');
  check(clone(accessorError).message === '', 'Error message accessor is not invoked');
  const symbolMessage = new Error();
  Object.defineProperty(symbolMessage, 'message', { value: Symbol('message'), configurable: true });
  check(refuses(symbolMessage, undefined, 'TypeError'), 'Error message uses abstract ToString semantics');

  const backing = new ArrayBuffer(8);
  new Uint8Array(backing).set([1, 2, 3, 4, 5, 6, 7, 8]);
  const firstView = new Uint8Array(backing, 1, 4);
  const secondView = new Uint16Array(backing, 2, 2);
  const dataView = new DataView(backing, 1, 5);
  for (const view of [firstView, secondView, dataView]) {
    for (const property of ['constructor', 'buffer', 'byteOffset', 'byteLength', 'length']) poison(view, property);
  }
  const viewsCopy = clone({ backing, firstView, secondView, dataView });
  check(viewsCopy.backing !== backing && viewsCopy.firstView.buffer === viewsCopy.backing &&
    viewsCopy.secondView.buffer === viewsCopy.backing && viewsCopy.dataView.buffer === viewsCopy.backing,
    'ArrayBuffer views share one cloned backing buffer');
  check(viewsCopy.firstView.byteOffset === 1 && viewsCopy.firstView.length === 4 &&
    viewsCopy.secondView.byteOffset === 2 && viewsCopy.secondView.length === 2 &&
    viewsCopy.dataView.byteOffset === 1 && viewsCopy.dataView.byteLength === 5,
    'ArrayBuffer views preserve native offsets and lengths');
  viewsCopy.firstView[1] = 34;
  check(viewsCopy.dataView.getUint8(1) === 34 && new Uint8Array(backing)[2] === 3,
    'cloned view writes share clone bytes while source bytes stay independent');

  const transferBuffer = new ArrayBuffer(6);
  const oldView = new Uint8Array(transferBuffer);
  oldView.set([9, 8, 7, 6, 5, 4]);
  const transferred = clone({ buffer: transferBuffer, view: oldView }, { transfer: new Set([transferBuffer]) });
  check(bytesEqual(new Uint8Array(transferred.buffer), [9, 8, 7, 6, 5, 4]) && transferred.view.buffer === transferred.buffer,
    'transfer preserves bytes and view backing identity');
  check(transferBuffer.byteLength === 0, 'transfer detaches source ArrayBuffer');
  check(oldView.length === 0, 'transfer invalidates old typed array');
  check(refuses(transferBuffer) && refuses(transferBuffer, { transfer: [transferBuffer] }),
    'detached ArrayBuffer cannot be cloned or transferred again');
  let detachedGetter = false;
  check(refuses({ get read() { detachedGetter = true; return 1; } }, { transfer: [transferBuffer] }) && detachedGetter,
    'detached transfer validation follows graph getter serialization');

  const failedFirst = new ArrayBuffer(3);
  const failedSecond = new ArrayBuffer(4);
  check(refuses({}, { transfer: [failedFirst, failedFirst] }) && failedFirst.byteLength === 3,
    'duplicate transfers fail without detaching any source');
  check(refuses({}, { transfer: [failedFirst, new Uint8Array(failedSecond)] }) &&
    failedFirst.byteLength === 3 && failedSecond.byteLength === 4,
    'unsupported transfer entries fail without partial detachment');
  check(refuses({ unsupported: () => {} }, { transfer: [failedFirst, failedSecond] }) &&
    failedFirst.byteLength === 3 && failedSecond.byteLength === 4,
    'failed graph clone leaves all transferred buffers attached');
  const thrown = { token: 'author exception' };
  let originalException = false;
  try {
    clone({ get failure() { throw thrown; } }, { transfer: [failedFirst, failedSecond] });
  } catch (caught) { originalException = caught === thrown; }
  check(originalException && failedFirst.byteLength === 3 && failedSecond.byteLength === 4,
    'author getter exception propagates without detaching transfers');

  const changedBuffer = new ArrayBuffer(1);
  new Uint8Array(changedBuffer)[0] = 10;
  const changedCopy = clone({
    buffer: changedBuffer,
    get update() { new Uint8Array(changedBuffer)[0] = 97; return true; }
  }, { transfer: [changedBuffer] });
  check(new Uint8Array(changedCopy.buffer)[0] === 97 && changedBuffer.byteLength === 0,
    'transfer captures bytes after graph getters finish');

  const outerFirst = new ArrayBuffer(2);
  const outerSecond = new ArrayBuffer(2);
  let reentrantRefused = false;
  try {
    clone({ get reentrant() { clone(outerSecond, { transfer: [outerSecond] }); return 1; } },
      { transfer: [outerFirst, outerSecond] });
  } catch (caught) { reentrantRefused = caught.name === 'DataCloneError'; }
  check(reentrantRefused && outerFirst.byteLength === 2 && outerSecond.byteLength === 0,
    'commit revalidates all buffers after reentrant detachment');
  const unused = new ArrayBuffer(2);
  check(clone({ result: 1 }, { transfer: [unused] }).result === 1 && unused.byteLength === 0,
    'transfer detaches listed buffer even when absent from the graph');
  const zero = new ArrayBuffer(0);
  check(clone(zero).byteLength === 0, 'attached zero-length ArrayBuffer can be cloned');
  clone(zero, { transfer: [zero] });
  check(refuses(zero), 'zero-length transferred ArrayBuffer is still detached');
  check(refuses(1, { transfer: [1] }, 'TypeError') && refuses(1, { transfer: null }, 'TypeError') &&
    refuses(1, 3, 'TypeError'), 'clone applies WebIDL options and sequence object conversion');

  check(refuses(new Promise(() => {})) && refuses(new WeakMap()) && refuses(new WeakSet()),
    'clone rejects native objects with unsupported internal slots');
  let proxyReads = 0;
  const proxy = new Proxy({}, { ownKeys() { proxyReads++; return []; } });
  check(refuses(proxy) && proxyReads === 0, 'clone rejects Proxy without author traps');
  if (typeof SharedArrayBuffer === 'function')
    check(refuses(new SharedArrayBuffer(2)), 'clone refuses shared memory without supported isolation');
  check(refuses(new URL('https://example.invalid/path')) && refuses(new URLSearchParams('a=1')),
    'clone rejects native nonserializable platform objects');
  check(refuses(new FormData()), 'clone rejects authentic privately branded FormData');
  const forgedForm = Object.create(FormData.prototype);
  forgedForm.note = 3;
  check(clone(forgedForm).note === 3, 'private host brand does not mistake forged FormData prototype');

  const blobInput = new Uint8Array([65, 0, 255, 66]);
  const blob = new Blob([blobInput], { type: 'APPLICATION/OCTET-STREAM' });
  blobInput[0] = 90;
  for (const property of ['constructor', 'arrayBuffer', '_b', 'size', 'type']) poison(blob, property);
  blob.expando = blob;
  const blobCopy = clone({ first: blob, second: blob });
  const clonedBytes = new Uint8Array(await apply(blobBuffer, blobCopy.first, []));
  check(blobCopy.first instanceof Blob && blobCopy.first !== blob && blobCopy.first === blobCopy.second,
    'Blob clone preserves real host brand and aliases');
  check(blobCopy.first.size === 4 && blobCopy.first.type === 'application/octet-stream' &&
    bytesEqual(clonedBytes, [65, 0, 255, 66]) && !owns(blobCopy.first, 'expando'),
    'Blob clone copies immutable private bytes and type without public getters');
  clonedBytes[0] = 12;
  check(bytesEqual(new Uint8Array(await apply(blobBuffer, blobCopy.first, [])), [65, 0, 255, 66]),
    'Blob clone arrayBuffer result cannot mutate private bytes');
  const forgedBlob = Object.create(Blob.prototype);
  forgedBlob.note = 4;
  const forgedBlobCopy = clone(forgedBlob);
  check(forgedBlobCopy.note === 4 && Object.getPrototypeOf(forgedBlobCopy) === Object.prototype,
    'private host brand does not mistake forged Blob prototype');

  const file = new File([new Uint8Array([0, 17, 255])], 'sample.bin', {
    type: 'application/octet-stream', lastModified: 1234
  });
  for (const property of ['constructor', 'name', 'lastModified', 'type', 'size']) poison(file, property);
  const fileCopy = clone({ file, same: file });
  check(fileCopy.file instanceof File && fileCopy.file === fileCopy.same &&
    fileCopy.file.name === 'sample.bin' && fileCopy.file.lastModified === 1234 &&
    fileCopy.file.type === 'application/octet-stream',
    'File clone preserves real host brand, metadata and aliases');
  check(bytesEqual(new Uint8Array(await apply(blobBuffer, fileCopy.file, [])), [0, 17, 255]),
    'File clone preserves exact private bytes');
  check(refuses(blob, { transfer: [blob] }) && refuses(file, { transfer: [file] }),
    'Blob and File are cloned but are not transferable');
}
