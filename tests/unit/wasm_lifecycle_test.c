/* Exercise the SHIPPING page lifecycle, not install/reset calls from a test.
 * wasm_js_test already called reset by hand, so it could not catch the missing
 * production close hook. A child process contains the old FreeRuntime abort;
 * the parent reports it as a failed lifecycle case, never a passing skip. */
#define main dom_iface_fixture_main
#include "dom_iface_test.c"
#undef main
#include "wasm_js_modules.inc"
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void module_bytes(const char *name, const unsigned char *bytes, size_t n)
{
    JSValue g = JS_GetGlobalObject(g_ctx);
    JSValue ab = JS_NewArrayBufferCopy(g_ctx, bytes, n);
    JSValue ctor = JS_GetPropertyStr(g_ctx, g, "Uint8Array");
    JSValue u8 = JS_CallConstructor(g_ctx, ctor, 1, (JSValueConst *)&ab);
    JS_SetPropertyStr(g_ctx, g, name, u8);
    JS_FreeValue(g_ctx, ctor); JS_FreeValue(g_ctx, ab); JS_FreeValue(g_ctx, g);
}

static struct node *open_page(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root || !js_page_open(root)) exit(2);
    g_ctx = js_page_ctx();
    js_page_eval("void 0", 6, "<warmup>", 0);
    module_bytes("addBytes", W_add, sizeof W_add);
    module_bytes("importBytes", W_imp, sizeof W_imp);
    return root;
}

static int reopen_case(void)
{
    struct node *root = open_page();
    ckjs("new WebAssembly.Instance(new WebAssembly.Module(addBytes)).exports.add(20,22)===42",
         "first page executes Wasm");
    js_page_close(); dom_free(root);
    root = open_page();
    ckjs("new WebAssembly.Instance(new WebAssembly.Module(addBytes)).exports.add(20,22)===42",
         "second page executes Wasm");
    /* 160 is above the native pool's 128 slots. Unreachable modules must
     * finalize on the SECOND runtime, not just on page one. Use modules here:
     * instance exports also exercise WeakMap ephemeron cycles, a separate GC
     * contract that must not substitute for the class-registration check. */
    int i;
    for (i = 0; i < 160; i++) {
        const char *src = "WebAssembly.Module.exports(new WebAssembly.Module(addBytes))[0].name==='add'";
        JSValue v = JS_Eval(g_ctx, src, strlen(src), "<wasm-reopen>", JS_EVAL_TYPE_GLOBAL);
        int ok = !JS_IsException(v) && JS_ToBool(g_ctx, v);
        JS_FreeValue(g_ctx, v);
        if (!ok) { printf("FAIL second runtime Wasm reclamation at allocation %d\n", i); return 1; }
        JS_RunGC(JS_GetRuntime(g_ctx));
    }
    puts("ok: second runtime reclaimed 160 compiled modules");
    js_page_close(); dom_free(root);
    return fails != 0;
}

static int cycle_case(void)
{
    struct node *root = open_page();
    ckjs("globalThis.keep=(function(){var instance; function twice(x){return instance?x*2:0;}"
         "instance=new WebAssembly.Instance(new WebAssembly.Module(importBytes),{env:{twice:twice}});"
         "return instance;})();keep.exports.call4()===8",
         "imported callback captures its live Wasm instance");
    ckjs("globalThis.memory=new WebAssembly.Memory({initial:1});"
         "globalThis.view=new Uint8Array(memory.buffer);view[0]=37;view[0]===37",
         "page retains native memory ArrayBuffer");
    puts("WASM-LIFECYCLE closing retained import cycle and memory");
    js_page_close(); dom_free(root);
    root = open_page();
    ckjs("new WebAssembly.Instance(new WebAssembly.Module(addBytes)).exports.add(4,5)===9",
         "fresh page executes after retained resources close");
    js_page_close(); dom_free(root);
    return fails != 0;
}

int main(void)
{
    struct rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;
    for (int test = 0; test < 2; test++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 2; }
        if (!pid) _exit(test ? cycle_case() : reopen_case());
        int status;
        if (waitpid(pid, &status, 0) != pid) return 2;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            printf("FAIL WASM-LIFECYCLE %s (exit=%d signal=%d)\n",
                   test ? "close releases retained resources" : "reopen registers native class",
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                   WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            failed++;
        }
    }
    printf("WASM-LIFECYCLE: 2 cases, %d failed\n", failed);
    return failed != 0;
}
