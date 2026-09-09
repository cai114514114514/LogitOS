// `then` must be read ONCE from a thenable and then CALLED. An engine that
// reads it twice runs a framework's getter twice; an engine that caches the
// wrong one calls the wrong function. Count the reads.
var reads = 0;
var th = { get then() { reads++; return function (res) { res('ok'); }; } };
Promise.resolve(th).then(function (v) { print('value', v, 'reads', reads); });
var reads2 = 0;
var th2 = { get then() { reads2++; return function (res) { res('ok2'); }; } };
new Promise(function (r) { r(th2); }).then(function (v) { print('value2', v, 'reads2', reads2); });
