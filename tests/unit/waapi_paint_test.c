/* The observer is the real layout display list and cstyle transform consumed
 * by the painter, not getComputedStyle's JS proxy. A fake monotonic clock
 * controls the shipping js_page deadline consumer; no host stopwatch. */
#define main old_layout_main
#include "layout_box_test.c"
#undef main
#include "js_page.h"
#include "js_anim.h"
#include "css_interp.h"
static unsigned long long wa_now=1000;
static unsigned long long wa_clock(void){return wa_now;}
static void run_js(const char *s){CHECK(js_page_eval(s,(int)strlen(s),"<waapi-paint-test>",0),"script evaluates");}
static void js_check(const char *s,const char *name){
 JSContext *ctx=js_page_ctx();JSValue v=JS_Eval(ctx,s,strlen(s),"<assert>",0);
 if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char *m=JS_ToCString(ctx,e);printf("exception: %s\n",m?m:"");if(m)JS_FreeCString(ctx,m);JS_FreeValue(ctx,e);}
 CHECK(!JS_IsException(v)&&JS_ToBool(ctx,v)>0,name);JS_FreeValue(ctx,v);
}
static int last_ran;
static void step(int ms){wa_now+=ms;last_ran=js_page_run_due();if(css_anim_needs_layout()==2)layout_page(g_root,400);}
static int ink_op(void){const struct item *items=layout_items();for(int i=0;i<layout_count();i++)if(items[i].node==ID("box")&&items[i].has_bg)return items[i].opacity;return -1;}
static int tx(void){struct cstyle *st=ID("box")->style;struct ci_xform x;double m[16];if(!st->xraw[XR_TRANSFORM])return 0;if(ci_transform_parse(st->xraw[XR_TRANSFORM],st->xrawlen[XR_TRANSFORM],16,16,&x))return -999;ci_transform_matrix(&x,40,40,m);return (int)(m[12]+0.5);}
int main(void){
 css_init();page("<html><body><div id=box style='width:40px;height:40px;background:red;opacity:1'></div><span id=other>other</span></body></html>",400);
 js_page_set_clock(wa_clock);CHECK(js_page_open(g_root),"page opens");
 run_js("var el=document.getElementById('box');var a=el.animate([{opacity:0,transform:'translateX(0px)'},{opacity:1,transform:'translateX(100px)'}],{duration:1000,fill:'both',easing:'linear'});var finishedCount=0;a.finished.then(function(){finishedCount++});");
 CHECK(js_page_pending(),"WAAPI wakes production deadline queue");step(500);
 CHECK(ink_op()>=126&&ink_op()<=129,"WAAPI midpoint reaches display-list opacity");EQ(tx(),50,"WAAPI midpoint reaches painter transform");
 js_check("a.currentTime===500&&a.playState==='running'&&finishedCount===0","running time advances without early finished promise");
 css_apply_scoped(ID("other"),0,"",0);css_extra_apply(ID("other"),"",0);
 run_js("a.pause()");step(100);EQ(tx(),50,"pause holds painter transform");js_check("a.currentTime===500&&a.playState==='paused'","pause excludes elapsed time");CHECK(!js_page_pending(),"paused animation does not spin queue");
 run_js("a.currentTime=250");step(0);EQ(tx(),25,"seek updates native transform");
 run_js("a.reverse()");step(100);EQ(tx(),15,"reverse advances backwards");
 run_js("a.finish()");step(0);EQ(tx(),0,"negative finish holds start frame");js_check("a.currentTime===0&&a.playState==='finished'&&finishedCount===1","finish state and promise settle once");
 run_js("a.cancel()");step(0);EQ(ink_op(),255,"cancel restores real display-list base opacity");EQ(tx(),0,"cancel removes native transform");
 run_js("a.playbackRate=1;a.play()");step(1000);EQ(tx(),100,"play after cancel restores registration and finishes");js_check("a.playState==='finished'&&a.currentTime===1000","automatic completion updates state");
 css_anim_snapshot(g_root);css_apply(g_root,"#box{opacity:.8}",17);css_extra_apply(g_root,"#box{opacity:.8}",17);layout_page(g_root,400);EQ(tx(),100,"held frame survives cascade replay");
 run_js("a.cancel();var b=el.animate([{opacity:0},{opacity:1}],{duration:100,fill:'none'});");step(100);EQ(ink_op(),255,"fill none restores inline base after completion");
 run_js("b.cancel();var automaticDone=0,automaticEvent=0;var still=el.animate([{opacity:1},{opacity:1}],{duration:100,fill:'forwards'});still.finished.then(function(){automaticDone++;document.getElementById('other').textContent='finished-without-timer'});still.addEventListener('finish',function(){automaticEvent++})");
 step(0);step(100);
 js_check("automaticDone===1&&automaticEvent===1&&document.getElementById('other').textContent==='finished-without-timer'","automatic finish reactions drain without timers or pixel changes");
 CHECK(last_ran>0,"microtask-only completion requests browser settlement");
 CHECK(!js_page_pending(),"automatic completion needs no follow-up task");
 run_js("still.cancel()");step(0);
 run_js("b.cancel();var c=el.animate([{opacity:0},{opacity:1}],{duration:1000,fill:'both'});c.pause();c.currentTime=500;");step(0);
 run_js("el.remove();c.cancel()");step(0);CHECK(!js_page_pending(),"detached canceled target releases deadline");
 js_check("(function(){try{el.animate([{width:'0px'},{width:'100px'}],100);return false}catch(e){return e.name==='NotSupportedError'}})()","unsupported property is explicitly refused");
 js_check("(function(){try{el.animate([{opacity:1}],100);return false}catch(e){return e.name==='NotSupportedError'}})()","implicit underlying endpoints are refused");
 js_check("(function(){try{el.animate([{opacity:0},{opacity:1}],{duration:Infinity}).finish();return false}catch(e){return e.name==='InvalidStateError'}})()","infinite forward finish rejects");
 run_js("document.getAnimations().forEach(function(a){a.cancel()})");step(0);
 js_page_close();CHECK(!css_anim_active(),"page close releases WAAPI clock and references");layout_free();dom_free(g_root);css_anim_reset();
 printf("waapi-paint: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
