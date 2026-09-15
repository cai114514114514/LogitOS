/* Same page installers as production, so a later installer can erase an
 * earlier API here just as it did in the actual browser. Network is unused. */
#define main select_original_main
#include "select_state_test.c"
#undef main
#include "js_cssom.h"
#include "js_webapi.h"
struct image;
void layout_page(struct node *, int);
void layout_free(void);
void img_free(struct image *o){(void)o;}
int img_decode(const uint8_t *p,int n,struct image *o){(void)p;(void)n;(void)o;return -1;}
static struct node *live_root;
static void reflow(void){
    css_apply(live_root,"body{margin:0}",14);
    layout_page(live_root,600);
    js_dom_clear_dirty();
}
static void viewport(int w,int h){css_viewport(w,h);js_webapi_set_viewport(w,h);}
int main(void){
    const char *html="<!doctype html><body><div id=x style='width:10px;height:10px'>X</div></body>";
    live_root=dom_parse(html,strlen(html));
    viewport(600,400);reflow();
    js_page_set_location("http://example.com/");
    if(!js_page_open(live_root))return 2;
    js_cssom_set_reflow(reflow);
    ck("live media initial","var m=matchMedia('(min-width: 700px)'),hits=0,old=0,chg=0;function fn(e){if(e.matches===m.matches)hits++}function legacy(){old++}m.addEventListener('change',fn);m.addListener(legacy);m.onchange=function(){chg++};!m.matches");
    viewport(800,400);
    ck("media listener deferred until pump","hits===0");
    js_webapi_pump(js_page_ctx());
    ck("live media updates after viewport","m.matches && hits===1 && old===1 && chg===1");
    viewport(800,400);js_webapi_pump(js_page_ctx());
    ck("same viewport causes no duplicate change","hits===1 && old===1 && chg===1");
    ck("remove listeners","m.removeEventListener('change',fn);m.removeListener(legacy);true");
    viewport(600,400);js_webapi_pump(js_page_ctx());
    ck("live media removal and onchange","!m.matches && hits===1 && old===1 && chg===2");
    /* Empty query lists mean all in LibCSS. The old scanner rejected them.
     * Compare with the evaluator deciding stylesheets; merely comparing an
     * unsupported MQ4 range made BOTH parsers say false and did not distinguish
     * them (caught by the first watched MEDIA_SCANNER control). */
    int expected=css_media_matches("",0);
    ck("live media shares cascade evaluator",expected?"matchMedia('').matches":"!matchMedia('').matches");
    ck("first dirty geometry flush","var x=document.getElementById('x');x.style.width='31px';x.offsetWidth===31");
    ck("second consecutive dirty geometry flush","x.style.width='47px';x.offsetWidth===47");
    js_cssom_set_reflow(NULL);js_page_close();layout_free();dom_free(live_root);
    printf("css-live-wiring: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
