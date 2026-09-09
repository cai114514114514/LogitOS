// for await over a SYNC iterable goes through CreateAsyncFromSyncIterator, a
// different path from m31. Does IT await the close?
function* sgen() { try { yield 1; yield 2; } finally { print('SYNC-CLEANUP'); } }
(async function () { for await (const v of sgen()) break; print('AFTER'); })();
