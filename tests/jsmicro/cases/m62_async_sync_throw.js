// An async function that throws BEFORE its first await must still reject
// rather than throw synchronously at the call site.
async function f() { throw new Error('early'); }
var caught = 'no';
try { f().catch(function (e) { print('rejected', e.message); }); } catch (e) { caught = 'yes'; }
print('threw synchronously?', caught);
async function g() { print('g body'); await 0; }
print('call returns', Object.prototype.toString.call(g()));
