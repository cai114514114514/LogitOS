// If queueMicrotask is a Promise.resolve().then wrapper it costs the SAME one
// turn as a bare .then -- but interleaved with an await it can drift. This is
// the case that reads the prelude.
async function a() { await null; print('await-resume'); }
queueMicrotask(function(){print('qmt');});
a();
Promise.resolve().then(function(){print('then');});
queueMicrotask(function(){print('qmt2');});
print('sync');
