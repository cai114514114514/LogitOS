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

    /* createElement + appendChild: a new node enters the live DOM and is findable */
    js_dom_clear_dirty();
    CK(run(ctx, "var d = document.createElement('DIV');"
                "d.setAttribute('id', 'new'); d.textContent = 'made';"
                "document.body.appendChild(d);"),
       "eval createElement/setAttribute/appendChild");
    CK(js_dom_dirty(), "DOM dirty after appendChild");
    CK(run(ctx, "if (document.getElementById('new').textContent !== 'made') throw 'not in DOM';"),
       "appended element reachable via getElementById");
    struct node *dv = find(root, "div");
    CK(dv && !strcmp(firsttext(dv), "made"), "div node really linked under body");

    /* appendChild moves (reparents) an existing node */
    CK(run(ctx, "document.querySelector('h1').appendChild(document.querySelector('a'));"),
       "eval appendChild reparent");
    struct node *h1b = find(root, "h1");
    CK(h1b && find(h1b, "a"), "a moved under h1");

    /* removeChild drops the subtree from the DOM */
    js_dom_clear_dirty();
    CK(run(ctx, "var b = document.body, p = document.querySelector('.x');"
                "b.removeChild(p);"),
       "eval removeChild");
    CK(js_dom_dirty(), "DOM dirty after removeChild");
    CK(!find(root, "p"), "p node gone from the tree");
    CK(run(ctx, "if (document.querySelector('.x') !== null) throw 'should be gone';"),
       "querySelector no longer finds removed node");

    /* classList add/remove/toggle/contains */
    js_dom_clear_dirty();
    CK(run(ctx, "var cl = document.querySelector('h1').classList;"
                "cl.add('big');"
                "if (!cl.contains('big')) throw 'add failed';"
                "cl.toggle('on'); cl.toggle('big');"
                "if (cl.contains('big')) throw 'toggle off failed';"
                "if (!cl.contains('on')) throw 'toggle on failed';"
                "cl.remove('on');"
                "if (cl.contains('on')) throw 'remove failed';"),
       "classList add/remove/toggle/contains");
    CK(js_dom_dirty(), "DOM dirty after classList ops");
    CK(!strcmp(dom_attr(h1b, "class") ? dom_attr(h1b, "class") : "", ""),
       "h1 class attribute back to empty");

    /* addEventListener: recorded, does not throw */
    CK(run(ctx, "document.body.addEventListener('click', function() {});"
                "document.body.addEventListener('keydown', function() {});"),
       "addEventListener does not throw");
    CK(js_dom_listener_count() == 2, "two listeners recorded");

    /* document.documentElement exists (no <html> in fixture -> first element) */
    CK(run(ctx, "if (document.documentElement === null) throw 'no documentElement';"),
       "document.documentElement is non-null");

    /* console.log/warn/error are installed and callable */
    CK(run(ctx, "console.log('L', 1); console.warn('W'); console.error('E');"),
       "console.log/warn/error callable");

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf(fails ? "\nJS-DOM TEST FAILED\n" : "\nJS-DOM TEST PASSED\n");
    return fails;
}
