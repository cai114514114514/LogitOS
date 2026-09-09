// Class field initialisation ORDER, which is what every framework's component
// base class depends on: a field initialiser runs in declaration order, after
// super() in a derived class, with `this` already bound.
'use strict';

function tag(s) { print('tag ' + s); return s; }

class Base {
  a = tag('Base.a');
  constructor() { print('Base ctor, a=' + this.a + ' b=' + this.b); }
  b = tag('Base.b');
}

class Derived extends Base {
  c = tag('Derived.c');
  constructor() {
    print('Derived before super');
    super();
    print('Derived after super, c=' + this.c);
    this.d = tag('Derived.d');
  }
}

new Derived();

// computed field names are evaluated ONCE at class definition time, in
// declaration order, BEFORE any instance exists.
let n = 0;
function key(s) { print('key ' + s); return s; }
class K {
  [key('k1')] = ++n;
  [key('k2')] = ++n;
  static [key('k3')] = 'static';
}
print('K keys ' + Object.keys(new K()).join(','));
print('K.k3 ' + K.k3);

// a field initialiser sees `this` and earlier fields, not later ones
class Self {
  x = 1;
  y = this.x + 1;
  z = typeof this.w;
  w = 4;
}
const s = new Self();
print('self ' + s.x + ' ' + s.y + ' ' + s.z + ' ' + s.w);

// fields shadow prototype accessors: [[Define]] not [[Set]]
class P { set v(x) { print('SETTER RAN ' + x); } }
class Q extends P { v = 9; }
print('define-not-set ' + new Q().v);
