/* Reuse the actual generated-content layout apparatus, not a CSS-name oracle. */
#define main generated_original_main
#include "generated_content_test.c"
#undef main
int main(void) {
    const char *values[] = {"'STR'", "attr(data-v)", "'PRE' attr(data-v)", "none", "normal", "counter(n)", "url(x.png)", "open-quote", "'PART' counter(n)", "none garbage", "attr()", "'broken"};
    for (int i=0;i<12;i++) {
        char html[1024], label[256]; int want=i<5;
        snprintf(html,sizeof html,"<!doctype html><style>@supports(content:%s){#x::before{content:'SUPPORTED'}}@supports not (content:%s){#x::after{content:'FALLBACK'}}</style><div id=x data-v=VALUE></div>",values[i],values[i]);
        page(html,600);
        snprintf(label,sizeof label,"@supports generated branch %s",values[i]);
        CHECK((ink("SUPPORTED")!=NULL)==want,label);
        /* An unbalanced value invalidates the entire condition, so the not
         * rule itself is invalid too; only well-formed probes assert fallback. */
        if(i<11){snprintf(label,sizeof label,"@supports fallback branch %s",values[i]);CHECK((ink("FALLBACK")!=NULL)==!want,label);}
        snprintf(label,sizeof label,"CSS.supports generated subset %s",values[i]);
        CHECK(css_supports_decl("content",7,values[i],-1)==want,label);
        drop();
    }
    printf("content-supports-wiring: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
