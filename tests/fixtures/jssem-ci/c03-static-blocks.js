// Static blocks and static initialisation order. TypeScript's `useDefineFor
// ClassFields` output and every decorator polyfill emit these.
'use strict';

class A {
  static a = print('static field a') || 1;
  static { print('static block 1, a=' + this.a); this.b = 2; }
  static c = print('static field c, b=' + this.b) || 3;
  static { print('static block 2, c=' + A.c); }
}
print('A ' + A.a + ' ' + A.b + ' ' + A.c);

// `this` in a static block is the class; a static block can reach privates
class B {
  static #p = 'priv';
  static seen;
  static { B.seen = B.#p; print('block this===B ' + (this === B)); }
}
print('B.seen ' + B.seen);

// static blocks in a derived class see the base's statics through the proto
class Base { static v = 'base'; }
class Sub extends Base {
  static { print('sub sees ' + this.v + ' super=' + super.v); }
}
void Sub;

// class expression name binding is const inside the body
const Named = class Inner {
  static who() { return Inner.name; }
};
print('inner name ' + Named.who() + ' ' + Named.name);
