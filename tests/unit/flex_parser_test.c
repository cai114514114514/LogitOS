/* The real stylesheet -> computed style -> layout consumer. In particular,
 * do not accept a parser return code while the selected grow/basis vanish. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main

static void decl(const char *rule, int grow, int shrink, int basis, int pct, int has)
{
    char html[4096];
    snprintf(html,sizeof html,"<style>#x{flex-grow:7;flex-shrink:8;flex-basis:9px;%s}</style><div id=x></div>",rule);
    page(html,400);
    struct cstyle *s=ID("x")->style;
    checks++;
    if(!s || s->flex_grow!=grow || s->flex_shrink!=shrink || s->has_fb!=has ||
       (has && (s->flex_basis!=basis || s->fb_pct!=pct))) {
        fails++;
        printf("  FAIL: %s -- got grow=%d shrink=%d basis=%d pct=%d has=%d; want %d %d %d %d %d\n",
            rule,s?s->flex_grow:-1,s?s->flex_shrink:-1,s?s->flex_basis:-1,s?s->fb_pct:-1,s?s->has_fb:-1,
            grow,shrink,basis,pct,has);
    }
}
#define FLEX(s,g,sh,b,p,h) decl("flex:" s, (g)*1024,(sh)*1024,b,p,h)
#define INVALID(s) FLEX(s,7,8,9,0,1)

int main(int argc, char **argv)
{
    css_init();css_viewport(400,600);
    /* Retain the separate discovered limitation as a runnable RED audit.
     * The numeric/length CALC parser accepts these, but the flex longhand
     * cascade's TODO branches skip them. This grammar patch does not pretend
     * those missing consumers work, nor make their old fallback the oracle. */
    if(argc>1 && !strcmp(argv[1],"--calc-audit")) {
        FLEX("calc(2) 3 0%",2,3,0,1,1);
        FLEX("2 calc(3) 0%",2,3,0,1,1);
        FLEX("2 3 calc(20px)",2,3,20,0,1);
        decl("flex-grow:calc(2)",2*1024,8*1024,9,0,1);
        decl("flex-shrink:calc(3)",7*1024,3*1024,9,0,1);
        printf("flex-parser-calc-audit: %d checks, %d failures\n",checks,fails);
        return fails?1:0;
    }
    FLEX("1 1 0%",1,1,0,1,1);
    FLEX("3 1 0%",3,1,0,1,1);
    FLEX("1 0 20px",1,0,20,0,1);
    FLEX("1 0",1,0,0,0,1);
    FLEX("1 2",1,2,0,0,1);
    FLEX("1 1 0",1,1,0,0,1);
    FLEX("0",0,1,0,0,1);
    FLEX("20px",1,1,20,0,1);
    FLEX("20px 2 3",2,3,20,0,1);
    FLEX("auto 2 3",2,3,0,0,0);
    FLEX("auto",1,1,0,0,0);
    FLEX("none",0,0,0,0,0);
    FLEX("initial",0,1,0,0,0);
    FLEX("2 3 auto",2,3,0,0,0);
    FLEX("1 1 0%!important",1,1,0,1,1);
    FLEX("0% 0",0,1,0,1,1);
    FLEX("20px 0 0",0,0,20,0,1);
    decl("flex:.5 .25 20px",512,256,20,0,1);
    INVALID("1 20px 2");
    INVALID("1 2 3");
    INVALID("1 -1 0%");
    INVALID("-1");
    INVALID("1 1 -2px");
    INVALID("1 initial");
    INVALID("1 none");
    INVALID("1 2 0% 4");
    INVALID("1 0% 0");
    INVALID("20px -1");
    INVALID("1 2 initial");
    INVALID("calc(2px) 3 0%");
    decl("flex-grow:-1",7*1024,8*1024,9,0,1);
    decl("flex-shrink:2px",7*1024,8*1024,9,0,1);
    decl("flex-basis:-2%",7*1024,8*1024,9,0,1);
    decl("flex-grow:calc(2px)",7*1024,8*1024,9,0,1);
    decl("flex-shrink:calc(2%)",7*1024,8*1024,9,0,1);
    decl("flex-basis:20%",7*1024,8*1024,20,1,1);
    page("<style>body{margin:0}#f{display:flex;width:200px}#a{flex:1 1 0%;min-width:0}#b{flex:3 1 0%;min-width:0}</style><div id=f><div id=a>A</div><div id=b>B</div></div>",400);
    EQ(width("a"),50,"three-part shorthand reaches real first flex slot");
    EQ(width("b"),150,"three-part shorthand reaches real second flex slot");
    printf("flex-parser: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
