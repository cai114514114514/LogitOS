/* Aqua OS — JavaScript app: embeds QuickJS (ring-3) and evaluates scripts,
 * showing console output + the last expression's value in a window. */
#include "aqua.h"
#include "quickjs.h"

int   printf(const char *, ...);
char *strcpy(char *, const char *);
unsigned long strlen(const char *);

/* accumulated console/result text shown in the window */
static char screen[8192];
static int  slen;
static void emit(const char *s)
{
    while (*s && slen < (int)sizeof screen - 1) screen[slen++] = *s++;
    screen[slen] = 0;
}

/* globalThis.print / console.log -> window + serial */
static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) emit(" ");
        emit(s); printf("%s ", s);
        JS_FreeCString(ctx, s);
    }
    emit("\n"); printf("\n");
    return JS_UNDEFINED;
}

static const char *SCRIPT =
    "function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }\n"
    "print('fib(20) =', fib(20));\n"
    "print('squares =', [1,2,3,4,5].map(function(x){return x*x;}).join(','));\n"
    "print('Math.sqrt(2) =', Math.sqrt(2));\n"
    "print('Math.PI =', Math.PI);\n"
    "print('json =', JSON.stringify({a:1,b:[true,null,'hi']}));\n"
    "var s=0; for(var i=1;i<=100;i++) s+=i;\n"
    "'sum 1..100 = ' + s;\n";

static void run_js(void)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { emit("JS_NewRuntime failed\n"); return; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { emit("JS_NewContext failed\n"); return; }

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1));
    JS_SetPropertyStr(ctx, g, "console", console);
    JS_FreeValue(ctx, g);

    JSValue v = JS_Eval(ctx, SCRIPT, strlen(SCRIPT), "<aqua>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        emit("Exception: "); emit(msg ? msg : "?"); emit("\n");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    } else {
        const char *s = JS_ToCString(ctx, v);
        emit("=> "); emit(s ? s : "undefined"); emit("\n");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

#define WINW 640
#define WINH 420

static void redraw(void)
{
    gui_clear(rgb(250, 250, 252));
    gui_rect(0, 0, WINW, 28, rgb(40, 44, 52));
    gui_text(12, 6, rgb(235, 235, 240), "JavaScript (QuickJS)");
    int y = 40; char line[256]; int li = 0;
    for (int i = 0; ; i++) {
        char c = screen[i];
        if (c == '\n' || c == 0) {
            line[li] = 0; gui_text(12, y, rgb(30, 30, 36), line); y += 20; li = 0;
            if (!c) break;
        } else if (li < 255) line[li++] = c;
    }
    gui_flush();
}

void app_main(void)
{
    gui_create("JavaScript", WINW, WINH);
    emit("Evaluating script...\n");
    run_js();
    redraw();
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) { if (e.type == EV_CLOSE) app_exit(0); }
        redraw();
        sys_yield();
    }
}
