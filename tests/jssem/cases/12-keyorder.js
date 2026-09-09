// pd03 -- OwnPropertyKeys ORDER. Integer-like keys ascending by numeric value,
// then string keys in creation order, then symbols in creation order. A
// reactivity system renders a list by iterating keys; a different order is a
// different render, with no error anywhere. Everything printed here is an
// order, which is the whole point.
(function () {
    'use strict';
    function ks(a) { return a.map(function (x) { return typeof x === 'symbol' ? String(x) : x; }).join('|'); }
    function show(tag, o) {
        print(tag);
        print('  ownKeys   ' + ks(Reflect.ownKeys(o)));
        print('  names     ' + ks(Object.getOwnPropertyNames(o)));
        print('  keys      ' + ks(Object.keys(o)));
        print('  symbols   ' + ks(Object.getOwnPropertySymbols(o)));
        var f = []; for (var k in o) f.push(k);
        print('  for-in    ' + f.join('|'));
        print('  entries   ' + Object.entries(o).map(function (e) { return e[0]; }).join('|'));
        print('  spread    ' + Object.keys({ ...o }).join('|'));
        print('  assign    ' + Object.keys(Object.assign({}, o)).join('|'));
        print('  JSON      ' + JSON.stringify(o));
    }

    var S1 = Symbol('one'), S2 = Symbol('two');

    // literal order deliberately scrambled: the ENGINE must reorder it
    var a = {};
    a.zebra = 1;
    a[2] = 'two';
    a[S1] = 'sym1';
    a.alpha = 2;
    a[0] = 'zero';
    a[S2] = 'sym2';
    a[1] = 'one';
    a['10'] = 'ten';
    a['-1'] = 'neg';
    a['01'] = 'leading-zero';
    a['1.5'] = 'frac';
    a[' 3'] = 'space';
    a['4294967294'] = 'max-index';
    a['4294967295'] = 'not-an-index';
    a['4294967296'] = 'beyond';
    a['0.0'] = 'zero-dot';
    a['-0'] = 'neg-zero';
    a['1e2'] = 'exp';
    a['Infinity'] = 'inf';
    show('scrambled-literal', a);

    // an object literal written in source order
    show('literal-source-order', { b: 1, 2: 'x', a: 2, 1: 'y', 0: 'z' });

    // delete then re-add: the key must move to the END of the string group
    var b = { x: 1, y: 2, z: 3 };
    delete b.x;
    b.x = 9;
    show('delete-then-readd', b);

    // integer keys are ordered NUMERICALLY, not lexicographically
    var c = {};
    ['10', '9', '100', '1', '20', '2'].forEach(function (k) { c[k] = k; });
    show('numeric-not-lexicographic', c);

    // an array, plus a non-index property, plus a hole
    var d = [1, 2, 3];
    d.extra = 'e';
    d[10] = 'sparse';
    show('array-with-extras', d);

    // class instance: fields, then assigned
    class K { constructor() { this.b = 1; this[1] = 'i'; this.a = 2; this[0] = 'j'; } }
    show('class-instance', new K());

    // Object.create with a descriptor map
    show('Object.create-descmap', Object.create(Object.prototype, {
        z: { value: 1, enumerable: true, configurable: true, writable: true },
        1: { value: 2, enumerable: true, configurable: true, writable: true },
        a: { value: 3, enumerable: true, configurable: true, writable: true },
        0: { value: 4, enumerable: true, configurable: true, writable: true }
    }));

    // non-enumerable keys appear in ownKeys/names and NOT in keys/for-in/JSON
    var e = { visible: 1 };
    Object.defineProperty(e, 'hidden', { value: 2, enumerable: false, configurable: true, writable: true });
    Object.defineProperty(e, 3, { value: 3, enumerable: false, configurable: true, writable: true });
    e.after = 4;
    show('mixed-enumerability', e);

    // for-in walks the prototype chain, own first, and DEDUPES a shadowed key
    var proto = { p1: 1, shared: 'proto', 5: 'protofive' };
    var child = Object.create(proto);
    child.c1 = 1;
    child.shared = 'own';
    child[2] = 'ownstwo';
    var fi = []; for (var k2 in child) fi.push(k2);
    print('for-in-proto-chain ' + fi.join('|'));
    print('  own-keys-only    ' + Object.keys(child).join('|'));

    // a shadowing key that is NON-enumerable hides the enumerable inherited one
    var child2 = Object.create({ hid: 'proto' });
    Object.defineProperty(child2, 'hid', { value: 'own', enumerable: false, configurable: true });
    var fi2 = []; for (var k3 in child2) fi2.push(k3);
    print('for-in-shadow-nonenum [' + fi2.join('|') + ']');

    // deleting during for-in: a key deleted before it is visited must not appear
    var f = { a: 1, b: 2, c: 3, d: 4 };
    var seen = [];
    for (var k4 in f) { seen.push(k4); if (k4 === 'a') { delete f.c; } }
    print('for-in-delete-during ' + seen.join('|'));

    // adding during for-in: implementation-defined whether it appears, but both
    // engines must agree with each other for a page to behave the same
    var g = { a: 1, b: 2 };
    var seen2 = [];
    for (var k5 in g) { seen2.push(k5); if (k5 === 'a') { g.zz = 3; } }
    print('for-in-add-during ' + seen2.join('|'));

    // JSON.stringify key order comes from the same list
    print('JSON-order ' + JSON.stringify({ b: 1, 2: 2, a: 3, 1: 4 }));

    // a property descriptor object's OWN key order is spec-fixed
    print('desc-data-order  ' + Object.keys(Object.getOwnPropertyDescriptor({ x: 1 }, 'x')).join('|'));
    print('desc-acc-order   ' + Object.keys(Object.getOwnPropertyDescriptor({ get x() { return 1; }, set x(v) {} }, 'x')).join('|'));
    print('desc-data-JSON   ' + JSON.stringify(Object.getOwnPropertyDescriptor({ x: 1 }, 'x')));
    print('descs-all        ' + JSON.stringify(Object.getOwnPropertyDescriptors({ b: 1, 1: 2, a: 3 })));

    // Object.groupBy / Map key order for completeness of the iteration family
    var m = new Map(); m.set('b', 1); m.set(1, 2); m.set('a', 3); m.set(0, 4);
    print('map-order ' + Array.from(m.keys()).join('|'));
    var st = new Set(['b', 1, 'a', 0, 1]);
    print('set-order ' + Array.from(st).join('|'));
})();
