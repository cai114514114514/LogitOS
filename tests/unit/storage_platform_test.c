/* Full platform installer/proxy/DOMException consumer; no substitute class. */
#define main platform_existing_main
#include "webapi_platform_test.c"
#undef main
#include "js_webapi.h"

int main(void)
{
    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) return 1;
    js_page_set_clock(clock_fn);
    js_page_set_location("https://storage-platform.example/");
    js_webapi_set_storage_session(700);
    if (!js_page_open(root)) { dom_free(root); return 1; }
    ctx = js_page_ctx();
    run("localStorage.clear();localStorage['x\\0y']='z\\0w';");
    ckjs("localStorage['x\\0y']==='z\\0w'", "named Storage proxy consumes byte-preserving backend");
    ckjs("(function(){try{localStorage.setItem('k','x'.repeat(262144));}catch(e){return e instanceof DOMException && e.name==='QuotaExceededError' && e.code===22;}return false;})()",
         "storage quota is a real DOMException");
    ckjs("localStorage.getItem('x\\0y')==='z\\0w'", "failed named storage update preserves content");
    js_page_close(); ctx = NULL; dom_free(root);
    printf("storage platform: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
