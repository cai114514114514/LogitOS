/* Real app_main mouse events, production DOM/layout/painter/URL parser; only
 * HTTP and GUI syscalls are host fixtures. Synthetic clicks must never acquire
 * this native capability. No live login endpoint is contacted by this gate. */
#define main loader_existing_main
#include "loader_test.c"
#undef main
#include "frame_open.h"
void app_main(void);
static int stage,polls,finished,clicked,cx,cy,source_tab,positive;
static const char *mode;
static const char *source="http://fixture.test/dir/source.html";
static const char *target="https://other.test/qr?state=a%2Bb%2Fc&redirect_uri=https%3A%2F%2Ffixture.test%2Freturn";
static void post(int type,int a,int b,int button,int mods){struct logit_event e={0};e.type=type;e.a=a;e.b=b;e.button=button;e.mods=mods;host_post_event(&e);}
static int expr(const char *s){JSContext *c=js_page_ctx();if(!c)return 0;JSValue v=JS_Eval(c,s,strlen(s),"<frame-observer>",JS_EVAL_TYPE_GLOBAL);int r=!JS_IsException(v)&&JS_ToBool(c,v);if(JS_IsException(v)){JSValue x=JS_GetException(c);JS_FreeValue(c,x);}JS_FreeValue(c,v);return r;}
static const struct paintop *painted(const char *s){for(int i=paint_nops-1;i>=0;i--)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==(int)strlen(s)&&!memcmp(paint_ops[i].text,s,strlen(s)))return &paint_ops[i];return 0;}
static void finish(void){
 finished=1;
 CHECK(clicked,"trusted frame press and release delivered");
 CHECK(tabs_count()==1&&tabs_active()==source_tab,"frame opening preserves the existing tab partition");
 if(positive){
  CHECK(!strcmp(js_page_location(),target),"native frame action reaches the exact src destination");
  CHECK(!strcmp(fake_site_nav_initiator(),source),"frame navigation keeps committed source initiator");
  CHECK(painted("FRAME-DESTINATION")!=0,"frame destination reaches real painter");
  if(!strcmp(mode,"relative"))CHECK(expr("sessionStorage.getItem('framePartition')==='source-owned'"),"same-origin destination retains sessionStorage in the same tab");
 }else CHECK(!strcmp(js_page_location(),source),"blocked or cancelled frame action never navigates");
 stage=99;post(EV_CLOSE,0,0,0,0);
}
void loader_poll_hook(void){
 host_clock+=10;if(stage==99)return;
 if(++polls>150){finish();return;}
 if(stage==0){
  if(!painted("FRAME-SOURCE"))return;
  source_tab=tabs_active();
  CHECK(expr("typeof URL==='function'"),"production URL parser is installed in the action harness");
  const struct paintop *p=painted(FRAME_OPEN_LABEL);
  int affordance=strcmp(mode,"sandbox")&&strcmp(mode,"srcdoc")&&strcmp(mode,"data")&&strcmp(mode,"overflow");
  CHECK((p!=0)==affordance,"native button is discoverable only for eligible frame sources");
  cx=p?p->x+5:140;cy=p?p->y+5:260;
  CHECK(expr("sessionStorage.setItem('framePartition','source-owned');true"),"source stores session state before navigation");
  CHECK(expr("document.getElementById('frame').dispatchEvent(new MouseEvent('click',{bubbles:true}));true"),"synthetic click dispatch completes without native action");
  CHECK(!strcmp(js_page_location(),source),"synthetic click cannot open embedded page");
  post(EV_KEY,12,0,0,EV_MOD_CTRL);
  const char *draft="http://draft.test/wrong";for(;*draft;draft++)post(EV_KEY,*draft,0,0,0);
  stage=1;return;
 }
 if(stage==1){
  if(host_evq_head!=host_evq_tail)return;
  CHECK(!strcmp(js_page_location(),source),"unsubmitted address edit preserves document identity");
  /* Attach mutation handlers after the synthetic-event assertion: otherwise
   * that apparatus step would remove the frame before native input begins. */
  if(!strcmp(mode,"cancel"))CHECK(expr("document.getElementById('frame').onclick=function(e){e.preventDefault()};true"),"cancel handler installed");
  if(!strcmp(mode,"remove"))CHECK(expr("document.getElementById('frame').onclick=function(){this.remove()};true"),"remove handler installed");
  if(!strcmp(mode,"sandbox-late"))CHECK(expr("document.getElementById('frame').onclick=function(){this.setAttribute('sandbox','')};true"),"late sandbox handler installed");
  post(EV_MOUSE,cx,cy,EV_BTN_LEFT,0);stage=2;return;
 }
 if(stage==2){paint_nops=0;clicked=1;post(EV_MOUSE_UP,cx,cy,EV_BTN_LEFT,0);stage=3;return;}
 if(stage==3){
  if(host_evq_head!=host_evq_tail)return;
  if(positive && strcmp(js_page_location(),target))return;
  paint_nops=0;post(EV_RESIZE,1180,620,0,0);stage=4;return;
 }
 if(stage==4){if(host_evq_head!=host_evq_tail)return;if(positive&&!painted("FRAME-DESTINATION"))return;finish();}
}
int main(int argc,char **argv){
 mode=argc>1?argv[1]:"open";positive=!strcmp(mode,"open")||!strcmp(mode,"relative");
 const char *src=target,*attrs="";char large[900];
 if(!strcmp(mode,"relative")){src="../destination.html?state=a%2Bb";target="http://fixture.test/destination.html?state=a%2Bb";}
 if(!strcmp(mode,"sandbox"))attrs="sandbox=''";
 if(!strcmp(mode,"srcdoc"))attrs="srcdoc=''";
 if(!strcmp(mode,"data"))src="data:text/html,local";
 if(!strcmp(mode,"overflow")||!strcmp(mode,"path-limit")){memset(large,'x',sizeof large);memcpy(large,"https://other.test/",19);large[!strcmp(mode,"overflow")?800:540]=0;src=large;}
 if(!strcmp(mode,"credentials"))src="https://user:pass@other.test/private";
 char page[1800];snprintf(page,sizeof page,"<!doctype html><style>body{margin:0}iframe{position:absolute;left:40px;top:140px;width:200px;height:200px;border:1px solid #888}</style><body>FRAME-SOURCE<iframe id=frame src='%s' %s></iframe>",src,attrs);
 fake_site_reset();fake_site_add(source,page);fake_site_add(target,"<!doctype html><body>FRAME-DESTINATION");
 tabs_set_store(&memfs);char session[300];snprintf(session,sizeof session,"logit-browser-session\t1\t0\n0\t%s\tframe\t0\n",source);memfs_write(SESSION_PATH,session,strlen(session));
 post(EV_KEY,'\n',0,0,0);if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(finished,"bounded event loop reached frame observations");
 CHECK(host_exited&&host_exit_code==0,"browser exits normally after frame action");
 printf("frame-open %s: %s\n",mode,fail?"FAIL":"PASS");return fail?1:0;
}
