/* A style assignment must change the actual cascade/geometry. Prototype
 * wrapping is a real consumer of the exposed declaration interface. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
int main(void)
{
    css_init();css_viewport(800,600);js_page_set_clock(clk);
    for(int round=0;round<2;round++){
        const char *html="<body><div id=probe style='width:40px;height:20px'>sample</div></body>";
        g_root=dom_parse(html,(int)strlen(html));g_sheetlen=0;reflow();
        js_cssom_set_reflow(reflow);if(!js_page_open(g_root))return 2;ctx=js_page_ctx();
        CK(eq("typeof CSSStyleDeclaration","function"),"STYLE interface exists");
        CK(eq("var e=document.getElementById('probe');var held=e.style;"
              "held instanceof CSSStyleDeclaration && getComputedStyle(e) instanceof CSSStyleDeclaration","true"),"inline and computed declarations use the real interface");
        CK(eq("Object.getPrototypeOf(held)===CSSStyleDeclaration.prototype && held.constructor===CSSStyleDeclaration","true"),"exposed prototype is the live declaration prototype");
        CK(eq("Object.prototype.toString.call(held)","[object CSSStyleDeclaration]"),"declaration has its interface tag");
        CK(eq("(function(){try{new CSSStyleDeclaration();return false}catch(e){return e instanceof TypeError}})()","true"),"interface cannot fabricate a detached declaration");
        CK(eq("(function(){'use strict';e.style='width:137px;height:23px';return e.offsetWidth})()","137"),"STYLE assignment updates measured geometry");
        CK(eq("held.width==='137px' && e.offsetHeight===23 && e.getAttribute('style').indexOf('137px')>=0","true"),"held declaration and DOM attribute observe forwarded write");
        CK(eq("(function(){var d=Object.getOwnPropertyDescriptor(CSSStyleDeclaration.prototype,'cssText'),calls=0;"
              "Object.defineProperty(CSSStyleDeclaration.prototype,'cssText',{configurable:true,get:d.get,set:function(v){calls++;return d.set.call(this,v)}});"
              "e.style='width:91px';Object.defineProperty(CSSStyleDeclaration.prototype,'cssText',d);return JSON.stringify([calls,e.offsetWidth,e.style.cssText])})()","[1,91,\"width:91px\"]"),"PutForwards uses a wrapped cssText setter and real layout");
        CK(eq("(function(){'use strict';try{e.style={toString:function(){throw new Error('conversion')}};return false}catch(x){return x.message==='conversion'&&e.offsetWidth===91}})()","true"),"failed string conversion preserves previous style and throws");
        CK(eq("e.style='';e.style.cssText",""),"empty style assignment clears declarations");
        js_page_close();layout_free();dom_free(g_root);
    }
    printf("style-forward: %d checks, %s\n",checks,fails?"FAIL":"PASS");return fails;
}
