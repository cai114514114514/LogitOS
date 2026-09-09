// new (async function(){}).constructor(...) -- the AsyncFunction constructor,
// which bundlers emit for regenerator-free async helpers.
var AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
print('AsyncFunction name', AsyncFunction.name);
var f = new AsyncFunction('a', 'await 0; return a * 2;');
f(21).then(function (v) { print('result', v); });
var AsyncGen = Object.getPrototypeOf(async function* () {}).constructor;
print('AsyncGeneratorFunction name', AsyncGen.name);
