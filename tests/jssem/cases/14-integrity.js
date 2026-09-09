// pd05 -- freeze / seal / preventExtensions and the TESTS for them.
//
// TestIntegrityLevel (spec 7.3.15) begins "Let extensible be ?IsExtensible(O);
// if extensible is true, return false" -- so on an EXTENSIBLE object the answer
// is false and NO other trap may run. That short-circuit is observable through
// a Proxy, and a framework that calls Object.isFrozen on a reactive object to
// decide whether to track it pays for every trap the engine runs anyway.
(function () {
    'use strict';
    function t(tag, fn) {
        var r;
        try { r = 'ok:' + String(fn()); }
        catch (e) { r = 'throw:' + (e && e.constructor ? e.constructor.name : String(e)); }
        print(tag + ' = ' + r);
    }

    // --- the short-circuit, watched through a proxy ---------------------
    function probe(fn, mkTarget) {
        var log = [];
        var h = {};
        ['get', 'set', 'has', 'deleteProperty', 'ownKeys', 'getOwnPropertyDescriptor',
         'defineProperty', 'getPrototypeOf', 'setPrototypeOf', 'isExtensible',
         'preventExtensions'].forEach(function (n) {
            h[n] = function (tt, a, b, c) {
                log.push(n + (typeof a === 'string' || typeof a === 'symbol' ? ' ' + String(a) : ''));
                return Reflect[n === 'deleteProperty' ? 'deleteProperty' : n](tt, a, b, c);
            };
        });
        var p = new Proxy(mkTarget(), h);
        var v;
        try { v = String(fn(p)); } catch (e) { v = '!' + e.constructor.name; }
        return v + ' [' + log.join(',') + ']';
    }
    function twoProps() { return { a: 1, b: 2 }; }
    function sealedTwo() { var o = twoProps(); Object.seal(o); return o; }
    function frozenTwo() { var o = twoProps(); Object.freeze(o); return o; }
    function emptyNonExt() { var o = {}; Object.preventExtensions(o); return o; }

    print('isFrozen-on-extensible   ' + probe(function (p) { return Object.isFrozen(p); }, twoProps));
    print('isSealed-on-extensible   ' + probe(function (p) { return Object.isSealed(p); }, twoProps));
    print('isFrozen-on-frozen       ' + probe(function (p) { return Object.isFrozen(p); }, frozenTwo));
    print('isSealed-on-sealed       ' + probe(function (p) { return Object.isSealed(p); }, sealedTwo));
    print('isFrozen-on-sealed       ' + probe(function (p) { return Object.isFrozen(p); }, sealedTwo));
    print('isFrozen-on-empty-nonext ' + probe(function (p) { return Object.isFrozen(p); }, emptyNonExt));
    print('isExtensible             ' + probe(function (p) { return Object.isExtensible(p); }, twoProps));
    print('freeze                   ' + probe(function (p) { Object.freeze(p); return 'done'; }, twoProps));
    print('seal                     ' + probe(function (p) { Object.seal(p); return 'done'; }, twoProps));
    print('preventExtensions        ' + probe(function (p) { Object.preventExtensions(p); return 'done'; }, twoProps));
    print('freeze-then-isFrozen     ' + probe(function (p) { Object.freeze(p); return Object.isFrozen(p); }, twoProps));

    // --- ordinary objects ------------------------------------------------
    t('freeze-returns-same', function () { var o = { a: 1 }; return Object.freeze(o) === o; });
    t('frozen-write-sloppy', function () { var o = Object.freeze({ a: 1 }); return (function () { o.a = 2; return o.a; })(); });
    t('frozen-write-strict', function () { var o = Object.freeze({ a: 1 }); o.a = 2; return 'no-throw'; });
    t('frozen-add-strict', function () { var o = Object.freeze({ a: 1 }); o.b = 2; return 'no-throw'; });
    t('frozen-delete-strict', function () { var o = Object.freeze({ a: 1 }); return delete o.a; });
    t('sealed-write-ok', function () { var o = Object.seal({ a: 1 }); o.a = 2; return o.a; });
    t('sealed-delete-strict', function () { var o = Object.seal({ a: 1 }); return delete o.a; });
    t('sealed-add-strict', function () { var o = Object.seal({ a: 1 }); o.b = 2; return 'no-throw'; });
    t('preventExtensions-write-ok', function () { var o = { a: 1 }; Object.preventExtensions(o); o.a = 2; return o.a; });
    t('preventExtensions-delete-ok', function () { var o = { a: 1 }; Object.preventExtensions(o); return delete o.a; });
    t('preventExtensions-setProto', function () { var o = {}; Object.preventExtensions(o); Object.setPrototypeOf(o, { z: 1 }); return 'no-throw'; });
    t('preventExtensions-setProto-same', function () { var o = {}; Object.preventExtensions(o); Object.setPrototypeOf(o, Object.prototype); return 'no-throw'; });
    t('frozen-primitive-passthrough', function () { return String(Object.freeze(5)) + ' ' + Object.isFrozen(5); });
    t('isFrozen-primitive', function () { return Object.isFrozen('str') + ' ' + Object.isSealed(7); });
    t('freeze-null', function () { return Object.freeze(null); });
    t('isFrozen-empty-object', function () { return Object.isFrozen({}); });
    t('isFrozen-empty-nonextensible', function () { var o = {}; Object.preventExtensions(o); return Object.isFrozen(o); });

    // accessors survive freeze: a getter is not made non-writable, only
    // non-configurable, so a computed property on a frozen object still runs
    t('freeze-accessor', function () {
        var o = { get g() { return 'gv'; }, set s(v) { this._s = v; } };
        Object.freeze(o);
        var dsc = Object.getOwnPropertyDescriptor(o, 'g');
        return o.g + ' cfg=' + dsc.configurable + ' hasGet=' + (typeof dsc.get === 'function') + ' isFrozen=' + Object.isFrozen(o);
    });
    t('frozen-accessor-set-strict', function () {
        var o = { set s(v) { this._s = v; } };
        Object.freeze(o);
        o.s = 1;
        return 'no-throw setter-ran=' + (o._s === undefined ? 'no' : 'yes');
    });

    // --- arrays ----------------------------------------------------------
    t('freeze-array-push', function () { var a = Object.freeze([1, 2]); a.push(3); return 'no-throw'; });
    t('freeze-array-index-strict', function () { var a = Object.freeze([1, 2]); a[0] = 9; return 'no-throw'; });
    t('freeze-array-length-desc', function () { var a = Object.freeze([1]); var dd = Object.getOwnPropertyDescriptor(a, 'length'); return 'w=' + dd.writable + ' c=' + dd.configurable; });
    t('seal-array-push', function () { var a = Object.seal([1, 2]); a.push(3); return 'no-throw'; });
    t('seal-array-index-ok', function () { var a = Object.seal([1, 2]); a[0] = 9; return a[0]; });
    t('seal-array-length-desc', function () { var a = Object.seal([1]); var dd = Object.getOwnPropertyDescriptor(a, 'length'); return 'w=' + dd.writable + ' c=' + dd.configurable; });
    t('isFrozen-empty-frozen-array', function () { return Object.isFrozen(Object.freeze([])); });
    t('freeze-array-sort', function () { var a = Object.freeze([2, 1]); a.sort(); return 'no-throw'; });
    t('nonwritable-length-push', function () {
        var a = [1];
        Object.defineProperty(a, 'length', { writable: false });
        a.push(2);
        return 'no-throw';
    });
    t('array-length-truncate-nonconfigurable', function () {
        var a = [1, 2, 3];
        Object.defineProperty(a, 1, { value: 2, configurable: false });
        a.length = 0;
        return a.length + '/' + a.join(',');
    });

    // --- functions -------------------------------------------------------
    t('freeze-function', function () { var f = function () {}; Object.freeze(f); return Object.isFrozen(f) + ' ' + (typeof f); });
    t('frozen-function-prototype-write', function () {
        function f() {}
        Object.freeze(f);
        f.prototype = {};
        return 'no-throw';
    });
    t('function-name-desc', function () { function nm() {} var dd = Object.getOwnPropertyDescriptor(nm, 'name'); return 'w=' + dd.writable + ' e=' + dd.enumerable + ' c=' + dd.configurable + ' v=' + dd.value; });
    t('function-length-desc', function () { function nm(a, b) {} var dd = Object.getOwnPropertyDescriptor(nm, 'length'); return 'w=' + dd.writable + ' e=' + dd.enumerable + ' c=' + dd.configurable + ' v=' + dd.value; });
    t('class-prototype-desc', function () { class Z {} var dd = Object.getOwnPropertyDescriptor(Z, 'prototype'); return 'w=' + dd.writable + ' e=' + dd.enumerable + ' c=' + dd.configurable; });
    t('arrow-has-no-prototype', function () { var a = function () {}; var b = () => 1; return ('prototype' in a) + '/' + ('prototype' in b); });

    // --- freeze deep-ish: the sealed-object identity a store relies on ----
    t('frozen-object-still-proxyable', function () {
        var o = Object.freeze({ a: 1 });
        var p = new Proxy(o, { get: function (tt, k) { return Reflect.get(tt, k); } });
        return p.a + ' isFrozen=' + Object.isFrozen(p);
    });
    t('proxy-of-frozen-get-lie', function () {
        var o = Object.freeze({ a: 1 });
        return new Proxy(o, { get: function () { return 'LIE'; } }).a;
    });

    // --- WHAT THE SHORT-CIRCUIT IS FOR ------------------------------------
    // If IsExtensible is consulted first, an extensible object answers false
    // and the key traps never run -- so they cannot throw, and cannot lie.
    // Consulting it last makes the answer depend on traps the spec says are
    // unreachable, which turns a `false` into an exception.
    t('isFrozen-throwing-ownKeys-extensible', function () {
        return Object.isFrozen(new Proxy({ a: 1 }, { ownKeys: function () { throw new RangeError('ownKeys'); } }));
    });
    t('isSealed-throwing-ownKeys-extensible', function () {
        return Object.isSealed(new Proxy({ a: 1 }, { ownKeys: function () { throw new RangeError('ownKeys'); } }));
    });
    t('isFrozen-throwing-gopd-extensible', function () {
        return Object.isFrozen(new Proxy({ a: 1 }, { getOwnPropertyDescriptor: function () { throw new RangeError('gopd'); } }));
    });
    t('isFrozen-invariant-violating-ownKeys-extensible', function () {
        // ownKeys returns a duplicate: a TypeError from the invariant check on
        // a path the spec never reaches for an extensible object.
        return Object.isFrozen(new Proxy({ a: 1 }, { ownKeys: function () { return ['a', 'a']; } }));
    });
    t('isFrozen-side-effecting-ownKeys-extensible', function () {
        var n = 0;
        var p = new Proxy({ a: 1 }, { ownKeys: function (tt) { n++; return Reflect.ownKeys(tt); } });
        Object.isFrozen(p); Object.isSealed(p);
        return 'ownKeys-trap-ran=' + n;
    });
    // The same shape, one level down: a plain (non-proxy) extensible object
    // must also answer without walking its properties. Observable only by
    // count, so it is measured through a proxy above; here the VALUE is
    // checked to stay correct on a large object.
    t('isFrozen-big-extensible-value', function () {
        var o = {};
        for (var i = 0; i < 50; i++) { o['k' + i] = i; }
        return Object.isFrozen(o) + '/' + Object.isSealed(o) + '/' + Object.isExtensible(o);
    });
})();
