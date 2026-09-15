/* app_main is the consumer under test; transport only delays bytes and posts
 * native events. No test callback edits the DOM or runs a replacement loop.
 * A held resource must allow a native edit to reach DOM AND actual paint before
 * completion. Seeing queued keys after completion is explicitly insufficient. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);
extern int held_id,held_started,held_active,held_pumps,held_fallback;
extern int held_allow,held_cancelled,held_taken,held_released,held_auto;
static const char *mode;
static int polls,finished,arrival,input_before_release,timer_before_release;
static int pending_parks,bad_park,timerless_parks,cancel_observed,finish_delay;
static uintptr_t original_slot;
static uint32_t original_serial;
static int lifetime_while_pending;
static int premature_load_park;
static void finish(void);
static const char *SOURCE="http://fixture.test/async/source.html";
static const char *TARGET="http://fixture.test/async/target.html";

static int expr(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<held-resource-observer>",JS_EVAL_TYPE_GLOBAL);
    int r=0;if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else r=JS_ToBool(ctx,v);
    JS_FreeValue(ctx,v);return r;
}
static const struct paintop *last_text(const char *s)
{
    int n=(int)strlen(s);
    for(int i=paint_nops-1;i>=0;i--)
        if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==n&&!memcmp(paint_ops[i].text,s,n))return &paint_ops[i];
    return 0;
}
static void post(int type,int a,int b,int button,int mods)
{
    struct logit_event e={0};e.type=type;e.a=a;e.b=b;e.button=button;e.mods=mods;host_post_event(&e);
}
static int is_cancel(void)
{return !strcmp(mode,"navigate")||!strcmp(mode,"tab")||!strcmp(mode,"close")||!strcmp(mode,"destroy");}
static struct node *find_id(struct node *root,const char *id)
{
    if(!root)return 0;
    const char *v=dom_attr(root,"id");if(v&&!strcmp(v,id))return root;
    for(struct node *c=root->first_child;c;c=c->next){struct node *n=find_id(c,id);if(n)return n;}
    return 0;
}
void held_lifetime_observe(void)
{
    /* A destruction can retire its job before the next poll hook. Observe the
     * page-written marker at release, without evaluating JS or dispatching. */
    struct node *holder=find_id(js_dom_root(),"holder");
    if(held_active&&holder&&dom_attr(holder,"data-lifetime"))lifetime_while_pending=1;
}
static struct node *observed_node(const char *s)
{
    JSContext *ctx=js_page_ctx();if(!ctx)return 0;
    JSValue v=JS_Eval(ctx,s,strlen(s),"<held-node-observer>",JS_EVAL_TYPE_GLOBAL);
    struct node *n=0;
    if(JS_IsException(v))JS_FreeValue(ctx,JS_GetException(ctx));else n=js_dom_node_from(v);
    JS_FreeValue(ctx,v);return n;
}

