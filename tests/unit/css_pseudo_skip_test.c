#define main generated_original_main
#include "generated_content_test.c"
#undef main
int main(void){
    unsigned long long before=css_generated_compose_count(),skipped=css_generated_skip_count();
    page("<!doctype html><style>*::before{color:red}*::after{content:none}</style><div>ONE</div><div>TWO</div><div>THREE</div><div>FOUR</div>",600);
    CHECK(ink("ONE") && ink("TWO") && ink("THREE") && ink("FOUR"),"empty pseudo styles preserve authored display list");
    unsigned long long cost=css_generated_compose_count()-before, saves=css_generated_skip_count()-skipped;
    printf("pseudo composition attempts=%llu skipped=%llu\n",cost,saves);
    CHECK(cost==0 && saves>=8,"empty pseudo reset avoids composition work");drop();
    page("<!doctype html><style>#x{content:'INHERITED';font-size:20px}#x::before{content:inherit}#x::after{content:attr(data-v)}</style><div id=x data-v=ATTR></div>",600);
    const struct item *a=ink("INHERITED"),*b=ink("ATTR");
    CHECK(a && b && a->font_px==20 && b->font_px==20,"content inherit and attr retain composed text and font");drop();
    page("<!doctype html><style>#x::before{content:'';display:block;width:37px;height:12px;background:red}</style><div id=x><div id=next>AFTER</div></div>",600);
    int x,y,w,h;layout_node_box(ID("x"),&x,&y,&w,&h);
    CHECK(h>=12 && ((struct cstyle*)ID("x")->style)->generated[0],"empty string still creates a real generated box");drop();
    printf("css-pseudo-skip: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
