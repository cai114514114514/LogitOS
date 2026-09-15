/* Shipping JS focus hook after a real initial cascade. This reproduces the
 * synchronous mutation/focus path before the embedder's next paint turn. */
#define main select_original_main
#include "select_state_test.c"
#undef main
struct image;
void img_free(struct image *o){(void)o;}
int img_decode(const uint8_t *p,int n,struct image *o){(void)p;(void)n;(void)o;return -1;}
int main(void){
    const char *html="<!doctype html><html><body><button id=prior>Prior</button><div id=existing><button id=target>Target</button></div></body></html>";
    struct node *root=dom_parse(html,strlen(html));
    css_apply(root,".off{display:none}",18);
    js_page_set_location("http://example.com/");
    if(!js_page_open(root))return 2;
    js_page_eval("void 0",6,"warmup",0);
    ck("initial focus on styled page","var prior=document.getElementById('prior');prior.focus();document.activeElement===prior");
    ck("detached focus preserves active element and author sheet","document.createElement('button').focus();document.activeElement===prior");
    ck("new hidden ancestor blocks synchronous focus","var w=document.createElement('div');w.style.display='none';var b=document.createElement('button');w.appendChild(b);document.body.appendChild(w);b.focus();document.activeElement===prior");
    if(!js_dom_dirty()){puts("FAIL style flush preserves DOM dirty for embedder paint");failures++;}checks++;
    ck("show then focus works without an intervening geometry read","w.style.display='block';b.focus();document.activeElement===b");
    ck("hide again in same dirty episode blocks focus","prior.focus();w.style.display='none';b.focus();document.activeElement===prior");
    ck("show again in same dirty episode restores focusability","w.style.display='block';b.focus();document.activeElement===b");
    ck("existing ancestor class mutation blocks focus","prior.focus();document.getElementById('existing').className='off';document.getElementById('target').focus();document.activeElement===prior");
    ck("removing class allows focus synchronously","document.getElementById('existing').className='';document.getElementById('target').focus();document.activeElement.id==='target'");
    js_page_close();dom_free(root);
    printf("focus-style-flush: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
