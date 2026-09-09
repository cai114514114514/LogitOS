// s03 -- the three remaining load-bearing shapes, in one file because each is
// short and none needs the others:
//
//   A. CLASS COMPONENTS: public and private (#x) fields, static blocks,
//      accessors defined on the PROTOTYPE (not the instance), private methods,
//      and the `#x in o` brand check that a class-based library uses to tell
//      its own instances from a look-alike.
//   B. TAGGED TEMPLATES: the strings array must be the SAME OBJECT on every
//      call from one call site and frozen, because a template-literal library
//      uses it as a WeakMap key to cache the parsed template. If identity is
//      not stable the cache misses forever and the library re-parses on every
//      render -- which is not a crash, it is a silent performance collapse,
//      the worst failure shape to find by looking.
//   C. WEAK IDENTITY: WeakMap/WeakSet/WeakRef as caches keyed by object.

// --- A ---------------------------------------------------------------------
class Base {
  static registry = [];
  static { Base.registry.push('static-block-ran'); }
  #secret = 'hidden';
  count = 0;
  constructor(n) { this.name = n; }
  get label() { return 'L:' + this.name; }
  set label(v) { this.name = v.replace('L:', ''); }
  #bump() { this.count++; return this.count; }
  tick() { return this.#bump(); }
  static isMine(o) { return #secret in o; }
  peek() { return this.#secret; }
}
class Derived extends Base {
  constructor(n) { super(n); this.extra = true; }
  get label() { return 'D:' + super.label; }
}

var b = new Base('one'), d = new Derived('two');
print('static block =', Base.registry.join(','));
print('field + ctor =', b.name + '|' + b.count);
print('private method =', b.tick() + ',' + b.tick());
print('brand check =', Base.isMine(b) + ',' + Base.isMine({}) + ',' + Base.isMine(d));
print('accessor on proto =',
      Object.getOwnPropertyDescriptor(Base.prototype, 'label') !== undefined);
print('getter =', b.label, '| super getter =', d.label);
b.label = 'L:three';
print('setter =', b.name);
print('private read =', b.peek());
print('field is own, method is not =',
      Object.prototype.hasOwnProperty.call(b, 'count') + ',' +
      Object.prototype.hasOwnProperty.call(b, 'tick'));

// --- B ---------------------------------------------------------------------
var seen = [];
function tag(strings) {
  seen.push(strings);
  return strings.raw.length + ':' + strings.join('|') + ':' + strings.raw.join('|');
}
function render(x) { return tag`a${x}b\n`; }
var r1 = render(1), r2 = render(2);
print('tagged result =', r1);
print('cooked vs raw =', (r1 === r2));
print('strings identity across calls =', seen[0] === seen[1]);
print('strings frozen =', Object.isFrozen(seen[0]) + ',' + Object.isFrozen(seen[0].raw));
function other(x) { return tag`a${x}b\n`; }   // identical TEXT, different site
other(3);
print('different call site => different object =', (seen[2] !== seen[0]));

var tcache = new WeakMap();
function cached(strings) {
  if (!tcache.has(strings)) tcache.set(strings, { parsed: strings.join('~') });
  return tcache.get(strings);
}
print('template cache hit =', cached(seen[0]) === cached(seen[1]));

// --- C ---------------------------------------------------------------------
var wm = new WeakMap(), ws = new WeakSet();
var k1 = {}, k2 = {};
wm.set(k1, 'v1'); ws.add(k1);
print('weakmap =', wm.get(k1) + ',' + wm.get(k2) + ',' + wm.has(k1) + ',' + wm.has(k2));
print('weakset =', ws.has(k1) + ',' + ws.has(k2));
print('typeof WeakRef/FinalizationRegistry =',
      typeof WeakRef + ',' + typeof FinalizationRegistry);
var wr = new WeakRef(k1);
print('weakref deref identity =', wr.deref() === k1);
var fr = new FinalizationRegistry(function () {});
fr.register(k1, 'token');
print('registry register returned =', fr.register(k2, 't2'));

// Symbol-keyed metadata, the other identity mechanism libraries use.
var S = Symbol('meta');
var host = {}; host[S] = 42;
print('symbol key =', host[S] + '|' + Object.keys(host).length + '|' +
      Object.getOwnPropertySymbols(host).length);
