#define main generated_original_main
#include "generated_content_test.c"
#undef main
static void inspect_grid(const char *label){
    const struct item *a=ink("A"),*b=ink("B");
    if (!(a && b && b->x-a->x==37)) printf("grid diagnostic: %s x=%d,%d display=%d\n",label,a?a->x:-1,b?b->x:-1,((struct cstyle*)ID("x")->style)->display);
    CHECK(a && b && b->x-a->x==37,label);
}
int main(void){
    page("<!doctype html><style>body{margin:0;color:#123456}</style><div id=x style='display:grid;grid-template-columns:37px 53px'><span>A</span><span>B</span></div>",600);
    EQ(css_extra_rules(),0,"ordinary stylesheet has no extension rules");
    inspect_grid("inline grid reaches layout without stylesheet extension rules");drop();
    page("<!doctype html><style>body{margin:0;color:#123456}</style><div id=x style='display:grid;grid-template-columns:37px 53px!important;grid-template-columns:11px 19px'><span>A</span><span>B</span></div>",600);
    inspect_grid("inline important keeps precedence on no-rule path");drop();
    page("<!doctype html><div id=x style='display:grid;grid-template-columns:37px 53px'><span>A</span><span>B</span></div>",600);
    inspect_grid("empty stylesheet retains inline extension layout");drop();
    printf("css-inline-extensions: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
