// yield* delegating to another async generator, and to a sync iterable.
async function* inner() { yield 1; yield 2; }
async function* outer() { yield 0; yield* inner(); yield* [3, 4]; yield 5; }
(async function () { for await (const v of outer()) print('v', v); print('done'); })();
print('sync');
