// The resolve/reject pair may fire at most once, and the guard is per-pair.
var res, rej;
var p = new Promise(function (a, b) { res = a; rej = b; });
p.then(function (v) { print('settled with', v); }, function (e) { print('rejected', e); });
res('first');
res('second');
rej('late reject');
print('resolve returns', res('third'));
print('resolve identity', res === res);
