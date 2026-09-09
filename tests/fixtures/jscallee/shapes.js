/* callee-shapes -- "not a function" has to name something, ON THE MACHINE.
 *
 * WHY THIS FILE EXISTS AND WHY IT IS NOT tests/unit/js_stack_test.c.
 * js_stack_test links $(QJS_SRC) for arm64/darwin against the system libc.
 * /bin/jssem links $(ENGINE_OBJ) -- the literal object files browser.elf
 * links, for x86_64-elf against mini-libc with -DLOGIT_OS. The message this
 * measures is built out of a bytecode scan and snprintf, and neither of those
 * is the same code on the two sides. This tree's rule 1 in one line: a survey
 * run in the probe measures the probe.
 *
 * The specimen was google.com/search, which caught a TypeError out of THIS
 * engine, URL-encoded our own message into its `sg_ss` self-report parameter
 * and navigated to the "unusual traffic" interstitial with it. The message it
 * carried named nothing: "not a function (the callee is a number)".
 *
 * Prints one line per shape and a machine-readable verdict at the end.
 */
var ok = 0, bad = 0;

function msg(f) {
    try { f(); } catch (e) { return e.message; }
    return "NO THROW";
}
function want(label, got, expect) {
    if (got === expect) { ok++; print("ok   " + label + " -> " + got); }
    else { bad++; print("FAIL " + label + "\n       got  " + got + "\n       want " + expect); }
}

/* ---- the shape the cached atom already named ---------------------------- */
want("method is a number",
     msg(function () { var o = { a: 1 }; o.a(); }),
     "a is not a function (it is the number 1)");
want("method is missing",
     msg(function () { ({}).nope(); }),
     "nope is not a function (it is undefined)");

/* ---- the eight that were unnamed until the bytecode was read ------------- */
want("local",
     msg(function () { var f = 1; f(); }),
     "f is not a function (it is the number 1)");
want("argument",
     msg(function () { (function (g) { g(); })(1); }),
     "g is not a function (it is the number 1)");
want("closure variable",
     msg(function () { var c = 1; (function () { c(); })(); }),
     "c is not a function (it is the number 1)");
want("global",
     msg(function () { globalThis.gnum = 1; gnum(); }),
     "gnum is not a function (it is the number 1)");
want("bundler's (0, o.a)()",
     msg(function () { var o = { a: 1 }; (0, o.a)(); }),
     "a is not a function (it is the number 1)");
want("chain names the last link",
     msg(function () { var o = { a: { b: 1 } }; o.a.b(); }),
     "b is not a function (it is the number 1)");
want("call with arguments",
     msg(function () { var f = 1; f(1, 2, 3); }),
     "f is not a function (it is the number 1)");
want("inside a branch",
     msg(function () { var o = { a: 1 }; if (o) { o.a(); } else { o.a(); } }),
     "a is not a function (it is the number 1)");
want("new on a non-function",
     msg(function () { var C = 1; new C(); }),
     "C is not a function (it is the number 1)");

/* ---- the value, because 0 and NaN and 1 are three different bugs -------- */
want("the number 0",
     msg(function () { var m = { q: 0 }, k = "q"; m[k](); }),
     "not a function (the callee is the number 0)");
want("NaN",
     msg(function () { var m = { q: NaN }, k = "q"; m[k](); }),
     "not a function (the callee is the number NaN)");
want("an object reports its class",
     msg(function () { var m = { q: [] }, k = "q"; m[k](); }),
     "not a function (the callee is an object (Array))");

/* A long string must not become the message: 2 MB of bundle text inside a
 * TypeError is a diagnostic that has turned into a denial of service. The
 * bound is a %.32s, and mini-libc's printf is not the host's -- which is the
 * whole reason this file runs in the guest. */
want("a long string callee is bounded",
     (function () {
         var m = { q: new Array(400).join("x") }, k = "q";
         var s = msg(function () { m[k](); });
         return (s.length < 100 && s.indexOf("\"...") > 0)
              ? "bounded" : "UNBOUNDED len=" + s.length;
     })(),
     "bounded");

/* ---- where the name is unrecoverable, say nothing -----------------------
 * Naming the WRONG property points the reader at a line that is fine, which
 * is worse than the bare message. These two are the guard on the whole scan. */
want("computed key names nothing",
     msg(function () { var o = { z: 1 }, k = "z"; o[k](); }),
     "not a function (the callee is the number 1)");
want("the result of a call names nothing",
     msg(function () { var o = { a: function () { return 1; } }; o.a()(); }),
     "not a function (the callee is the number 1)");
want("a computed call does not inherit the last method name",
     msg(function () { var p = { then: function () {} }; p.then();
                       var m = { q: 7 }, k = "q"; m[k](); }),
     "not a function (the callee is the number 7)");

/* ---- and a working call is untouched ------------------------------------ */
want("a call that works",
     (function () { var o = { a: function () { return 42; } };
                    return o.a() === 42 ? "42" : "BROKEN"; })(),
     "42");

print("CALLEE-OS-RESULT ok=" + ok + " fail=" + bad);
