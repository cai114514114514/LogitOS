// pd04 -- accessors, inheritance and descriptor transitions. A getter defined
// on a PROTOTYPE and inherited by instances is how every class-based observable
// works (mobx, Angular, Vue's computed); `this` inside it must be the instance,
// an assignment must reach the setter and not shadow it, and a setterless
// accessor must fail the way the spec says in each mode.
(function () {
    'use strict';
    function t(tag, fn) {
        var r;
        try { r = 'ok:' + String(fn()); }
        catch (e) { r = 'throw:' + (e && e.constructor ? e.constructor.name : String(e)); }
        print(tag + ' = ' + r);
    }
    function d(o, k) {
        var x = Object.getOwnPropertyDescriptor(o, k);
        if (!x) return 'undefined';
        return JSON.stringify(Object.keys(x).map(function (n) {
            var v = x[n];
            return n + '=' + (typeof v === 'function' ? 'fn' : String(v));
        }).join(','));
    }

    // --- a getter/setter on a prototype, inherited -----------------------
    var proto = {};
    var lastThisTag = '';
    Object.defineProperty(proto, 'v', {
        get: function () { lastThisTag = this.tag; return 'get(' + this.tag + ')'; },
        set: function (x) { this._v = x + '@' + this.tag; },
        enumerable: true, configurable: true
    });
    var i1 = Object.create(proto); i1.tag = 'i1';
    var i2 = Object.create(proto); i2.tag = 'i2';
    t('inherited-getter-this', function () { return i1.v + ' ' + i2.v; });
    t('inherited-setter-this', function () { i1.v = 'x'; return i1._v; });
    t('setter-did-not-shadow', function () { return Object.getOwnPropertyDescriptor(i1, 'v') === undefined; });
    t('own-keys-after-set', function () { return Object.keys(i1).join('|'); });
    t('desc-on-proto', function () { return d(proto, 'v'); });

    // --- inherited accessor with NO setter -------------------------------
    var p2 = {};
    Object.defineProperty(p2, 'ro', { get: function () { return 1; }, configurable: true, enumerable: true });
    var c2 = Object.create(p2);
    t('setterless-strict', function () { c2.ro = 5; return 'no-throw'; });
    t('setterless-sloppy', function () { return (function () { c2.ro = 5; return 'no-throw'; })(); });
    t('setterless-still-1', function () { return c2.ro; });
    t('setterless-defineProperty-shadows', function () {
        Object.defineProperty(c2, 'ro', { value: 7, writable: true, configurable: true, enumerable: true });
        return c2.ro;
    });

    // --- inherited NON-WRITABLE data property ----------------------------
    var p3 = {};
    Object.defineProperty(p3, 'frozen', { value: 1, writable: false, configurable: true, enumerable: true });
    var c3 = Object.create(p3);
    t('inherited-nonwritable-strict', function () { c3.frozen = 2; return 'no-throw'; });
    t('inherited-nonwritable-sloppy', function () { return (function () { c3.frozen = 2; return 'no-throw'; })(); });
    t('inherited-nonwritable-value', function () { return c3.frozen; });
    t('inherited-nonwritable-Reflect.set', function () { return Reflect.set(c3, 'frozen', 3); });

    // --- Object.defineProperty on an EXISTING property --------------------
    t('redefine-configurable', function () {
        var o = { a: 1 };
        Object.defineProperty(o, 'a', { value: 2 });
        return d(o, 'a');
    });
    t('redefine-partial-keeps-attrs', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: true, enumerable: true, configurable: true });
        Object.defineProperty(o, 'a', { value: 2 });
        return d(o, 'a');
    });
    t('define-new-defaults-false', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1 });
        return d(o, 'a');
    });
    t('assign-creates-all-true', function () {
        var o = {}; o.a = 1;
        return d(o, 'a');
    });
    t('redefine-nonconfigurable-same', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: false, enumerable: false, configurable: false });
        Object.defineProperty(o, 'a', { value: 1, writable: false, enumerable: false, configurable: false });
        return 'no-throw';
    });
    t('redefine-nonconfigurable-value', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'a', { value: 2 });
        return 'no-throw';
    });
    t('redefine-nonconfigurable-writable-to-false-ok', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: true, configurable: false });
        Object.defineProperty(o, 'a', { writable: false });
        return d(o, 'a');
    });
    t('redefine-nonconfigurable-writable-to-true', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: false, configurable: false });
        Object.defineProperty(o, 'a', { writable: true });
        return 'no-throw';
    });
    t('redefine-nonconfigurable-enumerable', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, enumerable: true, configurable: false });
        Object.defineProperty(o, 'a', { enumerable: false });
        return 'no-throw';
    });
    // a WRITABLE non-configurable data prop may still change value by assignment
    t('nonconfigurable-writable-assign', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, writable: true, configurable: false, enumerable: true });
        o.a = 9;
        return o.a + ' ' + d(o, 'a');
    });

    // --- data <-> accessor transitions -----------------------------------
    t('data-to-accessor-configurable', function () {
        var o = { a: 1 };
        Object.defineProperty(o, 'a', { get: function () { return 2; }, configurable: true });
        return o.a + ' ' + d(o, 'a');
    });
    t('data-to-accessor-nonconfigurable', function () {
        var o = {};
        Object.defineProperty(o, 'a', { value: 1, configurable: false });
        Object.defineProperty(o, 'a', { get: function () { return 2; } });
        return 'no-throw';
    });
    t('accessor-to-data-configurable', function () {
        var o = {};
        Object.defineProperty(o, 'a', { get: function () { return 1; }, configurable: true, enumerable: true });
        Object.defineProperty(o, 'a', { value: 5 });
        return o.a + ' ' + d(o, 'a');
    });
    t('accessor-to-data-nonconfigurable', function () {
        var o = {};
        Object.defineProperty(o, 'a', { get: function () { return 1; }, configurable: false });
        Object.defineProperty(o, 'a', { value: 5 });
        return 'no-throw';
    });
    t('accessor-replace-getter-nonconfigurable', function () {
        var o = {};
        var g = function () { return 1; };
        Object.defineProperty(o, 'a', { get: g, configurable: false });
        Object.defineProperty(o, 'a', { get: g });
        return 'same-getter-ok';
    });
    t('accessor-replace-getter-different', function () {
        var o = {};
        Object.defineProperty(o, 'a', { get: function () { return 1; }, configurable: false });
        Object.defineProperty(o, 'a', { get: function () { return 2; } });
        return 'no-throw';
    });
    t('getter-only-desc-set-undefined', function () {
        var o = {};
        Object.defineProperty(o, 'a', { get: function () { return 1; }, configurable: true });
        return d(o, 'a');
    });
    t('both-value-and-get', function () {
        Object.defineProperty({}, 'a', { value: 1, get: function () { return 2; } });
        return 'no-throw';
    });
    t('get-not-callable', function () {
        Object.defineProperty({}, 'a', { get: 42 });
        return 'no-throw';
    });
    t('get-undefined-is-ok', function () {
        var o = {};
        Object.defineProperty(o, 'a', { get: undefined, configurable: true });
        return d(o, 'a');
    });
    t('descriptor-is-a-proxy', function () {
        var seen = [];
        var desc = new Proxy({ value: 1, writable: true, enumerable: true, configurable: true }, {
            get: function (tt, k) { seen.push(String(k)); return Reflect.get(tt, k); },
            has: function (tt, k) { seen.push('has:' + String(k)); return Reflect.has(tt, k); }
        });
        var o = {};
        Object.defineProperty(o, 'a', desc);
        return o.a + ' [' + seen.join(',') + ']';
    });

    // --- class accessors on the prototype ---------------------------------
    class C {
        constructor() { this._n = 0; }
        get n() { return this._n; }
        set n(v) { this._n = v * 2; }
        static get sn() { return 'static'; }
    }
    t('class-accessor-on-proto', function () { return Object.getOwnPropertyDescriptor(C.prototype, 'n') !== undefined; });
    t('class-accessor-desc', function () { return d(C.prototype, 'n'); });
    t('class-accessor-not-own', function () { var c = new C(); c.n = 21; return c.n + ' own=' + Object.keys(c).join('|'); });
    t('class-static-accessor', function () { return C.sn; });
    t('class-proto-nonenumerable', function () { var c = new C(); var a = []; for (var k in c) a.push(k); return a.join('|'); });
    t('class-method-desc', function () { return d(C.prototype, 'constructor'); });

    // --- object-literal accessors -----------------------------------------
    var lit = { get g() { return 1; }, set g(v) { this._g = v; }, m() { return 2; } };
    t('literal-accessor-desc', function () { return d(lit, 'g'); });
    t('literal-method-desc', function () { return d(lit, 'm'); });
    t('literal-accessor-enumerable-in-forin', function () { var a = []; for (var k in lit) a.push(k); return a.join('|'); });

    // --- __defineGetter__ / __lookupGetter__ (legacy, still shipped) -------
    t('__defineGetter__', function () {
        var o = {};
        o.__defineGetter__('x', function () { return 'dg'; });
        return o.x + ' ' + d(o, 'x');
    });
    t('__lookupGetter__-inherited', function () {
        var pp = {}; pp.__defineGetter__('y', function () { return 1; });
        var cc = Object.create(pp);
        return typeof cc.__lookupGetter__('y');
    });
    t('__proto__-is-an-accessor', function () { return d(Object.prototype, '__proto__'); });

    // --- Reflect.set with a RECEIVER: the reactive-store primitive ---------
    t('Reflect.set-receiver-data', function () {
        var target = { a: 1 };
        var receiver = {};
        var r = Reflect.set(target, 'a', 2, receiver);
        return r + ' target=' + target.a + ' receiver=' + receiver.a + ' rdesc=' + d(receiver, 'a');
    });
    t('Reflect.set-receiver-accessor', function () {
        var got = [];
        var target = { set a(v) { got.push('setter-this-is-' + (this === target ? 'target' : 'receiver')); } };
        var receiver = {};
        var r = Reflect.set(target, 'a', 2, receiver);
        return r + ' [' + got.join(',') + '] receiver-own=' + Object.keys(receiver).join('|');
    });
    t('Reflect.set-receiver-nonwritable-target', function () {
        var target = {};
        Object.defineProperty(target, 'a', { value: 1, writable: false, configurable: false });
        return Reflect.set(target, 'a', 2, {});
    });
    t('Reflect.set-receiver-nonwritable-receiver', function () {
        var target = { a: 1 };
        var receiver = {};
        Object.defineProperty(receiver, 'a', { value: 0, writable: false, configurable: false });
        return Reflect.set(target, 'a', 2, receiver);
    });
    t('Reflect.set-receiver-primitive', function () { return Reflect.set({ a: 1 }, 'a', 2, 5); });
    t('Reflect.get-receiver', function () {
        var target = { get a() { return this.marker; } };
        return Reflect.get(target, 'a', { marker: 'from-receiver' });
    });

    // --- a proxy as PROTOTYPE: the trap sees the instance as receiver -------
    t('proxy-as-prototype', function () {
        var seen = [];
        var pp = new Proxy({}, {
            get: function (tt, k, r) { seen.push('get ' + String(k) + ' recv-is-child=' + (r === kid)); return 'V'; },
            set: function (tt, k, v, r) { seen.push('set ' + String(k) + ' recv-is-child=' + (r === kid)); return true; },
            has: function (tt, k) { seen.push('has ' + String(k)); return true; }
        });
        var kid = Object.create(pp);
        var v = kid.anything;
        kid.other = 1;
        var h = 'z' in kid;
        return v + ' own=' + Object.keys(kid).join('|') + ' [' + seen.join(',') + ']';
    });

    // --- super, which is a receiver-carrying [[Get]]/[[Set]] ---------------
    t('super-accessor-receiver', function () {
        var base = { get w() { return this.who; }, set w(v) { this.setOn = v; } };
        var derived = { __proto__: base, who: 'derived', run() { var g = super.w; super.w = 'sv'; return g + '/' + this.setOn; } };
        return derived.run() + ' own=' + Object.keys(derived).join('|');
    });
})();
