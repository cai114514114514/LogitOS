/* The interface is useful only if its instances still change the cascade.
 * Reuse the CSSOM host's DOM/QuickJS/layout apparatus, but recollect style
 * text on every reflow: the older fixture's frozen g_sheet cannot observe
 * insertRule or an appended fallback style and would test yesterday's CSS. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main

static void collect_sheet_text(struct node *n, int in_style)
{
    for (; n; n = n->next) {
        int style = in_style || (n->type == N_ELEM && !strcmp(n->tag, "style"));
        if (style && n->type == N_TEXT && n->text &&
            g_sheetlen + n->textlen + 1 < (int)sizeof g_sheet) {
            memcpy(g_sheet + g_sheetlen, n->text, (size_t)n->textlen);
            g_sheetlen += n->textlen;
            g_sheet[g_sheetlen++] = '\n';
        }
        collect_sheet_text(n->first_child, style);
    }
}

static void sheet_reflow(void)
{
    g_sheetlen = 0;
    collect_sheet_text(g_root, 0);
    g_sheet[g_sheetlen] = 0;
    css_apply(g_root, g_sheet, g_sheetlen);
    layout_page(g_root, 800);
    js_dom_clear_dirty(); /* the embedder consumes the reflow's invalidation */
}

static void test_sheet_interface(void)
{
    CK(eq("typeof CSSStyleSheet", "function"), "CSS-SHEET interface exists");
    CK(eq("typeof StyleSheet", "function"), "base stylesheet interface exists");
    CK(eq("var sheets=document.styleSheets, a=sheets[0], b=sheets[1];"
          "a instanceof CSSStyleSheet && a instanceof StyleSheet", "true"),
       "DOM-owned sheets implement both interfaces");
    CK(eq("Object.getPrototypeOf(a)===CSSStyleSheet.prototype && "
          "Object.getPrototypeOf(CSSStyleSheet.prototype)===StyleSheet.prototype", "true"),
       "interface inheritance uses the actual sheet prototype");
    CK(eq("a.constructor===CSSStyleSheet && !({} instanceof CSSStyleSheet)", "true"),
       "plain objects are not branded as sheets");
    CK(eq("Object.prototype.toString.call(a)", "[object CSSStyleSheet]"),
       "sheet has its interface toString tag");
    CK(eq("a.ownerNode===document.getElementById('first') && a.rules===a.cssRules", "true"),
       "owner and legacy rules alias are backed by this sheet");
    CK(eq("CSSStyleSheet.prototype.insertRule===a.insertRule && "
          "CSSStyleSheet.prototype.deleteRule===a.deleteRule", "true"),
       "methods are on the interface, not copied per instance");
    CK(eq("(function(){try{CSSStyleSheet.prototype.insertRule.call({},'#x{}');"
          "return false}catch(e){return e instanceof TypeError}})()", "true"),
       "method refuses an unbranded receiver");
    CK(eq("(function(){try{CSSStyleSheet.prototype.deleteRule.call("
          "Object.create(CSSStyleSheet.prototype),0);return false}"
          "catch(e){return e instanceof TypeError}})()", "true"),
       "a copied prototype does not grant a native sheet receiver");
    CK(eq("(function(){try{Object.getOwnPropertyDescriptor(CSSStyleSheet.prototype,"
          "'cssRules').get.call({});return false}catch(e){return e instanceof TypeError}})()", "true"),
       "rules getter also checks its receiver");
    CK(eq("(function(){'use strict';try{a.cssRules=[];return false}"
          "catch(e){return e instanceof TypeError}})()", "true"),
       "the rules attribute cannot be replaced by assignment");
    CK(eq("(function(){'use strict';try{a.ownerNode=b.ownerNode;return false}"
          "catch(e){return e instanceof TypeError}})()", "true"),
       "owner cannot be redirected by assignment");
    CK(eq("a.__sx=1; a.insertRule('#probe{width:137px}',a.cssRules.length);"
          "document.getElementById('probe').offsetWidth", "137"),
       "CSS-SHEET insertRule changes real computed geometry");
    CK(eq("b.ownerNode.textContent.indexOf('137')<0 && "
          "a.ownerNode.textContent.indexOf('137')>=0", "true"),
       "a user-created old index property cannot redirect writeback");
    CK(eq("a.deleteRule(a.cssRules.length-1);document.getElementById('probe').offsetWidth", "40"),
       "CSS-SHEET deleteRule restores the actual cascade");
    CK(eq("a.removeRule===CSSStyleSheet.prototype.removeRule", "true"),
       "legacy removeRule remains available");
    CK(eq("var held=a.cssRules; a.insertRule('#probe{height:23px}',0);"
          "held===a.cssRules && held===a.rules && held[0].selectorText==='#probe'", "true"),
       "a held rules list sees method mutations");
    CK(eq("(function(value){var text=value instanceof CSSStyleSheet?"
          "Array.from(value.cssRules,function(r){return r.cssText}).join('\\n'):value.cssText;"
          "var style=document.createElement('style');style.textContent=text;"
          "document.head.appendChild(style);return style.textContent.length>0})(b)", "true"),
       "generic stylesheet-to-style fallback accepts real sheet instances");
    CK(eq("(function(value){var text=value instanceof CSSStyleSheet?'wrong':value.cssText;"
          "var style=document.createElement('style');style.textContent=text;"
          "document.head.appendChild(style);return document.getElementById('probe').offsetWidth})"
          "({cssText:'#probe{width:83px}'})", "83"),
       "CSS-SHEET fallback style is applied and measured, not just appended");
    CK(eq("(function(){try{new CSSStyleSheet();return false}"
          "catch(e){return e instanceof TypeError}})()", "true"),
       "constructed sheets explicitly remain unsupported");
    CK(eq("(function(){try{new StyleSheet();return false}"
          "catch(e){return e instanceof TypeError}})()", "true"),
       "base interface is not constructible");
    CK(eq("!('replace' in CSSStyleSheet.prototype) && "
          "!('replaceSync' in CSSStyleSheet.prototype) && !('adoptedStyleSheets' in document)", "true"),
       "no successful-looking constructed/adopted stylesheet feature signal");
    /* Keep a cycle through the opaque owner and rules. A native class with a
     * finalizer but no gc_mark can prematurely free one of these values. */
    CK(eq("a.ownerNode.keptSheet=a;a.cssRules.keptSheet=a;true", "true"),
       "native backing participates in a collectible object cycle");
    JS_RunGC(JS_GetRuntime(ctx));
    CK(eq("a.ownerNode.keptSheet===a && a.cssRules.keptSheet===a", "true"),
       "held sheet graph survives collection");
}

int main(void)
{
    css_init(); css_viewport(800, 600); js_page_set_clock(clk);
    /* Two runtimes expose a class registered once globally but not installed
     * into the next runtime, and close() catches retained native JS roots. */
    for (int run = 0; run < 2; run++) {
        const char *page = "<!doctype html><html><head>"
            "<style id=first>#probe{width:40px;height:20px}</style>"
            "<style id=second>#other{width:61px}</style>"
            "</head><body><div id=probe>sample</div><div id=other>other</div></body></html>";
        g_root = dom_parse(page, (int)strlen(page));
        if (!g_root) return 2;
        sheet_reflow(); js_cssom_set_reflow(sheet_reflow);
        if (!js_page_open(g_root)) return 2;
        ctx = js_page_ctx();
        CK(eq("document.styleSheets.length", "2"), "apparatus parsed both real style owners");
        test_sheet_interface();
        js_page_close(); dom_free(g_root);
    }
    printf("stylesheet-interface: %d checks, %s\n", checks, fails ? "FAIL" : "PASS");
    return fails;
}