void held_native_arrival(void)
{
    /* Called by fake pump exactly once while pending. Enqueue only: even the
     * negative wait loop receives these events, but cannot process them. */
    arrival=1;
    struct node *script=find_id(js_dom_root(),"slow");
    if(script){original_slot=(uintptr_t)script;original_serial=script->serial;}
    if(!strcmp(mode,"destroy"))return;
    if(!strcmp(mode,"no-timer")||!strcmp(mode,"initial-idle"))return;
    if(!strcmp(mode,"close")){paint_nops=0;post(EV_CLOSE,0,0,0,0);return;}
    if(!strcmp(mode,"tab")){paint_nops=0;post(EV_KEY,20,0,0,EV_MOD_CTRL);return;}
    const struct paintop *p=last_text(!strcmp(mode,"navigate")?"LEAVE":"SEED");
    CHECK(p!=0,"native event coordinates come from actual initial paint");
    if(!p)return;
    int x=p->x+3,y=p->y+3;paint_nops=0;
    post(EV_MOUSE,x,y,EV_BTN_LEFT,0);post(EV_MOUSE_UP,x,y,EV_BTN_LEFT,0);
    if(!strcmp(mode,"navigate"))return;
    post(EV_KEY,1,0,0,EV_MOD_CTRL);
    for(const char *s="TYPED";*s;s++)post(EV_KEY,*s,0,0,0);
}
void late_callback_park(int ms)
{
    if(held_active){
        pending_parks++;if(ms<=0||ms>10)bad_park++;
        if(js_page_next_due()<0)timerless_parks++;
    }
    /* A no-op host wait would quietly execute one more poll and dispatch load.
     * The guest would instead sleep forever here. Stop on the FIRST such park,
     * before another host turn can hide the ordering defect. */
    if(!finished && !strcmp(mode,"initial-idle") && !held_active && held_taken==1 && ms==0 &&
       expr("events.indexOf('chain-load')>=0&&loads===0")){
        premature_load_park=1;finish();
    }
    /* Observe production's timeout argument; a host no-op by itself would
     * let a missing guest pump wake pass silently. Never advance network here. */
}
static void finish(void)
{
    if(finished)return;finished=1;
    CHECK(held_started==1,"exactly one held script request was admitted");
    CHECK(arrival,"native arrival occurred while request pending");
    CHECK(!held_fallback,"held request completed without synchronous-wait fallback");
    if(is_cancel()){
        CHECK(held_cancelled==1&&held_taken==0,"native cancellation releases pending script without execution");
        if(!strcmp(mode,"destroy")){
            struct node *replacement=observed_node("document.getElementById('replacement')");
            CHECK(lifetime_while_pending,"destruction happened after preparation while request pending");
            CHECK(replacement&&(uintptr_t)replacement==original_slot&&replacement->serial!=original_serial,
                  "destroyed script slot was actually reused with a new serial");
            CHECK(expr("typeof oldRealmRan==='undefined'&&stray===0&&events.join(',')==='ordered,ordered-load'"),
                  "destroyed script neither executes nor dispatches to recycled slot");
        }else if(!strcmp(mode,"navigate")){
            CHECK(!strcmp(js_page_location(),TARGET),"native link reached destination realm");
            CHECK(last_text("DESTINATION")!=0,"cancel destination reaches actual paint");
            CHECK(expr("typeof oldRealmRan==='undefined'&&typeof events==='undefined'"),"cancelled script and load callback never enter destination realm");
        }else if(!strcmp(mode,"tab"))CHECK(tabs_count()==2,"native new tab actually replaced the live realm");
    }else{
        if(strcmp(mode,"no-timer")&&strcmp(mode,"initial-idle")){
            CHECK(input_before_release,"native input painted while script pending");
            CHECK(timer_before_release,"JS timer ran while script pending");
        }else CHECK(pending_parks>0&&timerless_parks>0&&!bad_park,"pending script alone retains bounded pump wake");
        CHECK(held_taken==1,"held script body consumed exactly once");
        CHECK(expr("events.join(',')==='slow,slow-load,ordered,ordered-load,chain,chain-load'"),"ordered scripts and onload chain execute exactly once in order");
        CHECK(expr("loads===1"),"window load fires exactly once");
        if(!strcmp(mode,"initial")||!strcmp(mode,"initial-idle"))
            CHECK(expr("loadSawChain===true"),"initial inserted script chain delays window load");
        if(!strcmp(mode,"initial-idle"))
            CHECK(!premature_load_park,"initial load cannot park before lifecycle dispatch");
        CHECK(expr("badCurrent===0"),"external scripts expose their actual currentScript node");
        if(!strcmp(mode,"detach")){
            CHECK(lifetime_while_pending,"detach happened after preparation while request pending");
            CHECK(expr("keptSlow.parentNode===null&&keptSlow.id==='slow'"),"detached script wrapper stays live through execution and load");
        }
        if(!strcmp(mode,"mutate")){
            CHECK(lifetime_while_pending,"src mutation happened after preparation while request pending");
            CHECK(fake_site_fetched("alternate.js")==0&&expr("typeof alternateRan==='undefined'"),"prepared script uses original URL and source after src mutation");
        }
    }
    post(EV_CLOSE,0,0,0,0);
}
void loader_poll_hook(void)
{
    host_clock+=10;
    if(finished)return;
    if(++polls>900){finish();return;}
    if(held_active&&expr("lifetimeChanged===true"))lifetime_while_pending=1;
    if(held_active && !is_cancel() && strcmp(mode,"no-timer")&&strcmp(mode,"initial-idle")){
        if(expr("ticks>0"))timer_before_release=1;
        if(expr("document.getElementById('entry').value==='TYPED'")&&last_text("TYPED")){
            input_before_release=1;
            if(timer_before_release){
                CHECK(strcmp(mode,"initial")||expr("loads===0"),"window load stays owed during initial held script");
                held_allow=1;
            }
        }
    }
    if(is_cancel()){
        if(held_cancelled && !cancel_observed){
            cancel_observed=1;paint_nops=0;
            if(strcmp(mode,"close"))post(EV_RESIZE,1180,620,0,0);
        }
        if(cancel_observed && ++finish_delay>15)finish();
    }else if(held_started&&!held_active&&expr("events.indexOf('chain-load')>=0&&loads===1")){
        if(++finish_delay>8)finish();
    }
}
int main(int argc,char **argv)
{
    mode=argc>1?argv[1]:"input";held_auto=!strcmp(mode,"no-timer")||!strcmp(mode,"initial-idle");
    char page[4096];
    const char *setup=
        "var events=[],loads=0,loadSawChain=false,ticks=0,badCurrent=0,lifetimeChanged=false,stray=0,keptSlow;"
        "addEventListener('load',function(){loads++;loadSawChain=events.indexOf('chain-load')>=0});"
        "function add(id,url){var s=document.createElement('script');s.id=id;s.async=false;s.src=url;"
        "s.onload=function(){events.push(id+'-load');if(id==='slow')add('chain','chain.js')};"
        "if(id==='slow'){keptSlow=s;document.getElementById('holder').appendChild(s)}else document.head.appendChild(s)}"
        "function start(){add('slow','slow.js');add('ordered','ordered.js')}";
    const char *start=!strcmp(mode,"initial")?"setTimeout(function(){ticks++},100);start();":
        !strcmp(mode,"initial-idle")?"start();":
        !strcmp(mode,"no-timer")?"setTimeout(start,200);":
        !strcmp(mode,"detach")?"setTimeout(start,200);setTimeout(function(){keptSlow.parentNode.removeChild(keptSlow);document.getElementById('holder').setAttribute('data-lifetime','changed');lifetimeChanged=true;ticks++},300);":
        !strcmp(mode,"mutate")?"setTimeout(start,200);setTimeout(function(){keptSlow.src='alternate.js';document.getElementById('holder').setAttribute('data-lifetime','changed');lifetimeChanged=true;ticks++},300);":
        !strcmp(mode,"destroy")?"setTimeout(start,200);setTimeout(function(){document.getElementById('holder').innerHTML='<span id=replacement>REUSED</span>';document.getElementById('replacement').onload=function(){stray++};document.getElementById('holder').setAttribute('data-lifetime','changed');lifetimeChanged=true;ticks++},300);":
        "setTimeout(start,200);setTimeout(function(){ticks++},300);";
    snprintf(page,sizeof page,"<!doctype html><style>body{margin:0}"
        "#entry{position:absolute;left:40px;top:100px;width:280px;height:35px}"
        "#leave{position:absolute;left:40px;top:200px;width:150px}</style>"
        "<div id=holder></div><input id=entry value=SEED><a id=leave href=target.html>LEAVE</a><script>%s%s</script>",setup,start);
    fake_site_reset();fake_site_add(SOURCE,page);
    fake_site_add("http://fixture.test/async/slow.js","var oldRealmRan=1;events.push('slow');if(document.currentScript.id!=='slow')badCurrent++;");
    fake_site_add("http://fixture.test/async/ordered.js","events.push('ordered');if(document.currentScript.id!=='ordered')badCurrent++;");
    fake_site_add("http://fixture.test/async/chain.js","events.push('chain');if(document.currentScript.id!=='chain')badCurrent++;");
    fake_site_add("http://fixture.test/async/alternate.js","var alternateRan=1;");
    fake_site_add(TARGET,"<!doctype html><body>DESTINATION</body>");
    tabs_set_store(&memfs);char session[512];snprintf(session,sizeof session,"logit-browser-session\t1\t0\n0\t%s\tfixture\t0\n",SOURCE);
    memfs_write(SESSION_PATH,session,(int)strlen(session));post(EV_KEY,'\n',0,0,0);
    if(setjmp(host_exit_jmp)==0)app_main();
    if(!strcmp(mode,"close"))finish();
    CHECK(finished,"real event loop reached all held-resource observations");
    CHECK(host_exited&&host_exit_code==0,"native browser close exits normally");
    printf("inserted-script-async %s: %s polls=%d pumps=%d parks=%d fallback=%d cancelled=%d taken=%d\n",mode,fail?"FAIL":"PASS",polls,held_pumps,pending_parks,held_fallback,held_cancelled,held_taken);
    return fail?1:0;
}
