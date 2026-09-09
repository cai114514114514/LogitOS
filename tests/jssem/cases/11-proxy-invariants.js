// pd02 -- the Proxy INVARIANTS. A handler that lies about a non-configurable
// property must make the operation THROW; that throw is how a caller learns an
// object is really frozen. An engine that silently accepts the lie hands the
// caller the lie instead, which is strictly worse than having no Proxy at all.
// Every case prints either the thrown class or the value that came back.
(function () {
    'use strict';
    function t(tag, fn) {
        var r;
        try { r = 'ok:' + String(fn()); }
        catch (e) { r = 'throw:' + (e && e.constructor ? e.constructor.name : String(e)); }
        print(tag + ' = ' + r);
    }

    function ncTarget() {
        var o = {};
        Object.defineProperty(o, 'nc', { value: 1, writable: false, configurable: false, enumerable: true });
        Object.defineProperty(o, 'ncw', { value: 2, writable: true, configurable: false, enumerable: true });
        Object.defineProperty(o, 'ncacc', { get: function () { return 3; }, configurable: false, enumerable: true });
        return o;
    }

    // [[Get]] must report the real value of a non-configurable non-writable prop
    t('get-lies-nonwritable', function () {
        return new Proxy(ncTarget(), { get: function () { return 'LIE'; } }).nc;
    });
    t('get-truth-nonwritable', function () {
        return new Proxy(ncTarget(), { get: function () { return 1; } }).nc;
    });
    t('get-lies-writable-ok', function () {
        return new Proxy(ncTarget(), { get: function () { return 'LIE'; } }).ncw;
    });
    // an accessor with no getter must report undefined
    t('get-lies-accessor-no-getter', function () {
        var o = {};
        Object.defineProperty(o, 'x', { set: function () {}, configurable: false });
        return new Proxy(o, { get: function () { return 'LIE'; } }).x;
    });

    // [[Set]] must not claim success on a non-writable non-configurable prop
    t('set-lies-nonwritable', function () {
        var p = new Proxy(ncTarget(), { set: function () { return true; } });
        p.nc = 'LIE';
        return p.nc;
    });
    t('set-false-sloppy', function () {
        return (function () {
            var p = new Proxy({}, { set: function () { return false; } });
            p.a = 1;                      // sloppy: silent
            return 'no-throw';
        })();
    });
    t('set-false-strict', function () {
        var p = new Proxy({}, { set: function () { return false; } });
        p.a = 1;                          // strict (this file): TypeError
        return 'no-throw';
    });

    // [[HasProperty]] must not deny a non-configurable own property
    t('has-lies-nonconfigurable', function () {
        return 'nc' in new Proxy(ncTarget(), { has: function () { return false; } });
    });
    // ...nor an own property of a non-extensible target
    t('has-lies-nonextensible', function () {
        var o = { a: 1 }; Object.preventExtensions(o);
        return 'a' in new Proxy(o, { has: function () { return false; } });
    });

    // [[Delete]] must not claim to have deleted a non-configurable property
    t('delete-lies-nonconfigurable', function () {
        var p = new Proxy(ncTarget(), { deleteProperty: function () { return true; } });
        return delete p.nc;
    });

    // [[OwnPropertyKeys]] invariants
    t('ownKeys-omits-nonconfigurable', function () {
        return Object.getOwnPropertyNames(new Proxy(ncTarget(), { ownKeys: function () { return ['ncw', 'ncacc']; } })).join('|');
    });
    t('ownKeys-duplicate', function () {
        return Object.getOwnPropertyNames(new Proxy({}, { ownKeys: function () { return ['a', 'a']; } })).join('|');
    });
    t('ownKeys-extra-on-nonextensible', function () {
        var o = { a: 1 }; Object.preventExtensions(o);
        return Object.getOwnPropertyNames(new Proxy(o, { ownKeys: function () { return ['a', 'ghost']; } })).join('|');
    });
    t('ownKeys-missing-on-nonextensible', function () {
        var o = { a: 1, b: 2 }; Object.preventExtensions(o);
        return Object.getOwnPropertyNames(new Proxy(o, { ownKeys: function () { return ['a']; } })).join('|');
    });
    t('ownKeys-nonstring-nonsymbol', function () {
        return Object.getOwnPropertyNames(new Proxy({}, { ownKeys: function () { return [1]; } })).join('|');
    });

    // [[GetOwnProperty]] invariants
    t('gopd-undefined-for-nonconfigurable', function () {
        return String(Object.getOwnPropertyDescriptor(new Proxy(ncTarget(), { getOwnPropertyDescriptor: function () { return undefined; } }), 'nc'));
    });
    t('gopd-undefined-on-nonextensible', function () {
        var o = { a: 1 }; Object.preventExtensions(o);
        return String(Object.getOwnPropertyDescriptor(new Proxy(o, { getOwnPropertyDescriptor: function () { return undefined; } }), 'a'));
    });
    t('gopd-reports-configurable-for-nonconfigurable', function () {
        return JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy(ncTarget(), {
            getOwnPropertyDescriptor: function () { return { value: 1, writable: false, enumerable: true, configurable: true }; }
        }), 'nc'));
    });
    t('gopd-invents-prop-on-nonextensible', function () {
        var o = {}; Object.preventExtensions(o);
        return JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy(o, {
            getOwnPropertyDescriptor: function () { return { value: 1, writable: true, enumerable: true, configurable: true }; }
        }), 'ghost'));
    });
    t('gopd-nonconfigurable-for-absent', function () {
        return JSON.stringify(Object.getOwnPropertyDescriptor(new Proxy({}, {
            getOwnPropertyDescriptor: function () { return { value: 1, writable: true, enumerable: true, configurable: false }; }
        }), 'ghost'));
    });

    // [[DefineOwnProperty]] invariants
    t('defineProperty-true-on-nonextensible', function () {
        var o = {}; Object.preventExtensions(o);
        Object.defineProperty(new Proxy(o, { defineProperty: function () { return true; } }), 'ghost', { value: 1, configurable: true });
        return 'no-throw';
    });
    t('defineProperty-nonconfigurable-not-on-target', function () {
        Object.defineProperty(new Proxy({}, { defineProperty: function () { return true; } }), 'x', { value: 1, configurable: false });
        return 'no-throw';
    });
    t('defineProperty-false-throws-in-Object.defineProperty', function () {
        Object.defineProperty(new Proxy({}, { defineProperty: function () { return false; } }), 'x', { value: 1 });
        return 'no-throw';
    });
    t('defineProperty-false-Reflect', function () {
        return Reflect.defineProperty(new Proxy({}, { defineProperty: function () { return false; } }), 'x', { value: 1 });
    });

    // [[IsExtensible]] must agree with the target
    t('isExtensible-lies', function () {
        return Object.isExtensible(new Proxy({}, { isExtensible: function () { return false; } }));
    });
    t('preventExtensions-lies', function () {
        Object.preventExtensions(new Proxy({}, { preventExtensions: function () { return true; } }));
        return 'no-throw';
    });

    // [[GetPrototypeOf]] must agree with a non-extensible target
    t('getProto-lies-nonextensible', function () {
        var o = {}; Object.preventExtensions(o);
        return String(Object.getPrototypeOf(new Proxy(o, { getPrototypeOf: function () { return null; } })));
    });
    t('getProto-lies-extensible-ok', function () {
        return String(Object.getPrototypeOf(new Proxy({}, { getPrototypeOf: function () { return null; } })));
    });
    t('getProto-nonobject', function () {
        return String(Object.getPrototypeOf(new Proxy({}, { getPrototypeOf: function () { return 42; } })));
    });
    t('setProto-lies-nonextensible', function () {
        var o = {}; Object.preventExtensions(o);
        Object.setPrototypeOf(new Proxy(o, { setPrototypeOf: function () { return true; } }), { z: 1 });
        return 'no-throw';
    });

    // a non-callable trap is a TypeError at the point of use, not at construction
    t('noncallable-trap-construct', function () { new Proxy({}, { get: 42 }); return 'constructed'; });
    t('noncallable-trap-use', function () { return new Proxy({}, { get: 42 }).a; });
    t('trap-absent-is-not-null', function () { return new Proxy({ a: 5 }, { get: undefined }).a; });
    t('trap-null-is-TypeError', function () { return new Proxy({ a: 5 }, { get: null }).a; });

    // apply/construct on a non-callable target
    t('proxy-nonfunction-call', function () { return new Proxy({}, { apply: function () { return 1; } })(); });
    t('construct-trap-returns-primitive', function () { return new (new Proxy(function () {}, { construct: function () { return 1; } }))(); });
    t('apply-on-arrow-as-construct', function () { return new (new Proxy(function () {}, {}))(); });

    // a revoked proxy
    t('revoked-get', function () { var r = Proxy.revocable({ a: 1 }, {}); r.revoke(); return r.proxy.a; });
    t('revoked-typeof', function () { var r = Proxy.revocable({ a: 1 }, {}); r.revoke(); return typeof r.proxy; });
    t('revoked-typeof-fn', function () { var r = Proxy.revocable(function () {}, {}); r.revoke(); return typeof r.proxy; });
    t('revoke-twice', function () { var r = Proxy.revocable({}, {}); r.revoke(); r.revoke(); return 'ok'; });
    t('revoked-in-weakmap', function () { var r = Proxy.revocable({}, {}); var m = new WeakMap(); m.set(r.proxy, 1); r.revoke(); return m.get(r.proxy); });
})();
