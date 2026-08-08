/* js_syntax_test -- the language the vendored QuickJS actually accepts.
 *
 * WHY: the browser's failure mode on a real page is not "slow", it is "this
 * 42 KB polyfill produced SyntaxError and every feature it was going to install
 * is silently missing". A compatibility table cannot tell you that; only
 * compiling the bytes a real site serves can.
 *
 * Two halves:
 *
 *  1. LITERALS AND SYNTAX, case by case, each with an expected verdict. The
 *     decisive group is member access on a NON-DECIMAL numeric literal --
 *     `0xde0b6b3a7640080.toFixed(0)`. This is legal JavaScript (a
 *     HexIntegerLiteral has no fractional part, so the `.` can only be a
 *     property access) and stock QuickJS 2024-01-13 rejects it, because
 *     js_atof() eats the `.` as the start of a hex FLOAT -- a thing that
 *     exists in C99 and in QuickJS's own BigFloat mode but not in JavaScript --
 *     and then fails the `is_float && radix != 10` check. See the LOGIT PATCH
 *     comment in third_party/quickjs/quickjs.c.
 *
 *     NEGATIVE CONTROL: revert that patch and the four "hex/oct/bin literal
 *     then property access" cases below fail, and so does the real-page case
 *     in part 2. Nothing else in this file changes.
 *
 *     The patch must not simply accept more: `0x1.8p3` (a real C hex float) is
 *     still required to be a SyntaxError, and is checked here.
 *
 *  2. THE REAL PAGE. tests/fixtures/jsperf/baidu-polyfill.js is the file every
 *     baidu.com search page loads. It is here as bytes, not as a reduced test
 *     case, because the reduction is only convincing if the original also
 *     passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static int checks, failed;

static void fail(const char *what, const char *detail)
{
    failed++;
    printf("FAIL: %s\n      %s\n", what, detail ? detail : "");
}

/* Compile `src`. Returns 1 on success, 0 on SyntaxError; on failure copies the
 * message into `msg`. */
static int try_compile(JSContext *ctx, const char *src, char *msg, size_t msgn)
{
    if (msg) msg[0] = 0;
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>",
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (msg) snprintf(msg, msgn, "%s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return 0;
    }
    JS_FreeValue(ctx, v);
    return 1;
}

static void expect_compiles(JSContext *ctx, const char *label, const char *src)
{
    char msg[256];
    checks++;
    if (!try_compile(ctx, src, msg, sizeof msg)) {
        char d[512];
        snprintf(d, sizeof d, "`%s` should compile, got: %s", src, msg);
        fail(label, d);
    }
}

static void expect_syntax_error(JSContext *ctx, const char *label, const char *src)
{
    checks++;
    if (try_compile(ctx, src, NULL, 0)) {
        char d[512];
        snprintf(d, sizeof d, "`%s` should be a SyntaxError, but it compiled", src);
        fail(label, d);
    }
}

/* Evaluate an expression and require it to equal `want` as a string. Catches
 * the case where a literal parses but parses to the WRONG NUMBER, which a
 * compile-only check cannot see. */
