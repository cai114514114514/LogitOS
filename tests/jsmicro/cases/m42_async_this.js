// async methods, async arrows, and async class methods all resume with the
// right `this` and in the right order.
var o = {
  name: 'o',
  async m() { await 0; print('method this', this.name); },
  arrow: null
};
o.arrow = async () => { await 0; print('arrow this', o.name); };
class C { constructor() { this.name = 'C'; } async m() { await 0; print('class this', this.name); } }
o.m(); o.arrow(); new C().m();
Promise.resolve().then(function(){print('t1');}).then(function(){print('t2');})
  .then(function(){print('t3');});
print('sync');
