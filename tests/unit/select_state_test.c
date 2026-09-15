#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"
#include "js_page.h"
#include "forms.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* Link stubs, the same set tests/unit/wpt_test.c carries and for the same
 * reason: this links the SHIPPING browser files (js_page/js_dom/js_webapi/
 * js_platform/js_select/js_module), and two of them reach for the fetcher and
 * the image registry, neither of which exists off the machine. A runner over
 * stubbed DOM files would measure the stubs; stubbing the network does not. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }
/* The module installer is linked but no case imports a module. Its browser_rt
 * fetch hooks must still resolve at link time; report not found if called. */
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
int  res_fetch(const char *src, unsigned char **buf, int *len)
{ (void)src; (void)buf; (void)len; return -1; }


static int checks, failures;
static void ck(const char *name, const char *src)
{
    checks++;
    JSContext *ctx = js_page_ctx();
    JSValue v = JS_Eval(ctx, src, strlen(src), "<dom-id>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (!ok) {
        printf("FAIL %s", name);
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, e);
            printf(": %s", s ? s : "exception");
            JS_FreeCString(ctx, s); JS_FreeValue(ctx, e);
        }
        puts(""); failures++;
    }
    JS_FreeValue(ctx, v);
}

/* A script choosing the option and the native painter choosing its label must
 * read one state. JS-only assertions all passed while Python's switcher said
 * English in JS but showed the first language, Greek, in the control. */
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px/2); }
static void paint_label(const char *name, struct node *select, const char *want)
{
    struct fpaint p={0}; checks++;
    if (!fc_paint_state(select,16,0,300,&p) || !p.text ||
        p.len != (int)strlen(want) || memcmp(p.text,want,p.len)) {
        printf("FAIL %s: native index=%d label=%.*s expected=%s\n",name,
               fc_selected_index(select),p.len,p.text ? p.text : "",want);
        failures++;
    }
}
int main(void)
{
    const char *html="<!doctype html><html><body><form id='f'><select id='s'>"
       "<option value='el'>Greek</option><option value='en' selected>English</option>"
       "</select></form></body></html>";
    struct node *root=dom_parse(html,strlen(html));
    if (!root) return 2;
    js_page_set_location("http://example.com/");
    if (!js_page_open(root)) { dom_free(root); return 2; }
    js_page_eval("void 0;",7,"<warmup>",0);
    struct node *select=dom_get_element_by_id(root->doc,"s");
    ck("initial selected attribute", "var s=document.getElementById('s');s.value==='en' && s.selectedIndex===1");
    paint_label("initial native label",select,"English");
    js_dom_clear_dirty();
    ck("script value", "s.value='el';s.value==='el' && s.options[0].selected && !s.options[1].selected");
    checks++;
    if (js_dom_inval_level() != INVAL_PAINT) { puts("FAIL script selection requests repaint"); failures++; }
    paint_label("script value reaches painter",select,"Greek");
    fc_set_selected_index(select,1); /* the dropdown's native selection path */
    ck("native selection reaches JS", "s.value==='en' && s.selectedIndex===1 && s.options[1].selected");
    ck("script index", "s.selectedIndex=0;s.value==='el'");
    paint_label("script index reaches painter",select,"Greek");
    ck("script option", "s.options[1].selected=true;s.value==='en'");
    paint_label("script option reaches painter",select,"English");
    ck("option reorder preserves selected identity", "var selected=s.options[1];s.insertBefore(selected,s.firstChild);s.value==='en' && s.selectedIndex===0");
    paint_label("reordered option native label",select,"English");
    ck("restore option order", "s.appendChild(selected);s.selectedIndex===1");
    ck("unselected option false preserves selection", "s.options[0].selected=false;s.selectedIndex===1");
    ck("selection does not change markup default", "s.options[1].hasAttribute('selected') && !s.options[0].hasAttribute('selected')");
    ck("missing value deselects", "s.value='absent';s.value==='' && s.selectedIndex===-1");
    paint_label("missing value clears native label",select,"");
    ck("out of range index deselects", "s.selectedIndex=99;s.value==='' && s.selectedIndex===-1");
    ck("negative index deselects", "s.selectedIndex=-5;s.value==='' && s.selectedIndex===-1");
    fc_set_selected_index(select,99);
    paint_label("native oversized index clears label",select,"");
    fc_set_selected_index(select,-5);
    paint_label("native negative index clears label",select,"");
    ck("form reset default", "document.getElementById('f').reset();s.value==='en' && s.selectedIndex===1");
    paint_label("form reset native label",select,"English");
    ck("reset button resets live selection", "s.value='el';var rb=document.createElement('button');rb.type='reset';document.getElementById('f').appendChild(rb);rb.click();s.value==='en' && s.selectedIndex===1");
    paint_label("reset button native label",select,"English");
    ck("detached switcher selection", "var detached=document.createElement('select');"
       "detached.innerHTML='<option value=dev>Development</option><option value=stable>Stable</option>';"
       "detached.value='stable';detached.value==='stable'");
    ck("attach selected switcher", "detached.id='detached';document.body.appendChild(detached);detached.value==='stable'");
    struct node *detached=dom_get_element_by_id(root->doc,"detached");
    paint_label("detached selection reaches painter",detached,"Stable");
    ck("orphan option selected before insertion", "var orphan=document.createElement('option');orphan.value='chosen';orphan.textContent='Chosen';orphan.selected=true;"
       "var os=document.createElement('select');os.id='orphan-select';os.innerHTML='<option>Default</option>';os.appendChild(orphan);document.body.appendChild(os);true");
    paint_label("orphan option native label before JS read",dom_get_element_by_id(root->doc,"orphan-select"),"Chosen");
    ck("orphan selection reaches JS", "os.value==='chosen' && os.selectedIndex===1");
    ck("default skips disabled option", "var ds=document.createElement('select');ds.innerHTML='<option disabled>Disabled</option><option>Enabled</option>';ds.value==='Enabled' && ds.selectedIndex===1");
    ck("explicit size one still selects first", "ds.setAttribute('size','1');ds.selectedIndex===1");
    ck("multiple default attributes resolve consistently", "var ms=document.createElement('select');ms.innerHTML='<option selected>A</option><option selected>B</option>';"
       "ms.selectedIndex===1 && !ms.options[0].selected && ms.options[1].selected");
    ck("multiple selection stays independent", "var multi=document.createElement('select');multi.multiple=true;"
       "multi.setAttribute('multiple','');multi.innerHTML='<option value=a>A</option><option value=b>B</option>';"
       "document.body.appendChild(multi);multi.options[0].selected=true;multi.options[1].selected=true;"
       "multi.selectedOptions.length===2");
    ck("multiple reports first selected index", "multi.selectedIndex===0 && multi.value==='a'");
    ck("multiple to single has one native state", "multi.removeAttribute('multiple');multi.selectedIndex===1 && multi.value==='b' && !multi.options[0].selected && multi.options[1].selected");
    ck("single to multiple does not revive deselected option", "multi.setAttribute('multiple','');multi.selectedOptions.length===1 && multi.selectedIndex===1");
    ck("form reset clears multiple current state", "document.getElementById('f').appendChild(multi);document.getElementById('f').reset();multi.selectedOptions.length===0 && multi.selectedIndex===-1");
    js_page_close(); fc_reset(); dom_free(root);
    printf("select-state: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
