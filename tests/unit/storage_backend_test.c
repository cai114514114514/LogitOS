/* Real JS Storage consumers, reusing the existing WebAPI host boundary.
 * The old backend keyed sessionStorage only by origin and used strlen for
 * JS strings. STORAGE_BEFORE lets the same fixture expose both old failures
 * before the new embedder API exists; it is never a product implementation. */
#define main webapi_existing_main
#include "webapi_test.c"
#undef main
#ifndef STORAGE_BEFORE
#include "storage_backend.h"
#endif

static void storage_page(unsigned long long id, const char *url)
{
#ifndef STORAGE_BEFORE
    js_webapi_set_storage_session(id);
#else
    (void)id;
#endif
    open_ctx(url);
}

int main(void)
{
    storage_page(101, "https://store.example/a");
    run("localStorage.clear();sessionStorage.clear();"
        "localStorage.setItem('shared','local');sessionStorage.setItem('private','one');"
        "localStorage.setItem('a\\0b','v\\0z');localStorage.setItem('a','plain');");
    ckjs("localStorage.getItem('a\\0b')==='v\\0z'", "embedded NUL values and keys survive");
    ckjs("localStorage.getItem('a')==='plain' && localStorage.length===3", "NUL key does not alias prefix");
    ckjs("localStorage.key(1)==='a\\0b'", "key enumeration preserves NUL");
    run("localStorage.removeItem('a\\0b');");
    ckjs("localStorage.getItem('a')==='plain'", "removing NUL key preserves prefix");
    ckjs("(function(){var e=new Error('coercion');try{localStorage.getItem({toString:function(){throw e;}});}catch(x){return x===e;}return false;})()", "key conversion exception propagates");
    close_ctx();

    storage_page(202, "https://store.example/b");
    ckjs("localStorage.getItem('shared')==='local'", "local storage shared by same origin");
    ckjs("sessionStorage.getItem('private')===null", "different tab has independent session storage");
    run("sessionStorage.setItem('private','two');");
    close_ctx();
    storage_page(101, "https://store.example/reload");
    ckjs("sessionStorage.getItem('private')==='one'", "same tab survives runtime recreation");
    close_ctx();
    storage_page(101, "http://store.example/");
    ckjs("localStorage.getItem('shared')===null && sessionStorage.getItem('private')===null", "scheme separates storage origin");
    close_ctx();
    storage_page(101, "https://store.example:8443/");
    ckjs("localStorage.getItem('shared')===null", "port separates storage origin");
    close_ctx();
#ifndef STORAGE_BEFORE
    js_webapi_drop_storage_session(101);
#endif
    storage_page(101, "https://store.example/new-tab");
    ckjs("sessionStorage.getItem('private')===null", "closed tab session is cleared before identity reuse");
    ckjs("localStorage.getItem('shared')==='local'", "tab close preserves local storage");
    close_ctx();
    storage_page(202, "https://store.example/");
    ckjs("sessionStorage.getItem('private')==='two'", "closing one tab preserves another session");
    run("localStorage.clear();localStorage.setItem('kept','old');var full='x'.repeat(262144);var quota='';"
        "try{localStorage.setItem('kept',full);}catch(e){quota=e.name;}");
    ckjs("quota==='QuotaExceededError'", "quota rejects with named DOM error");
    ckjs("localStorage.getItem('kept')==='old' && localStorage.length===1", "quota failure preserves previous value atomically");
    close_ctx();

#ifndef STORAGE_BEFORE
    struct storage_key key = { "https://allocation.example", STORAGE_LOCAL, 0 };
    size_t n = 0;
    ck(storage_backend_set(&key, "x", 1, "old", 3) == STORAGE_OK, "backend accepts explicit key");
    storage_backend_fail_alloc_after(0);
    ck(storage_backend_set(&key, "x", 1, "new", 3) == STORAGE_NOMEM, "allocation failure is explicit");
    storage_backend_fail_alloc_after(-1);
    const char *v = storage_backend_get(&key, "x", 1, &n);
    ck(v && n == 3 && !memcmp(v, "old", 3), "allocation failure leaves old bytes intact");
    storage_backend_clear(&key);
    for (int i = 0; i < STORAGE_AREA_LIMIT + 2; i++) {
        char org[64]; snprintf(org, sizeof org, "https://read%d.example", i);
        struct storage_key readkey = { org, STORAGE_LOCAL, 0 };
        ck(storage_backend_get(&readkey, "missing", 7, &n) == NULL,
           "read-only lookup does not reserve an area");
    }
    ck(storage_backend_set(&key, "after", 5, "reads", 5) == STORAGE_OK,
       "read-only origins do not exhaust writable areas");
#endif
    printf("storage backend: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
