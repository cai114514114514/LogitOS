// pd06 -- Proxy over the objects a framework actually wraps: arrays (a
// reactive list), functions, class instances with PRIVATE fields, and the
// built-in collections. An array proxy's `length` bookkeeping and a private
// field's brand check behind a proxy are the two places every proxy-based
// framework has had a bug, so they are the two places worth diffing.
(function () {
    'use strict';
    function t(tag, fn) {
        var r;
        try { r = 'ok:' + String(fn()); }
        catch (e) { r = 'throw:' + (e && e.constructor ? e.constructor.name : String(e)); }
        print(tag + ' = ' + r);
    }
    function traced(target, sink) {
        return new Proxy(target, {
            get: function (tt, k, r) { sink.push('get ' + String(k)); return Reflect.get(tt, k, r); },
            set: function (tt, k, v, r) { sink.push('set ' + String(k) + '=' + String(v)); return Reflect.set(tt, k, v, r); },
            has: function (tt, k) { sink.push('has ' + String(k)); return Reflect.has(tt, k); },
            deleteProperty: function (tt, k) { sink.push('del ' + String(k)); return Reflect.deleteProperty(tt, k); },
            ownKeys: function (tt) { sink.push('ownKeys'); return Reflect.ownKeys(tt); },
            getOwnPropertyDescriptor: function (tt, k) { sink.push('gopd ' + String(k)); return Reflect.getOwnPropertyDescriptor(tt, k); },
            defineProperty: function (tt, k, dd) { sink.push('dp ' + String(k)); return Reflect.defineProperty(tt, k, dd); }
        });
    }

    // --- arrays -----------------------------------------------------------
    t('Array.isArray-proxy', function () { return Array.isArray(new Proxy([], {})); });
    t('Array.isArray-nested-proxy', function () { return Array.isArray(new Proxy(new Proxy([], {}), {})); });
    t('Array.isArray-proxy-of-object', function () { return Array.isArray(new Proxy({}, {})); });
    t('array-proxy-length', function () { var s = []; var p = traced([1, 2, 3], s); return p.length; });
    t('array-proxy-push', function () {
        var s = []; var a = [1, 2]; var p = traced(a, s);
        p.push(3);
        return a.join(',') + ' [' + s.join(',') + ']';
    });
    t('array-proxy-index-write-grows', function () {
        var s = []; var a = [1]; var p = traced(a, s);
        p[3] = 'x';
        return a.length + ' [' + s.join(',') + ']';
    });
    t('array-proxy-length-shrink', function () {
        var s = []; var a = [1, 2, 3]; var p = traced(a, s);
        p.length = 1;
        return a.join(',') + '/' + a.length + ' [' + s.join(',') + ']';
    });
    t('array-proxy-pop', function () {
        var s = []; var a = [1, 2]; var p = traced(a, s);
        var v = p.pop();
        return v + ' len=' + a.length + ' [' + s.join(',') + ']';
    });
    t('array-proxy-splice', function () {
        var s = []; var a = [1, 2, 3]; var p = traced(a, s);
        p.splice(1, 1);
        return a.join(',') + ' traps=' + s.length;
    });
    t('array-proxy-forEach', function () {
        var s = []; var p = traced([1, 2], s); var out = [];
        p.forEach(function (v, i) { out.push(i + ':' + v); });
        return out.join('|') + ' [' + s.join(',') + ']';
    });
    t('array-proxy-spread', function () { var s = []; var p = traced([1, 2], s); return [...p].join(',') + ' [' + s.join(',') + ']'; });
    t('array-proxy-for-of', function () {
        var s = []; var p = traced([1, 2], s); var out = [];
        for (var v of p) out.push(v);
        return out.join(',') + ' [' + s.join(',') + ']';
    });
    t('array-proxy-JSON', function () { var s = []; var p = traced([1, 2], s); return JSON.stringify(p) + ' [' + s.join(',') + ']'; });
    t('array-proxy-concat', function () { return [0].concat(new Proxy([1, 2], {})).join(','); });
    t('array-proxy-isConcatSpreadable', function () {
        var p = new Proxy([1, 2], { get: function (tt, k) { if (k === Symbol.isConcatSpreadable) return false; return Reflect.get(tt, k); } });
        return [0].concat(p).length;
    });
    t('array-proxy-join', function () { return new Proxy([1, 2], {}).join('-'); });
    t('array-proxy-instanceof', function () { return (new Proxy([], {})) instanceof Array; });
    t('array-proxy-toString-tag', function () { return Object.prototype.toString.call(new Proxy([], {})); });
    t('object-proxy-toString-tag', function () { return Object.prototype.toString.call(new Proxy({}, {})); });
    t('fn-proxy-toString-tag', function () { return Object.prototype.toString.call(new Proxy(function () {}, {})); });
    t('array-length-nonwritable-through-proxy', function () {
        var a = [1, 2];
        Object.defineProperty(a, 'length', { writable: false });
        var p = new Proxy(a, {});
        p.push(3);
        return 'no-throw';
    });

    // --- functions --------------------------------------------------------
    t('fn-proxy-typeof', function () { return typeof new Proxy(function () {}, {}); });
    t('fn-proxy-name-length', function () {
        function nm(a, b) {}
        var p = new Proxy(nm, {});
        return p.name + '/' + p.length;
    });
    t('fn-proxy-bind', function () {
        var p = new Proxy(function (a) { return this.v + a; }, {});
        return p.bind({ v: 1 })(2);
    });
    t('fn-proxy-new-target', function () {
        var seen = 'none';
        function F() { seen = (new.target === undefined) ? 'undefined' : new.target.name; }
        var p = new Proxy(F, {});
        p(); var a = seen;
        new p(); var b = seen;
        return a + '/' + b;
    });
    t('fn-proxy-construct-newtarget-proxy', function () {
        function F() { this.nt = (new.target === p) ? 'proxy' : (new.target === F ? 'target' : 'other'); }
        var p = new Proxy(F, {});
        return new p().nt;
    });
    t('class-extends-proxy', function () {
        function Base() { this.b = 1; }
        var P = new Proxy(Base, {});
        class D extends P { constructor() { super(); this.d = 2; } }
        var o = new D();
        return o.b + '/' + o.d + '/' + (o instanceof D) + '/' + (o instanceof Base);
    });
    t('proxy-Symbol.hasInstance', function () {
        var C = function () {};
        var p = new Proxy(C, { get: function (tt, k) { if (k === Symbol.hasInstance) return function () { return true; }; return Reflect.get(tt, k); } });
        return ({}) instanceof p;
    });
    t('proxy-apply-trap-thisArg', function () {
        var got;
        var p = new Proxy(function () {}, { apply: function (tt, th, args) { got = (th === null ? 'null' : (th === undefined ? 'undefined' : typeof th)); return 1; } });
        p.call(undefined);
        var a = got;
        p.call(null);
        return a + '/' + got;
    });

    // --- private fields behind a proxy ------------------------------------
    class Priv {
        #x = 5;
        #m() { return 'pm'; }
        getX() { return this.#x; }
        setX(v) { this.#x = v; return this.#x; }
        callM() { return this.#m(); }
        static has(o) { return #x in o; }
    }
    t('private-direct', function () { return new Priv().getX(); });
    t('private-through-proxy-get', function () {
        var p = new Proxy(new Priv(), {});
        return p.getX();
    });
    t('private-through-proxy-set', function () {
        var p = new Proxy(new Priv(), {});
        return p.setX(9);
    });
    t('private-method-through-proxy', function () {
        return new Proxy(new Priv(), {}).callM();
    });
    t('private-brand-on-proxy', function () { return Priv.has(new Proxy(new Priv(), {})); });
    t('private-brand-on-instance', function () { return Priv.has(new Priv()); });
    t('private-brand-on-plain', function () { return Priv.has({}); });

    // --- built-in collections behind a proxy -------------------------------
    t('map-through-proxy', function () {
        var m = new Map([['a', 1]]);
        return new Proxy(m, {}).get('a');
    });
    t('map-through-proxy-rebound', function () {
        var m = new Map([['a', 1]]);
        var p = new Proxy(m, { get: function (tt, k, r) { var v = Reflect.get(tt, k, r); return typeof v === 'function' ? v.bind(tt) : v; } });
        return p.get('a') + '/' + p.size;
    });
    t('set-through-proxy', function () { return new Proxy(new Set([1]), {}).has(1); });
    t('date-through-proxy', function () { return new Proxy(new Date(0), {}).getTime(); });
    t('regexp-through-proxy', function () { return new Proxy(/a(b)/, {}).exec('zab')[1]; });
    t('weakmap-through-proxy', function () { var k = {}; var w = new WeakMap([[k, 1]]); return new Proxy(w, {}).get(k); });
    t('typedarray-through-proxy', function () { var ta = new Uint8Array([1, 2]); return new Proxy(ta, {})[0]; });
    t('promise-through-proxy-typeof-then', function () { return typeof new Proxy(Promise.resolve(1), {}).then; });
    t('error-through-proxy-message', function () { return new Proxy(new TypeError('m'), {}).message; });
    t('arguments-through-proxy', function () {
        return (function () { return new Proxy(arguments, {})[0]; })('ax');
    });

    // --- iteration through traps -------------------------------------------
    t('iterator-symbol-trapped', function () {
        var s = [];
        var p = new Proxy({ 0: 'a', 1: 'b', length: 2 }, {
            get: function (tt, k, r) { s.push(String(k)); return Reflect.get(tt, k, r); }
        });
        return Array.from(p).join(',') + ' [' + s.join(',') + ']';
    });
    t('custom-iterator-through-proxy', function () {
        var o = {}; o[Symbol.iterator] = function () { var i = 0; return { next: function () { return i < 2 ? { value: i++, done: false } : { value: undefined, done: true }; } }; };
        return [...new Proxy(o, {})].join(',');
    });
    t('proxy-Symbol.toPrimitive', function () {
        var p = new Proxy({}, { get: function (tt, k) { if (k === Symbol.toPrimitive) return function (h) { return 'tp:' + h; }; return Reflect.get(tt, k); } });
        return (p + '') + ' ' + (+p === +p ? 'num-ok' : 'num-nan') + ' ' + String(p);
    });
    t('proxy-Symbol.toStringTag', function () {
        var p = new Proxy({}, { get: function (tt, k) { if (k === Symbol.toStringTag) return 'Tagged'; return Reflect.get(tt, k); } });
        return Object.prototype.toString.call(p);
    });
})();
