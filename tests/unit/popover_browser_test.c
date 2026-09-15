/* Run the guest fixture through actual browser.c app_main/restyle/CSSOM hook.
 * The earlier runtime+paint tests manually called layout_page and therefore
 * could not catch a paint-only mark being discarded as CSS_CHANGED_NONE. */
#define main loader_original_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void app_main(void);
static int steps,seen,closing,stage;
static int js_int(const char *source){
 JSContext *ctx=js_page_ctx();if(!ctx)return -999;
 JSValue v=JS_Eval(ctx,source,strlen(source),"<popover-browser-observer>",JS_EVAL_TYPE_GLOBAL);
 int32_t n=-999;if(JS_IsException(v)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}else JS_ToInt32(ctx,&n,v);
 JS_FreeValue(ctx,v);return n;
}
static void post(int type){struct logit_event e={0};e.type=type;if(type==EV_KEY)e.a='\n';host_post_event(&e);}
static int mouse_x,mouse_y;
static int mouse_at(const char *word,int type){
 if(type==EV_MOUSE) {
  /* The fixture repeats Outside in panel prose. First painted occurrence
   * is the external button; reverse search silently clicks inside the layer. */
  int found=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==(int)strlen(word)&&!memcmp(paint_ops[i].text,word,strlen(word))){mouse_x=paint_ops[i].x+3;mouse_y=paint_ops[i].y+3;found=1;break;}
  if(!found)return 0;
 }
 struct logit_event e={0};e.type=type;e.a=mouse_x;e.b=mouse_y;e.button=EV_BTN_LEFT;host_post_event(&e);return 1;
}
void loader_poll_hook(void){
 host_clock+=5;if(closing)return;
 if(!stage&&js_int("typeof checks==='number'&&checks===8")>0){
   seen=1;stage=1;
   int failures=js_int("failures.length");
   printf("popover browser guest-fixture: checks=8 failures=%d\n",failures);
   CHECK(failures==0,"first synchronous popover show produces actual browser geometry");
   const char *async="window.asyncToggleStates=[];panel.addEventListener('toggle',function(e){asyncToggleStates.push(e.oldState+'>'+e.newState)});var cmd=document.createElement('button');cmd.textContent='Command';cmd.commandForElement=panel;cmd.command='toggle-popover';document.body.appendChild(cmd);";
   CHECK(js_page_eval(async,strlen(async),"<popover-task-check>",0),"asynchronous toggle listener installed");
 }
 if(stage==1&&mouse_at("Toggle",EV_MOUSE)){stage=2;return;}
 if(stage==2){mouse_at("Toggle",EV_MOUSE_UP);stage=3;return;}
 if(stage==3&&js_int("asyncToggleStates.length")>=1){
   CHECK(js_int("panel.matches(':popover-open')&&asyncToggleStates[0]==='closed>open'")==1,"native opener click opens and delivers toggle");
   if(mouse_at("Outside",EV_MOUSE)){stage=4;return;}
 }
 if(stage==4){mouse_at("Outside",EV_MOUSE_UP);stage=5;return;}
 if(stage==5&&js_int("asyncToggleStates.length")>=2){
   CHECK(js_int("!panel.matches(':popover-open')&&asyncToggleStates[1]==='open>closed'")==1,"native outside click dismisses and delivers toggle");
   if(mouse_at("Toggle",EV_MOUSE)){stage=6;return;}
 }
 if(stage==6){mouse_at("Toggle",EV_MOUSE_UP);stage=7;return;}
 if(stage==7&&js_int("asyncToggleStates.length")>=3){
   CHECK(js_int("panel.matches(':popover-open')")==1,"native second opener click reopens");
   struct logit_event e={0};e.type=EV_KEY;e.a=27;host_post_event(&e);stage=8;return;
 }
 if(stage==8&&js_int("asyncToggleStates.length")>=4){
   CHECK(js_int("asyncToggleStates.length===4&&!panel.matches(':popover-open')&&asyncToggleStates[3]==='open>closed'")==1,"native Escape closes with exactly four lifecycle toggle events");
   struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);stage=9;return;
 }
 if(stage==9&&js_int("asyncToggleStates.length")>=5){
   CHECK(js_int("panel.matches(':popover-open')")==1,"native keyboard activates popover invoker");
   struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);stage=10;return;
 }
 if(stage==10&&js_int("asyncToggleStates.length")>=6){
   CHECK(js_int("!panel.matches(':popover-open')")==1,"native keyboard toggles popover closed once");
   if(mouse_at("Command",EV_MOUSE)){stage=11;return;}
 }
 if(stage==11){mouse_at("Command",EV_MOUSE_UP);stage=12;return;}
 if(stage==12&&js_int("asyncToggleStates.length")>=7){
   CHECK(js_int("panel.matches(':popover-open')")==1,"native command invoker reaches shared default action");
   const char *cancel="cmd.addEventListener('click',function(e){e.preventDefault()},{once:true})";
   js_page_eval(cancel,strlen(cancel),"<cancel-native-invoker>",0);
   if(mouse_at("Command",EV_MOUSE)){stage=13;return;}
 }
 if(stage==13){mouse_at("Command",EV_MOUSE_UP);stage=14;return;}
 if(stage==14){
   CHECK(js_int("panel.matches(':popover-open')&&asyncToggleStates.length===7")==1,"canceled native click runs no invoker default or duplicate toggle");
   struct logit_event e={0};e.type=EV_KEY;e.a=27;host_post_event(&e);stage=15;return;
 }
 if(stage==15&&js_int("asyncToggleStates.length")>=8){
   CHECK(js_int("!panel.matches(':popover-open')&&asyncToggleStates.length===8")==1,"native input sequence completes with exact lifecycle count");
   closing=1;post(EV_CLOSE);return;
 }
 if(++steps>1800){printf("popover native stage=%d toggle count=%d pending=%d\n",stage,js_int("typeof asyncToggleStates==='undefined'?-1:asyncToggleStates.length"),js_page_pending());CHECK(0,"native popover input and lifecycle complete within bounded polls");closing=1;post(EV_CLOSE);}
}
int main(int argc,char **argv){
 if(argc!=2)return 2;FILE *fp=fopen(argv[1],"rb");if(!fp)return 2;
 fseek(fp,0,SEEK_END);long len=ftell(fp);rewind(fp);char *page=malloc((size_t)len+1);if(!page)return 2;
 if(fread(page,1,(size_t)len,fp)!=(size_t)len)return 2;fclose(fp);page[len]=0;
 fake_site_reset();fake_site_add("http://fixture.test/popover.html",page);tabs_set_store(&memfs);
 const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/popover.html\tfixture\t0\n";
 memfs_write(SESSION_PATH,session,strlen(session));post(EV_KEY);
 if(setjmp(host_exit_jmp)==0)app_main();
 CHECK(host_exited&&host_exit_code==0,"popover browser event loop exits normally");
 CHECK(seen,"ordinary guest fixture executed in actual browser runtime");free(page);
 printf("popover browser: %s\n",fail?"FAIL":"PASS");return fail?1:0;
}
