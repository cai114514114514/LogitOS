// 16 -- [[DefineOwnProperty]] through a Proxy, in the shape a wrapper actually
// takes. A handler that FORWARDS faithfully -- `defineProperty(t,k,d){ return
// Reflect.defineProperty(t,k,d) }` -- must be indistinguishable from having no
// trap at all. Every row below is that identity, tested against the property
// attributes that make it hard: non-configurable-but-writable, which is what an
// array's `length` is and what every array mutator writes.
(function () {
    'use strict';
    function t(tag, fn) {
        var r;
        try { r = 'ok:' + String(fn()); }
        catch (e) { r = 'throw:' + (e && e.constructor ? e.constructor.name : String(e)); }
        print(tag + ' = ' + r);
    }
    // the faithful forwarder: no behaviour of its own
    function fwd() { return { defineProperty: function (tt, k, d) { return Reflect.defineProperty(tt, k, d); } }; }
    // every trap, all forwarding -- what a tracking wrapper looks like
    function fwdAll() {
        return {
            get: function (tt, k, r) { return Reflect.get(tt, k, r); },
            set: function (tt, k, v, r) { return Reflect.set(tt, k, v, r); },
            has: function (tt, k) { return Reflect.has(tt, k); },
            deleteProperty: function (tt, k) { return Reflect.deleteProperty(tt, k); },
            ownKeys: function (tt) { return Reflect.ownKeys(tt); },
            getOwnPropertyDescriptor: function (tt, k) { return Reflect.getOwnPropertyDescriptor(tt, k); },
            defineProperty: function (tt, k, d) { return Reflect.defineProperty(tt, k, d); }
        };
    }
    function withAttrs(a) { var o = {}; Object.defineProperty(o, 'p', a); return o; }

    // --- the identity, one property-attribute combination per row ---------
    // c = configurable, w = writable. Each row redefines `p` with a PARTIAL
    // descriptor, which is what an engine's own array code emits.
    [
        ['c+w  ', { value: 1, writable: true, configurable: true, enumerable: true }],
        ['c+nw ', { value: 1, writable: false, configurable: true, enumerable: true }],
        ['nc+w ', { value: 1, writable: true, configurable: false, enumerable: true }],
        ['nc+nw', { value: 1, writable: false, configurable: false, enumerable: true }]
    ].forEach(function (row) {
        var tag = row[0], attrs = row[1];
        t('dp ' + tag + ' <- {value:2}          ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { value: 2 }); return o.p; });
        t('dp ' + tag + ' <- {value:2,w:true}   ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { value: 2, writable: true }); return o.p; });
        t('dp ' + tag + ' <- {value:2,w:false}  ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { value: 2, writable: false }); return o.p; });
        t('dp ' + tag + ' <- {w:true}           ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { writable: true }); return o.p; });
        t('dp ' + tag + ' <- {w:false}          ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { writable: false }); return o.p; });
        t('dp ' + tag + ' <- {enumerable:true}  ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, fwd()), 'p', { enumerable: true }); return o.p; });
        t('dp ' + tag + ' NO-TRAP <- {value:2}  ', function () { var o = withAttrs(attrs); Object.defineProperty(new Proxy(o, {}), 'p', { value: 2 }); return o.p; });
        t('dp ' + tag + ' NO-PROXY <- {value:2} ', function () { var o = withAttrs(attrs); Object.defineProperty(o, 'p', { value: 2 }); return o.p; });
        t('dp ' + tag + ' assign through trap   ', function () { var o = withAttrs(attrs); var p = new Proxy(o, fwd()); try { p.p = 3; } catch (e) { throw e; } return o.p; });
    });

    // --- the same identity over an ARRAY, which is where it is reached ----
    // `length` is non-configurable and writable, so every mutator that changes
    // it takes the nc+w path above.
    function arrOp(name, fn) {
        t('array ' + name + ' bare      ', function () { var a = [3, 1, 2]; var p = new Proxy(a, {}); fn(p); return a.join(','); });
        t('array ' + name + ' fwd-dp    ', function () { var a = [3, 1, 2]; var p = new Proxy(a, fwd()); fn(p); return a.join(','); });
        t('array ' + name + ' fwd-all   ', function () { var a = [3, 1, 2]; var p = new Proxy(a, fwdAll()); fn(p); return a.join(','); });
        t('array ' + name + ' no-proxy  ', function () { var a = [3, 1, 2]; fn(a); return a.join(','); });
    }
    arrOp('push    ', function (x) { x.push(9); });
    arrOp('pop     ', function (x) { x.pop(); });
    arrOp('shift   ', function (x) { x.shift(); });
    arrOp('unshift ', function (x) { x.unshift(0); });
    arrOp('splice  ', function (x) { x.splice(1, 1); });
    arrOp('sort    ', function (x) { x.sort(); });
    arrOp('reverse ', function (x) { x.reverse(); });
    arrOp('fill    ', function (x) { x.fill(7); });
    arrOp('len=1   ', function (x) { x.length = 1; });
    arrOp('len=5   ', function (x) { x.length = 5; });
    arrOp('idx     ', function (x) { x[0] = 7; });
    arrOp('idx-grow', function (x) { x[5] = 7; });
    arrOp('delete  ', function (x) { delete x[0]; });
    arrOp('concat  ', function (x) { x.concat([1]); });
    arrOp('slice   ', function (x) { x.slice(1); });
    arrOp('map     ', function (x) { x.map(function (v) { return v; }); });
    arrOp('indexOf ', function (x) { x.indexOf(1); });
    arrOp('iterate ', function (x) { var s = 0; for (var v of x) s += v; });

    // --- a wrapper that OBSERVES defineProperty, which is the whole reason a
    // wrapper installs the trap in the first place ------------------------
    t('observed-push-sees-writes', function () {
        var seen = [];
        var a = [];
        var p = new Proxy(a, {
            defineProperty: function (tt, k, d) { seen.push(String(k)); return Reflect.defineProperty(tt, k, d); }
        });
        p.push('x');
        return a.join(',') + ' saw=[' + seen.join(',') + ']';
    });
    t('observed-index-write-sees-writes', function () {
        var seen = [];
        var a = [0];
        var p = new Proxy(a, {
            defineProperty: function (tt, k, d) { seen.push(String(k)); return Reflect.defineProperty(tt, k, d); }
        });
        p[0] = 'x';
        return a.join(',') + ' saw=[' + seen.join(',') + ']';
    });
    // a trap that REFUSES must make the operation fail, in both modes
    t('refusing-dp-trap-strict', function () {
        var p = new Proxy([1], { defineProperty: function () { return false; } });
        p.push(2);
        return 'no-throw';
    });
    t('refusing-dp-trap-Reflect', function () {
        return Reflect.defineProperty(new Proxy({}, { defineProperty: function () { return false; } }), 'x', { value: 1 });
    });

    // --- the same identity for the OTHER trap that reads back the target ---
    t('fwd-gopd-identity', function () {
        var a = [1, 2];
        var p = new Proxy(a, { getOwnPropertyDescriptor: function (tt, k) { return Reflect.getOwnPropertyDescriptor(tt, k); } });
        return JSON.stringify(Object.getOwnPropertyDescriptor(p, 'length')) + '/' + JSON.stringify(Object.getOwnPropertyDescriptor(p, 0));
    });
    t('fwd-set-identity-nc-w', function () {
        var o = withAttrs({ value: 1, writable: true, configurable: false, enumerable: true });
        var p = new Proxy(o, { set: function (tt, k, v, r) { return Reflect.set(tt, k, v, r); } });
        p.p = 5;
        return o.p;
    });
    t('fwd-deleteProperty-identity', function () {
        var a = [1, 2];
        var p = new Proxy(a, { deleteProperty: function (tt, k) { return Reflect.deleteProperty(tt, k); } });
        delete p[0];
        return String(a[0]) + '/' + a.length;
    });
})();
