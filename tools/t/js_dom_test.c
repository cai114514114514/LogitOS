/* Host test for the JS<->DOM bindings (user/js_dom.c).
 * Parses HTML, runs a script that reads/writes the DOM via document/Element,
 * and checks the live DOM changed + the dirty flag fired. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static struct node *find(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && !strcmp(n->tag, tag)) return n;
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find(c, tag); if (r) return r; }
    return 0;
}
static const char *firsttext(struct node *n)
{ for (struct node *c = n->first_child; c; c = c->next) if (c->type == N_TEXT) return c->text; return ""; }

static int fails;
#define CK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails = 1; } else printf("ok: %s\n", m); } while (0)

static int run(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v);
    if (!ok) { JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
               printf("  JS exception: %s\n", m ? m : "?"); if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, v);
    return ok;
}

int main(void)
{
    const char *html = "<body><h1 id='t'>Old</h1><p class='x'>hi</p><a href='/y'>z</a></body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_dom_init(ctx, root);

    CK(run(ctx, "document.getElementById('t').textContent = 'New' + document.querySelector('.x').tagName;"),
       "eval getElementById/querySelector/textContent=");
    CK(js_dom_dirty(), "DOM marked dirty after textContent set");
    struct node *h1 = find(root, "h1");
    CK(h1 && !strcmp(firsttext(h1), "Newp"), "h1 textContent updated to 'Newp'");

    js_dom_clear_dirty();
    CK(run(ctx, "var a = document.querySelector('a'); a.setAttribute('href','/changed'); a.getAttribute('href');"),
       "eval setAttribute/getAttribute");
    CK(js_dom_dirty(), "DOM dirty after setAttribute");
    struct node *a = find(root, "a");
    CK(a && !strcmp(dom_attr(a, "href") ? dom_attr(a, "href") : "", "/changed"), "a href updated to /changed");

    CK(run(ctx, "if (document.getElementById('nope') !== null) throw 'should be null';"),
       "getElementById(missing) returns null");

    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf(fails ? "\nJS-DOM TEST FAILED\n" : "\nJS-DOM TEST PASSED\n");
    return fails;
}
