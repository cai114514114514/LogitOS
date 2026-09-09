print('withResolvers', typeof Promise.withResolvers);
if (typeof Promise.withResolvers === 'function') {
  var o = Promise.withResolvers();
  print('shape', typeof o.promise, typeof o.resolve, typeof o.reject);
  o.promise.then(function (v) { print('got', v); });
  o.resolve('R');
}