static void expect_value(JSContext *ctx, const char *label,
                         const char *src, const char *want)
{
    checks++;
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        char d[512];
        snprintf(d, sizeof d, "`%s` threw: %s", src, m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        fail(label, d);
        return;
    }
    const char *got = JS_ToCString(ctx, v);
    if (!got || strcmp(got, want) != 0) {
        char d[512];
        snprintf(d, sizeof d, "`%s` = %s, expected %s", src, got ? got : "?", want);
        fail(label, d);
    }
    if (got) JS_FreeCString(ctx, got);
    JS_FreeValue(ctx, v);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return NULL; }
    b[n] = 0; *len = got;
    return b;
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* ---- property access on a non-decimal numeric literal ---------------
     * THE regression this file exists for. Each of these is legal JavaScript
     * and each one is what a real polyfill writes. */
    expect_compiles(ctx, "hex literal then property",
                    "0xde0b6b3a7640080.toFixed(0)");
    expect_compiles(ctx, "short hex literal then property",
                    "0x10.toString(16)");
    expect_compiles(ctx, "binary literal then property",
                    "0b1010.toString(2)");
    expect_compiles(ctx, "octal literal then property",
                    "0o17.toString(8)");
    /* and it has to parse to the right number, not merely parse */
    expect_value(ctx, "hex literal value survives",
                 "0x10.toString(16)", "10");
    expect_value(ctx, "big hex literal value survives",
                 "0xde0b6b3a7640080.toFixed(0)", "1000000000000000128");
    expect_value(ctx, "binary literal value survives",
                 "0b1010.toString(10)", "10");
    expect_value(ctx, "octal literal value survives",
                 "0o17.toString(10)", "15");
    expect_value(ctx, "hex literal then method chain",
                 "0xff.toString(2).length", "8");

    /* The patch must not have widened the grammar. JavaScript has no
     * non-decimal fractional literal at all -- these are C, not JS. */
    expect_syntax_error(ctx, "hex float rejected", "var x = 0x1.8p3;");
    expect_syntax_error(ctx, "hex fraction rejected", "var x = 0x1.8;");
    expect_syntax_error(ctx, "binary fraction rejected", "var x = 0b1.1;");
    expect_syntax_error(ctx, "octal fraction rejected", "var x = 0o1.1;");
    /* ...and Number()/parseFloat must give the same verdict at run time. */
    expect_value(ctx, "Number('0x1.8') is NaN", "String(Number('0x1.8'))", "NaN");
    expect_value(ctx, "Number('0x10') still 16", "String(Number('0x10'))", "16");
    expect_value(ctx, "parseInt hex still works", "String(parseInt('0x1f'))", "31");
    expect_value(ctx, "parseFloat decimal unaffected", "String(parseFloat('1.5e3'))", "1500");

    /* Decimal literals, which the patch must leave exactly as they were. */
    expect_value(ctx, "decimal fraction", "(1.255).toFixed(2)", "1.25");
    expect_value(ctx, "leading-dot literal then property", ".9.toFixed(0)", "1");
    expect_value(ctx, "exponent then property", "8e-5.toFixed(3)", "0.000");
    expect_value(ctx, "double dot", "5..toFixed(1)", "5.0");
    expect_value(ctx, "numeric separators", "String(1_000_000)", "1000000");
    expect_value(ctx, "hex bigint literal", "String(0xffn)", "255");
    expect_value(ctx, "legacy octal (sloppy)", "String(0755)", "493");
    expect_value(ctx, "non-octal decimal", "String(089)", "89");
    expect_syntax_error(ctx, "10instanceof still rejected", "0x10instanceof Number");

    /* ---- the ES features the real corpus actually uses ------------------
     * Not a compatibility table: every one of these appears in the bundles in
     * tests/fixtures/jsperf. */
    expect_compiles(ctx, "class static block", "class A{static{this.x=1}}");
    expect_compiles(ctx, "private methods", "class A{#m(){return 1};go(){return this.#m()}}");
    expect_compiles(ctx, "logical assignment", "var a=1;a??=2;a||=3;a&&=4;");
    expect_compiles(ctx, "optional chaining call", "var o={};o?.a?.b?.();");
    expect_compiles(ctx, "async generator", "async function* g(){yield 1}");
    expect_compiles(ctx, "for await", "async function f(){for await(const x of []){}}");
    expect_compiles(ctx, "regexp named group", "/(?<y>a)/.exec('a')");
    expect_compiles(ctx, "regexp lookbehind", "/(?<=a)b/.test('ab')");
    expect_compiles(ctx, "optional catch binding", "try{}catch{}");
    expect_compiles(ctx, "hashbang", "#!/usr/bin/env node\n1");
    expect_compiles(ctx, "dynamic import", "async function f(){await import('./x.js')}");

    /* ---- the real page ---------------------------------------------------
     * baidu.com's polyfill bundle, byte for byte. Before the lexer fix this
     * did not compile, so none of the polyfills in it were ever installed. */
    const char *poly = argc > 1 ? argv[1] : "tests/fixtures/jsperf/baidu-polyfill.js";
    size_t plen = 0;
    char *psrc = slurp(poly, &plen);
    checks++;
    if (!psrc) {
        fail("real page fixture", "could not read the baidu polyfill fixture");
    } else {
        char msg[256];
        if (!try_compile(ctx, psrc, msg, sizeof msg)) {
            char d[512];
            snprintf(d, sizeof d,
                     "%s (%lu bytes) does not compile: %s -- every polyfill in "
                     "it is missing from the page", poly, (unsigned long)plen, msg);
            fail("real page fixture", d);
        }
        free(psrc);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (failed) {
        printf("js_syntax_test: %d/%d checks FAILED\n", failed, checks);
        return 1;
    }
    printf("js_syntax_test: %d checks pass\n", checks);
    return 0;
}
