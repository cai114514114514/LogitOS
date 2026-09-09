// A class static block that starts async work: module-scope initialisation in
// every framework compiled to classes.
var log = [];
class C {
  static ready = null;
  static { log.push('static block'); C.ready = Promise.resolve('R').then(function (v) { log.push('resolved ' + v); return v; }); }
}
log.push('after class');
C.ready.then(function () { print(log.join(' | ')); });
print('sync');
