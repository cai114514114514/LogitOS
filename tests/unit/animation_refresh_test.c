/* The real app loop must publish animation frames without rebuilding boxes.
 * Count actual layout_page entries, not host elapsed time. The second half
 * compares refreshed items against an ordinary full layout of the same styles,
 * including text-parent ownership, controls, markers and rasterized SVG. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);
static int done,polls,observed,frames,lo=255,hi,geometry_ok=1;
static unsigned long long builds;
static int x0,y0,w0,h0;
static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<animation-observer>",0);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v);
    if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);return ok;
}
void loader_poll_hook(void)
{
    host_clock+=20;if(done)return;
    if(++polls<10000&&!expr("typeof ready!=='undefined'&&ready"))return;
    struct node *n=dom_get_element_by_id(js_dom_root()->doc,"fade");
    int x,y,w,h;int found=n&&layout_node_box(n,&x,&y,&w,&h);
    if(!observed){
        CHECK(found,"animated element has a real box");
        x0=x;y0=y;w0=w;h0=h;builds=layout_build_count();observed=1;
    }
    geometry_ok &= found&&x==x0&&y==y0&&w==w0&&h==h0;
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++)if(it[i].node==n&&it[i].has_bg){
        if(it[i].opacity<lo)lo=it[i].opacity;
        if(it[i].opacity>hi)hi=it[i].opacity;
    }
    if(++frames<80)return;
    CHECK(hi-lo>120,"animation publishes changing display-list alpha");
    CHECK(geometry_ok,"animation preserves real box geometry");
    CHECK(layout_build_count()==builds,"animation frames do not rebuild geometry");
    done=1;struct logit_event e={0};e.type=EV_CLOSE;host_post_event(&e);
}
static void differential(void)
{
    const char *html="<body style='margin:0'><div id=box style='width:240px;background:red'>"
        "alpha <span style='background:blue'>nested text</span><input value=control>"
        "<svg width=16 height=16><rect width=16 height=16 fill='green'/></svg>"
        "<img src='http://fixture.test/image.png' width=20 height=20></div>"
        "<ul><li id=marker>marker text</li></ul><p id=hide style='visibility:hidden'>hidden</p></body>";
    struct node *root=dom_parse(html,strlen(html));
    const char *css="#box::before{content:'PSEUDO';opacity:.2}";
    css_viewport(400,600);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
    const int values[]={127,255,0,254};
    for(unsigned v=0;v<sizeof values/sizeof values[0];v++){
    struct cstyle *st=dom_get_element_by_id(root->doc,"box")->style;st->opacity=values[v];st->hidden=values[v]==0;
    st=dom_get_element_by_id(root->doc,"marker")->style;st->opacity=values[v];
    CHECK(layout_refresh_opacity(root),"paint snapshot order rebuild succeeds");
    int count=layout_count();struct item *copy=malloc((size_t)count*sizeof *copy);
    memcpy(copy,layout_items(),(size_t)count*sizeof *copy);
    layout_page(root,400);const struct item *full=layout_items();int same=count==layout_count();
    for(int i=0;i<count&&i<layout_count();i++){
        int equal=copy[i].type==full[i].type&&copy[i].node==full[i].node&&
            copy[i].x==full[i].x&&copy[i].y==full[i].y&&copy[i].w==full[i].w&&copy[i].h==full[i].h&&
            copy[i].opacity==full[i].opacity&&copy[i].hidden==full[i].hidden;
        if(!equal)printf("refresh mismatch item=%d type=%d pseudo=%d alpha=%d/%d hidden=%d/%d box=%d,%d,%d,%d/%d,%d,%d,%d\n",
            i,copy[i].type,copy[i].pseudo,copy[i].opacity,full[i].opacity,copy[i].hidden,full[i].hidden,
            copy[i].x,copy[i].y,copy[i].w,copy[i].h,full[i].x,full[i].y,full[i].w,full[i].h);
        same &= equal;
    }
    CHECK(same,"paint refresh matches full layout for text controls markers SVG and images");
    free(copy);
    }
    layout_free();dom_free(root);
}
int main(void)
{
    const char *page="<!doctype html><style>@keyframes fade{from{opacity:0}to{opacity:1}}"
        "#fade{width:180px;height:60px;background:red;animation:fade 1s linear infinite alternate}"
        "</style><div id=fade>FADE TEXT</div><p>STATIONARY TEXT</p>"
        "<script>var ready=false;window.addEventListener('load',function(){ready=true});</script>";
    fake_site_reset();fake_site_add("http://fixture.test/animation.html",page);tabs_set_store(&memfs);
    const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/animation.html\tanimation\t0\n";
    memfs_write(SESSION_PATH,session,strlen(session));
    struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
    if(setjmp(host_exit_jmp)==0)app_main();
    CHECK(done&&observed,"animation observation completed");differential();
    return fail?1:0;
}
