// super in every position a compiler emits it: methods, getters, static
// methods, nested arrows (which must capture the home object), and inside a
// nested class body.
'use strict';

class Base {
  greet() { return 'base-greet'; }
  get val() { return 'base-val'; }
  static sgreet() { return 'base-static'; }
}

class Kid extends Base {
  greet() { return 'kid+' + super.greet(); }
  get val() { return 'kid+' + super.val; }
  static sgreet() { return 'kid+' + super.sgreet(); }

  // super inside a nested arrow: the arrow has no home object of its own, so
  // it must close over the method's. Babel/TS output relies on this.
  arrowSuper() {
    const f = () => () => super.greet();
    return f()();
  }

  // super inside a nested ordinary function is a SyntaxError, so it is not
  // here; super inside a nested arrow inside a callback is the real pattern.
  inCallback() {
    return [1].map(() => super.greet())[0];
  }
}

const k = new Kid();
print('greet ' + k.greet());
print('val ' + k.val);
print('static ' + Kid.sgreet());
print('arrow ' + k.arrowSuper());
print('callback ' + k.inCallback());

// object literal methods have a home object too
const proto = { hi() { return 'proto-hi'; } };
const obj = { __proto__: proto, hi() { return 'obj+' + super.hi(); },
              arrow() { return (() => super.hi())(); } };
print('objsuper ' + obj.hi());
print('objarrow ' + obj.arrow());

// new.target
function NT() { print('new.target ' + (new.target ? new.target.name : 'undefined')); }
NT();
new NT();
class T { constructor() { print('class new.target ' + new.target.name); } }
class T2 extends T {}
new T2();

// `this` is in TDZ before super() in a derived constructor
class Bad extends Base {
  constructor() {
    try { print(this.greet()); } catch (e) { print('tdz ' + e.constructor.name); }
    super();
    print('after super ok ' + this.greet());
  }
}
new Bad();

// calling super() twice throws ReferenceError
class Twice extends Base {
  constructor() { super(); try { super(); } catch (e) { print('twice ' + e.constructor.name); } }
}
new Twice();

// a derived constructor that returns an object overrides `this`
class Ret extends Base { constructor() { super(); return { tag: 'other' }; } }
print('ret ' + new Ret().tag);
