/* weakmap_fin_test -- the js_map_finalizer reentrancy repro.
 *
 * The crash this gates (chat.deepseek.com, [fault] page fault rip in
 * js_map_get, cr2=0x10): a WeakMap dies while one record's VALUE is the only
 * strong reference to another record's KEY. js_map_finalizer() used to
 * interleave, per record, "unlink from the key's weak-ref list" and "free the
 * value" in ONE loop; the value free cascades through free_object ->
 * reset_weak_ref(key), which correctly unlinks AND FREES the later record of
 * the SAME map -- and the outer loop's pre-saved el1 then points into freed
 * memory. The next iteration reads mr->empty off a freed chunk, runs
 * weak_ref_unlink() against the already-dead key, and double-frees the record
 * chunk; the double-freed chunk is handed out twice, and when one of its two
 * owners is a live WeakMap's record the stale writes land in that map's
 * hash_link chain -- which is how js_map_get, pages later, walks into
 * rbx==NULL and reads 0x10(%rbx).
 *
 * The JS below builds the poison shape twice (object-keyed and symbol-keyed),
 * then hammers the allocator with same-size Map/WeakMap traffic so an aliased
 * chunk, if any, shows up as a round-trip mismatch rather than a crash the
 * harness cannot attribute.
 *
 * Expected result: exit 0, one PASS line. On the pre-fix engine the process
 * dies inside the first IIFE (assert in weak_ref_unlink, or the freed-memory
 * read) -- see test-weakmap-fin-control, which deletes the two-pass detach
 * and REQUIRES exactly that death.
 *
 * Host build only: the engine is the same quickjs.c the browser links; what
 * differs on the machine is the allocator underneath, and the defect being
 * gated (a reentrant free through a finalizer cascade) is allocator-blind --
 * it hands some allocator the same chunk twice, and every allocator this tree
 * runs on turns that into corruption eventually.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst t, int argc,
                        JSValueConst *argv)
{
    int i;
    (void)t;
    for (i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s)
            return JS_EXCEPTION;
        fputs(s, stdout);
        if (i + 1 < argc)
            fputc(' ', stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}

static const char *poison_object_keys =
    "(function () {\n"
    "    var a = {}, b = {};\n"
    "    var w = new WeakMap();\n"
    "    w.set(a, b);   /* rec1: key a (weak), value b (the ONLY strong ref to b) */\n"
    "    w.set(b, a);   /* rec2: key b (weak), value a (the ONLY strong ref to a) */\n"
    "    a = null; b = null;\n"
    "    /* w must die HERE, not at frame teardown: quickjs frees the map\n"
    "       before the null'd locals on return, so b would still hold a\n"
    "       stack ref and the cascade would not fire (found by tracing --\n"
    "       see the trace note in weakmap_fin_test.c's header) */\n"
    "    w = null;\n"
    "})();";

static const char *poison_symbol_key =
    "(function () {\n"
    "    var s2 = Symbol('y');\n"
    "    var a = {};\n"
    "    var w = new WeakMap();\n"
    "    w.set(s2, a);   /* rec1: SYMBOL key (dup'd ref), value a -- the only\n"
    "                       strong ref to a once the local is nulled */\n"
    "    w.set(a, s2);   /* rec2: object key a, still on a's weak-ref list */\n"
    "    a = null;\n"
    "    w = null;   /* finalizer frees rec1's value (a) FIRST; a dies, its\n"
    "                   weak-ref list still names rec2 -> rec2 freed under the\n"
    "                   loop (the dangling-el1 shape), via the symbol path */\n"
    "})();";

static const char *poison_finreg_unregister =
    "(function () {\n"
    "    var tk = {}, T = {};\n"
    "    var reg = new FinalizationRegistry(function () {});\n"
    "    var H = {};\n"
    "    reg.register(T, H, tk);  /* cell1: token=tk, held=H (strong). T is kept\n"
    "                                alive so cell1 is not reclaimed before the\n"
    "                                unregister below (a throwaway target dies at\n"
    "                                once and takes its cell -- and held's\n"
    "                                reference -- with it). */\n"
    "    reg.register(H, 1);      /* cell2: target=H -- on H's weak-ref list,\n"
    "                                AFTER cell1 on the registry's cells */\n"
    "    H = null;                /* cell1's held value is now H's only ref */\n"
    "    reg.unregister(tk);      /* frees cell1's held (H) mid-loop; H dies\n"
    "                                synchronously, its weak-ref list still\n"
    "                                names cell2, which is unlinked AND freed\n"
    "                                under the loop -- the saved el1 dangles */\n"
    "    T = null;\n"
    "})();";

static const char *churn =
    "for (var i = 0; i < 5000; i++) {\n"
    "    var m = new Map();\n"
    "    var k = { i: i };\n"
    "    m.set(k, i);\n"
    "    if (m.get(k) !== i) throw new Error('strong map round-trip broken at ' + i);\n"
    "    var wm = new WeakMap();\n"
    "    var k2 = {};\n"
    "    wm.set(k2, i);\n"
    "    if (!wm.has(k2) || wm.get(k2) !== i)\n"
    "        throw new Error('weak map round-trip broken at ' + i);\n"
    "}\n"
    "var big = new Map();\n"
    "for (var i = 0; i < 2000; i++) big.set({ k: i }, i);\n"
    "if (big.size !== 2000) throw new Error('size drift: ' + big.size);\n"
    "big.forEach(function (v, kk) { if (big.get(kk) !== v) throw new Error('forEach mismatch'); });\n"
    "big.clear();\n"
    "if (big.size !== 0) throw new Error('clear broken');\n"
    "print('PASS weakmap-finalizer: survived finalizer cascade + churn');";

static int run(JSContext *ctx, const char *src, const char *name)
{
    JSValue r = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("FAIL %s: %s\n", name, s ? s : "<no message>");
        JS_FreeCString(ctx, s);
        /* the stack, when there is one, names where it died */
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsException(st) && !JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) {
                printf("  at %s\n", ss);
                JS_FreeCString(ctx, ss);
            }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, r);
        return 1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    int bad = 0;

    JS_SetContextOpaque(ctx, NULL);
    {
        JSValue glob = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, glob, "print",
                          JS_NewCFunction(ctx, js_print, "print", 1));
        JS_FreeValue(ctx, glob);
    }

    /* each poison snippet kills its map inside JS_Eval; the GC pass after it
       mops up anything the cascade left half-dead, so a stale pointer that
       survived the first free still gets dereferenced here on the host */
    bad |= run(ctx, poison_object_keys, "poison-object-keys.js");
    JS_RunGC(rt);
    bad |= run(ctx, poison_symbol_key, "poison-symbol-key.js");
    JS_RunGC(rt);
    bad |= run(ctx, poison_finreg_unregister, "poison-finreg-unregister.js");
    JS_RunGC(rt);
    bad |= run(ctx, churn, "churn.js");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt); /* runs every finalizer still pending */
    if (bad) {
        printf("FAIL weakmap-finalizer: at least one snippet failed\n");
        return 1;
    }
    return 0;
}
