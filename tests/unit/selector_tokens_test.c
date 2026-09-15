/* Count the actual tokenization operations in the shipping matcher. The
 * String hooks delegate unchanged and are restored before the mutation cases;
 * no elapsed host time is presented as guest browser speed. */
#define main dom_iface_existing_main
#include "dom_iface_test.c"
#undef main

int main(void)
{
    const char *html = "<!doctype html><html><body><article id='target' "
                       "class='one needle three four' data-v='ok'></article></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root || !js_page_open(root)) return 2;
    g_ctx = js_page_ctx();
    ckjs("var el=document.getElementById('target'), splitCount=0, foldCount=0;"
         "var oldSplit=String.prototype.split, oldReplace=String.prototype.replace;"
         "String.prototype.split=function(){if(String(this)==='one needle three four')splitCount++;"
         "return oldSplit.apply(this,arguments)};"
         "String.prototype.replace=function(){if(String(this)==='ARTICLE')foldCount++;"
         "return oldReplace.apply(this,arguments)};"
         "var matches=0;for(var i=0;i<100;i++)if(el.matches('article.needle[data-v=ok]'))matches++;"
         "matches===100", "repeated compound matches retain all answers");
    ckjs("splitCount===1", "unchanged class string is tokenized once");
    ckjs("foldCount===1", "unchanged HTML tag is folded once");
    ckjs("String.prototype.split=oldSplit;String.prototype.replace=oldReplace;"
         "el.className='other';!el.matches('.needle')&&el.matches('.other')",
         "class replacement is visible immediately");
    ckjs("el.className='one needle three four';el.matches('.needle')",
         "returning to a cached value preserves its tokens");
    ckjs("el.setAttribute('class','a\\tneedle\\nb\\fc\\rd e');el.matches('.needle')&&!el.matches('.need')",
         "all five ASCII separators retain exact token boundaries");
    ckjs("el.className='a\\u00a0needle';!el.matches('.needle')&&el.matches('.a\\u00a0needle')",
         "non ASCII whitespace stays inside the token");
    ckjs("el.className='needle\\u0000tail';!el.matches('.needle')",
         "embedded NUL does not truncate the class value");
    ckjs("el.className='__proto__ constructor';el.matches('.__proto__')&&el.matches('.constructor')",
         "cache keys cannot collide with object prototypes");
    ckjs("var count=0;for(var i=0;i<600;i++){el.className='unique'+i+' needle';"
         "if(el.matches('.needle'))count++;}count===600",
         "cache capacity never drops uncached matching results");
    ckjs("el.className=Array(600).join('x')+' needle';el.matches('.needle')",
         "oversize class strings still match through the uncached path");
    ckjs("el.setAttribute('data-v','\\u0130');!el.matches('[data-v=i i]')",
         "attribute case folding remains ASCII only");
    js_page_close(); dom_free(root);
    printf("selector-tokens: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
