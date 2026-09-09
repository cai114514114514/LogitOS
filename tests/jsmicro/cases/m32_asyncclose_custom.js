// The same property against a hand-written async iterator, so the finding does
// not depend on the async-generator machinery.
var it = {
  [Symbol.asyncIterator]() { return this; },
  next() { return Promise.resolve({ value: 1, done: false }); },
  return() {
    print('return() called');
    return Promise.resolve().then(function () { print('  return() settled'); })
                            .then(function () { return { done: true }; });
  }
};
(async function () { for await (const v of it) break; print('AFTER-LOOP'); })();
