// pd01 -- every Proxy trap, and the SEQUENCE each language operation drives it
// in. A reactivity system is a pile of traps; which traps an operation calls,
// in what order, with what key, IS the observable behaviour. Everything here
// is a printed sequence, so a difference is a diff and not a judgement.
(function () {
    'use strict';
    var log = [];
    function L(s) { log.push(s); }
    function dump(tag) { print(tag + ' -> [' + log.join(',') + ']'); log = []; }
    function k(x) { return typeof x === 'symbol' ? String(x) : x; }

    var SYM = Symbol('s');

    function mk() {
        var t = { b: 2, a: 1 };
        t[0] = 'zero';
        t[SYM] = 'sym';
        Object.defineProperty(t, 'hidden', { value: 'h', enumerable: false, configurable: true, writable: true });
        return t;
    }

    var handler = {
        get: function (t, p, r) { L('get ' + k(p)); return Reflect.get(t, p, r); },
        set: function (t, p, v, r) { L('set ' + k(p) + '=' + v); return Reflect.set(t, p, v, r); },
        has: function (t, p) { L('has ' + k(p)); return Reflect.has(t, p); },
        deleteProperty: function (t, p) { L('del ' + k(p)); return Reflect.deleteProperty(t, p); },
        ownKeys: function (t) { L('ownKeys'); return Reflect.ownKeys(t); },
        getOwnPropertyDescriptor: function (t, p) { L('gopd ' + k(p)); return Reflect.getOwnPropertyDescriptor(t, p); },
        defineProperty: function (t, p, d) { L('defineProperty ' + k(p)); return Reflect.defineProperty(t, p, d); },
        getPrototypeOf: function (t) { L('getProto'); return Reflect.getPrototypeOf(t); },
        setPrototypeOf: function (t, v) { L('setProto'); return Reflect.setPrototypeOf(t, v); },
        isExtensible: function (t) { L('isExtensible'); return Reflect.isExtensible(t); },
        preventExtensions: function (t) { L('preventExtensions'); return Reflect.preventExtensions(t); }
    };

    // --- one operation at a time, each from a fresh proxy ---------------
    function op(tag, fn) {
        var p = new Proxy(mk(), handler);
        log = [];
        var r;
        try { r = fn(p); } catch (e) { r = '!' + e.constructor.name; }
        print(tag + ' = ' + String(r));
        dump('  ' + tag);
    }

    op('read-a', function (p) { return p.a; });
    op('read-missing', function (p) { return p.zzz; });
    op('write-existing', function (p) { p.a = 10; return p.a; });
    op('write-new', function (p) { p.n = 1; return p.n; });
    op('in-operator', function (p) { return 'a' in p; });
    op('delete', function (p) { return delete p.a; });
    op('Object.keys', function (p) { return Object.keys(p).join('|'); });
    op('Object.getOwnPropertyNames', function (p) { return Object.getOwnPropertyNames(p).join('|'); });
    op('Reflect.ownKeys', function (p) { return Reflect.ownKeys(p).map(k).join('|'); });
    op('Object.getOwnPropertySymbols', function (p) { return Object.getOwnPropertySymbols(p).map(k).join('|'); });
    op('Object.values', function (p) { return Object.values(p).join('|'); });
    op('Object.entries', function (p) { return Object.entries(p).map(function (e) { return e[0] + ':' + e[1]; }).join('|'); });
    op('spread', function (p) { var o = Object.assign({}, p); return Object.keys(o).join('|'); });
    op('object-spread-literal', function (p) { var o = { ...p }; return Object.keys(o).join('|'); });
    op('JSON.stringify', function (p) { return JSON.stringify(p); });
    op('for-in', function (p) { var a = []; for (var q in p) a.push(q); return a.join('|'); });
    op('Object.assign-target', function (p) { Object.assign(p, { a: 99, fresh: 1 }); return 'ok'; });
    op('getOwnPropertyDescriptor', function (p) { return JSON.stringify(Object.getOwnPropertyDescriptor(p, 'a')); });
    op('defineProperty', function (p) { Object.defineProperty(p, 'dp', { value: 5, configurable: true, writable: true, enumerable: true }); return p.dp; });
    op('getPrototypeOf', function (p) { return Object.getPrototypeOf(p) === Object.prototype; });
    op('instanceof', function (p) { return p instanceof Object; });
    op('setPrototypeOf', function (p) { Object.setPrototypeOf(p, null); return Object.getPrototypeOf(p); });
    op('isExtensible', function (p) { return Object.isExtensible(p); });
    op('preventExtensions', function (p) { Object.preventExtensions(p); return Object.isExtensible(p); });
    op('Object.seal', function (p) { Object.seal(p); return Object.isSealed(p); });
    op('Object.freeze', function (p) { Object.freeze(p); return Object.isFrozen(p); });
    op('String-coerce', function (p) { return String(p); });
    op('hasOwnProperty', function (p) { return Object.prototype.hasOwnProperty.call(p, 'a'); });
    op('Object.hasOwn', function (p) { return Object.hasOwn(p, 'a'); });
    op('compound-assign', function (p) { p.a += 1; return p.a; });
    op('increment', function (p) { p.a++; return p.a; });
    op('destructure', function (p) { var a = p.a, b = p.b; return a + '/' + b; });
    op('rest-destructure', function (p) { var { a, ...rest } = p; return a + '/' + Object.keys(rest).join('+'); });

    // --- apply and construct -------------------------------------------
    // `this` is undefined on a plain call in strict mode, so the constructor
    // guards rather than throwing -- an uncaught throw here would end the file
    // and silently delete every row below it, which is exactly how a
    // differential comes back green having measured nothing.
    function Base(x) { if (this !== undefined) { this.x = x; } return { called: x }; }
    Base.prototype.tag = 'base';
    var fh = {
        apply: function (t, thisArg, args) { L('apply ' + args.join('+')); return Reflect.apply(t, thisArg, args); },
        construct: function (t, args, nt) { L('construct ' + args.join('+') + ' nt=' + (nt === fp ? 'proxy' : nt === t ? 'target' : 'other')); return Reflect.construct(t, args, nt); },
        get: function (t, p, r) { L('fget ' + k(p)); return Reflect.get(t, p, r); }
    };
    var fp = new Proxy(Base, fh);

    log = [];
    print('typeof fnproxy = ' + typeof fp);
    print('call = ' + JSON.stringify(fp(1, 2)));
    dump('  call');

    log = [];
    var inst = new fp(7);
    print('new .x = ' + inst.x + ' proto-is-Base = ' + (Object.getPrototypeOf(inst) === Base.prototype));
    dump('  new');

    log = [];
    print('Reflect.construct-nt = ' + (function () {
        function Other() {}
        var o = Reflect.construct(fp, [3], Other);
        return Object.getPrototypeOf(o) === Other.prototype;
    })());
    dump('  reflect-construct');

    log = [];
    print('apply-via-call = ' + JSON.stringify(fp.call({}, 5)));
    dump('  fp.call');
})();
