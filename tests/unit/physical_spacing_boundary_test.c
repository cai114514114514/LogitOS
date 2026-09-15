/* Valid existing native units and invalid declaration preservation through the
 * same real geometry pipeline. No font ratios are invented here: every native
 * fallback is compared with the identical ordinary native declaration alone. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static void done(void) { layout_free();dom_free(g_root);g_root=0; }
static void box(const char *id,int b[4]) {
    char label[100];snprintf(label,sizeof label,"fixture box exists: %s",id);
    CHECK(layout_node_box(ID(id),b,b+1,b+2,b+3),label);
}
static void value(const char *group,const char *edge,int got,int want) {
    char label[200];snprintf(label,sizeof label,"%s: %s",group,edge);EQ(got,want,label);
}
static void padding(const char *label,const char *decl,const char *extra,
                    const char *inline_decl,int t,int r,int b,int l) {
    char html[6000];
    snprintf(html,sizeof html,"<style>html{font-size:16px;--spacing:4px}body{margin:0}"
        "#frame{width:200px}#target{width:100px;%s}.probe{height:10px;width:20px}%s</style>"
        "<div id=frame><div class=target id=target style='%s'><div class=probe id=inside></div></div></div>",
        decl,extra?extra:"",inline_decl?inline_decl:"");
    page(html,400);int q[4],c[4];box("target",q);box("inside",c);
    value(label,"left content origin",c[0]-q[0],l);
    value(label,"top content origin",c[1]-q[1],t);
    value(label,"horizontal edges contribute exactly once",q[2],100+l+r);
    value(label,"vertical edges contribute exactly once",q[3],10+t+b);done();
}

static void native_shape(const char *decl,int out[8]) {
    char html[4096];snprintf(html,sizeof html,"<style>body{margin:0}#frame{width:200px;padding:1px}"
        "#target{font-size:20px;width:100px;%s}#inside{height:10px}</style>"
        "<div id=frame><div id=target><div id=inside></div></div></div>",decl);
    page(html,400);box("target",out);box("inside",out+4);done();
}
static void native_compare(const char *name,const char *reference,const char *candidate) {
    printf("checking native %s\n",name);
    int a[8],b[8];native_shape(reference,a);native_shape(candidate,b);
    for(int i=0;i<8;i++) {char label[160];snprintf(label,sizeof label,"native %s geometry %d",name,i);EQ(b[i],a[i],label);}
}
static void support(const char *prop,const char *value,int want) {
    char label[180];snprintf(label,sizeof label,"supports %s: %s",prop,value);
    printf("checking %s\n",label);
    EQ(css_supports_decl(prop,-1,value,-1),want,label);
}
int main(void) {
    setbuf(stdout,0);
    native_compare("ch override","padding:2ch","padding:calc(3px*7);padding:2ch");
    native_compare("ex override","padding:2ex","padding:calc(3px*7);padding:2ex");
    native_compare("revert override","padding:revert","padding:calc(3px*7);padding:revert");
    native_compare("margin ch","margin:2ch","margin:calc(3px*7);margin:2ch");
    padding("uppercase","PADDING:CALC(4px*3)",0,0,12,12,12,12);
    padding("invalid unit","padding:calc(4px*3);padding:3qu",0,0,12,12,12,12);
    padding("invalid last component","padding:calc(4px*3);padding:1px 2px 3px auto",0,0,12,12,12,12);
    padding("invalid bare product","padding:calc(4px*3);padding:4px*2",0,0,12,12,12,12);
    padding("logical reset","padding:calc(4px*3);padding-inline:initial",0,0,12,0,12,0);
    padding("logical inherit","padding:calc(4px*3);padding-inline:inherit","#frame{padding:7px}",0,12,7,12,7);
    support("padding","calc(4px * 6)",1);support("PADDING-TOP","calc(4px * 6)",1);
    support("margin","calc(-4px * 2) auto",1);support("top","calc(10% + 2px)",1);
    support("padding","calc(4px * 2px)",0);support("padding","calc(3deg)",0);
    support("padding","1px 2px 3px auto",0);support("padding","-3px",0);
    support("padding","2ch",1);support("padding","revert",1);
    printf("physical spacing boundaries: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
