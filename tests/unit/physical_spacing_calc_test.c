/* Finite ordinary CSS through DOM, variable expansion, cascade and actual
 * layout boxes. Literal declarations and fixed containing-block arithmetic
 * are independent oracles; no page capture, network or script is involved. */
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
static void margin(const char *label,const char *decl,int t,int r,int b,int l) {
    char html[4096];
    /* One pixel parent padding keeps normal block margins from collapsing
     * outside their containing box. The following sibling has zero margins. */
    snprintf(html,sizeof html,"<style>html{--spacing:4px}body{margin:0}"
        "#frame{width:200px;padding:1px}#target{height:10px;%s}#after{height:1px}</style>"
        "<div id=frame><div id=target></div><div id=after></div></div>",decl);
    page(html,400);int f[4],q[4],n[4];box("frame",f);box("target",q);box("after",n);
    value(label,"left margin positions box",q[0]-f[0],1+l);
    value(label,"top margin positions box",q[1]-f[1],1+t);
    value(label,"both side margins reduce automatic width",q[2],200-l-r);
    value(label,"bottom margin positions next sibling",n[1]-q[1]-q[3],b);done();
}
static void margin_auto(const char *label,const char *decl,int t) {
    char html[4096];snprintf(html,sizeof html,"<style>body{margin:0}"
        "#frame{width:200px;padding:1px}#target{width:20px;height:10px;%s}</style>"
        "<div id=frame><div id=target></div></div>",decl);
    page(html,400);int f[4],q[4];box("frame",f);box("target",q);
    value(label,"auto side margins center fixed box",q[0]-f[0],91);
    value(label,"vertical calculation survives auto shorthand",q[1]-f[1],1+t);done();
}
static void inset(const char *label,const char *decl,int x,int y) {
    char html[4096];snprintf(html,sizeof html,"<style>html{--spacing:4px}body{margin:0}"
        "#frame{position:relative;width:200px;height:100px}#target{position:absolute;width:20px;height:10px;%s}</style>"
        "<div id=frame><div id=target></div></div>",decl);
    page(html,400);int f[4],q[4];box("frame",f);box("target",q);
    value(label,"absolute x uses containing block",q[0]-f[0],x);
    value(label,"absolute y uses containing block",q[1]-f[1],y);done();
}
static void percentage(void) {
    const char *html="<style>body{margin:0}#target{width:100px;padding:calc(5% + 3px) calc(10% + 2px)}"
        "#inside{width:20px;height:10px}</style><div id=target><div id=inside></div></div>";
    page(html,400);
    for(int i=0;i<3;i++) {
        if(i)layout_page(g_root,800);
        int q[4],c[4];box("target",q);box("inside",c);
        const char *name=i==0?"percentage/initial":i==1?"percentage/resize":"percentage/repeat";
        int x=i?82:42,y=i?43:23;
        value(name,"padding x uses containing width plus fixed addend",c[0]-q[0],x);
        value(name,"padding y also uses containing width",c[1]-q[1],y);
        value(name,"padding width is not accumulated",q[2],100+2*x);
        value(name,"padding height is not accumulated",q[3],10+2*y);
    }done();
    const char *m="<style>body{margin:0}#frame{padding:1px}#target{height:10px;"
        "margin:calc(5% + 3px) calc(10% + 2px)}#after{height:1px}</style>"
        "<div id=frame><div id=target></div><div id=after></div></div>";
    page(m,402);
    for(int i=0;i<3;i++) {
        if(i)layout_page(g_root,802);
        int f[4],q[4],n[4];box("frame",f);box("target",q);box("after",n);
        const char *name=i==0?"percentage/margin initial":i==1?"percentage/margin resize":"percentage/margin repeat";
        int x=i?82:42,y=i?43:23,cw=i?800:400;
        value(name,"margin x uses containing content width",q[0]-f[0],1+x);
        value(name,"vertical percent margin also uses width",q[1]-f[1],1+y);
        value(name,"margin automatic width recomputes",q[2],cw-2*x);
        value(name,"bottom margin recomputes",n[1]-q[1]-q[3],y);
    }done();
    const char *s="<style>body{margin:0}#frame{position:relative;height:100px}"
        "#target{position:absolute;width:20px;height:10px;left:calc(10% + 2px);top:calc(10% + 3px)}</style>"
        "<div id=frame><div id=target></div></div>";
    page(s,400);
    for(int i=0;i<3;i++) {
        if(i)layout_page(g_root,800);
        int f[4],q[4];box("frame",f);box("target",q);
        const char *name=i==0?"percentage/inset initial":i==1?"percentage/inset resize":"percentage/inset repeat";
        value(name,"left uses width plus immutable addend",q[0]-f[0],i?82:42);
        value(name,"top uses height rather than width",q[1]-f[1],13);
    }done();
}
int main(int argc,char **argv) {
    css_init();css_viewport(400,240);
    padding("literal/padding four edges","padding:4px 8px 12px 16px",0,0,4,8,12,16);
    margin("literal/margin four edges","margin:4px 8px 12px 16px",4,8,12,16);
    margin("literal/negative margin","margin:-4px -8px -12px -16px",-4,-8,-12,-16);
    margin_auto("literal/auto margin","margin:12px auto",12);
    inset("literal/top left","top:12px;left:8px",8,12);
    inset("literal/bottom right","bottom:12px;right:8px",172,78);
    if(argc==2&&!strcmp(argv[1],"literal"))goto finish;
    if(argc!=1)return 2;
    padding("physical/padding one component","padding:calc(var(--spacing)*3)",0,0,12,12,12,12);
    padding("physical/padding two components","padding:calc(4px*2) calc(4px*3)",0,0,8,12,8,12);
    padding("physical/padding three components","padding:calc(4px*1) calc(4px*2) calc(4px*3)",0,0,4,8,12,8);
    padding("physical/padding four components","padding:calc(4px*1) calc(4px*2) calc(4px*3) calc(4px*4)",0,0,4,8,12,16);
    padding("physical/padding longhands","padding-top:calc(4px*1);padding-right:calc(4px*2);padding-bottom:calc(4px*3);padding-left:calc(4px*4)",0,0,4,8,12,16);
    padding("physical/font relative","font-size:20px;padding:calc(.5em + .25rem)",0,0,14,14,14,14);
    padding("cascade/later longhand zero","padding:calc(4px*3);padding-left:0",0,0,12,12,12,0);
    padding("cascade/later shorthand zero","padding-left:calc(4px*3);padding:0",0,0,0,0,0,0);
    padding("cascade/later shorthand calc","padding-left:7px;padding:calc(4px*3)",0,0,12,12,12,12);
    padding("cascade/later logical edge","padding:calc(4px*3);padding-inline-start:5px",0,0,12,12,12,5);
    padding("cascade/later physical edge","padding-inline-start:5px;padding-left:calc(4px*3)",0,0,0,0,0,12);
    padding("cascade/later logical shorthand","padding-left:calc(4px*3);padding-inline:5px 7px",0,0,0,7,0,5);
    padding("cascade/later physical shorthand","padding-inline:5px 7px;padding:calc(4px*3)",0,0,12,12,12,12);
    padding("cascade/physical specificity","padding-left:calc(4px*3)",".target{padding-inline-start:5px}",0,0,0,0,12);
    padding("cascade/logical specificity","padding-inline-start:5px",".target{padding-left:calc(4px*3)}",0,0,0,0,5);
    padding("cascade/physical important","padding-left:calc(4px*3)!important",0,"padding-inline-start:5px",0,0,0,12);
    padding("cascade/logical important","padding-inline-start:5px!important",0,"padding-left:calc(4px*3)",0,0,0,5);
    padding("reset/initial","padding:calc(4px*3);padding:initial",0,0,0,0,0,0);
    padding("reset/unset","padding:calc(4px*3);padding:unset",0,0,0,0,0,0);
    padding("reset/inherit","padding:calc(4px*3);padding:inherit","#frame{padding:7px}",0,7,7,7,7);
    margin("physical/margin shorthand","margin:calc(4px*1) calc(4px*2) calc(4px*3) calc(4px*4)",4,8,12,16);
    margin("physical/margin longhands","margin-top:calc(4px*1);margin-right:calc(4px*2);margin-bottom:calc(4px*3);margin-left:calc(4px*4)",4,8,12,16);
    margin("physical/negative margin","margin:calc(0px - 4px) calc(0px - 8px) calc(0px - 12px) calc(0px - 16px)",-4,-8,-12,-16);
    margin("cascade/margin later longhand","margin:calc(4px*3);margin-left:0",12,12,12,0);
    margin("cascade/margin later shorthand","margin-left:calc(4px*3);margin:0",0,0,0,0);
    margin("cascade/margin logical then physical","margin-inline-start:5px;margin-left:calc(4px*3)",0,0,0,12);
    margin("cascade/margin physical then logical","margin-left:calc(4px*3);margin-inline-start:5px",0,0,0,5);
    margin_auto("physical/margin auto","margin:calc(4px*3) auto",12);
    inset("physical/top left","top:calc(var(--spacing)*3);left:calc(4px*2)",8,12);
    inset("physical/bottom right","bottom:calc(4px*3);right:calc(4px*2)",172,78);
    inset("cascade/inset later literal","top:calc(4px*3);top:0;left:calc(4px*2);left:0",0,0);
    inset("cascade/inset later calc","top:0;top:calc(4px*3);left:0;left:calc(4px*2)",8,12);
    inset("cascade/inset auto reset","top:calc(4px*3);top:auto;bottom:8px;left:calc(4px*2);left:auto;right:4px",176,82);
    percentage();
finish:
    printf("physical-spacing-calc: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
