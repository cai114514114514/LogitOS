/* Local browser fixture with actual SVG rasterization and GUI draw records.
 * HTTP alone is mocked. The native passive loader cannot borrow parent fetch
 * or change location to satisfy these observations. */
#define main loader_existing_main
#define img_decode loader_unused_img_decode
#define img_free loader_unused_img_free
#define img_init loader_unused_img_init
#include "loader_test.c"
#undef main
#undef img_decode
#undef img_free
#undef img_init
#include "passive_frame.h"
#include "js_dom.h"
#include "bfetch.h"
void app_main(void);
static int stage,polls,finished,embed_requests,resource_requests,mutated;
static const char *mode;
static const char *parent="https://parent.test/page.html";
static int blocked;
static char *large_fixture;
static int active(void){return !strncmp(mode,"active",6);}
static int port_mode(void){return !strncmp(mode,"active-ports",12);}
static int restore_mode(void){return !strcmp(mode,"active-ports-restore");}
static int restored;
static void policy_retention_checks(void)
{
 struct tab t={0};t.used=1;size_t initial=tab_retained_bytes(&t);
 char policy[]="frame-src 'none'";
 tab_keep_document_policy(&t,policy,1);policy[0]='x';
 CHECK(t.document_policy_known&&!strcmp(t.document_csp,"frame-src 'none'"),"tab policy owns a copy of response bytes");
 CHECK(tab_retained_bytes(&t)==initial+strlen(t.document_csp)+1,"retained byte count includes response policy allocation");
 tab_keep_document_policy(&t,t.document_csp,1);
 CHECK(t.document_policy_known&&!strcmp(t.document_csp,"frame-src 'none'"),"retaining an existing policy handles aliasing");
 tab_keep_document_policy(&t,"",0);
 CHECK(!t.document_policy_known&&!t.document_csp,"unknown response policy stays unknown");
 char too_long[TAB_DOCUMENT_CSP_MAX+1];memset(too_long,'x',sizeof too_long-1);too_long[sizeof too_long-1]=0;
 tab_keep_document_policy(&t,too_long,1);
 CHECK(!t.document_policy_known&&!t.document_csp,"oversized response policy is not truncated into permission");
 tab_keep_document_policy(&t,"",1);
 CHECK(t.document_policy_known&&t.document_csp&&!t.document_csp[0],"known empty response policy differs from missing metadata");
 tab_drop_content(&t);
 CHECK(!t.document_policy_known&&!t.document_csp&&tab_retained_bytes(&t)==initial,"dropping document content releases and invalidates its policy");
}
/* Policy-bearing response metadata is a transport fixture, not a production
 * fallback. Native transport separately tests all raw header occurrences. */
int bfetch_response_policy_known(int id){(void)id;return strcmp(mode,"unknown")!=0;}
const char *bfetch_response_header(int id,const char *name){
 const char *u=bfetch_url(id);
 if(!strcmp(name,"content-type")&&!strcmp(mode,"bad-mime")&&strstr(u,"child.html"))return "text/html-unknown";
 if(!strcmp(name,"content-type"))return strstr(u,".css")?"text/css":strstr(u,".svg")?"image/svg+xml":strstr(u,".js")?"text/javascript":"text/html";
 if(!strcmp(name,"content-security-policy")){
  if(restore_mode()&&!strcmp(u,parent))return "frame-src https://child.test";
  if(restore_mode()&&strstr(u,"other.html"))return "frame-src 'none'";
  if(!strcmp(mode,"parent-csp")&&!strcmp(u,parent))return "frame-src 'none'";
  if(!strcmp(mode,"base-blocked")&&strstr(u,"child.html"))return "base-uri 'none';script-src 'none'";
  if(!strcmp(mode,"allowed-csp")&&strstr(u,"child.html"))return "script-src 'none'; style-src 'unsafe-inline' https:; img-src https:";
  return strstr(u,"child.html")&&!active()?"script-src 'none'":"";
 }
 if(!strcmp(name,"x-frame-options"))return !strcmp(mode,"xfo")&&strstr(u,"child.html")?"SAMEORIGIN":"";
 return "";
}
int bfetch_start_embedded(const char *ref,const char *owner,const char *ancestor,bfetch_embedded_redirect redirect,void *context){
 char final[600];CHECK(redirect&&redirect(context,owner,ref,final,sizeof final),"embedded transport validates its initial redirect destination");
 CHECK(!strcmp(ancestor,parent),"every child resource keeps the top ancestor identity");
 if(!strcmp(owner,parent))embed_requests++;
 else{resource_requests++;CHECK(!strcmp(owner,"https://child.test/child.html"),"image and stylesheet requests use the child document owner");}
 return bfetch_start_from(owner,ref);
}
static int expr(const char *s){JSContext *c=js_page_ctx();if(!c)return 0;JSValue v=JS_Eval(c,s,strlen(s),"<passive-observer>",JS_EVAL_TYPE_GLOBAL);int ok=!JS_IsException(v)&&JS_ToBool(c,v);if(JS_IsException(v))JS_FreeValue(c,JS_GetException(c));JS_FreeValue(c,v);return ok;}
static const struct paintop *textop(const char *s){for(int i=paint_nops-1;i>=0;i--)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==(int)strlen(s)&&!memcmp(paint_ops[i].text,s,strlen(s)))return &paint_ops[i];return 0;}
static int image_painted(void){for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_BLIT&&paint_ops[i].w==32&&paint_ops[i].h==32)return 1;return 0;}
static void post(int t){struct logit_event e={0};e.type=t;if(t==EV_RESIZE){e.a=1180;e.b=620;}host_post_event(&e);}
static void finish(void){finished=1;CHECK(!strcmp(js_page_location(),parent),"embedding never navigates or replaces the parent document");CHECK(tabs_count()==(restore_mode()?2:1),"embedding never opens a second tab");CHECK(textop("PARENT-RETAINED")!=0,"parent still paints after the child context is restored");CHECK(expr("document.querySelector('#inside')===null && typeof childRan==='undefined'"),"child DOM and scripts do not enter the parent realm");CHECK(fake_site_fetched("never.js")==0&&fake_site_fetched("nested.html")==0,"policy-blocked scripts and nested documents never fetch");
 if(active()){
  CHECK(textop("FRAME-MESSAGE")!=0,"native child click and bidirectional message repaint embedded pixels");
  CHECK(expr("got===1 && bad===0 && sourceOK"),"parent receives exact child origin and stable source identity");
  CHECK(!js_dom_has_activation(),"window messages do not grant parent user activation");
  CHECK(fake_site_fetched("active.js")== (restore_mode()?2:1),"external classic child script loaded once per document runtime");
  if(restore_mode()){
   CHECK(restored==2&&fake_site_fetched("page.html")==1,"restored tab reuses its original document bytes");
   CHECK(tab_cur()->document_policy_known&&tab_cur()->document_csp&&!strcmp(tab_cur()->document_csp,"frame-src https://child.test"),"restored tab retains its own response policy");
  }
  CHECK(expr("document.getElementById('f').contentWindow===savedWindow"),"frame WindowProxy identity survives child load");
  CHECK(expr("savedWindow.postMessage('wake','https://child.test');true"),"idle parent queues another window message");
  CHECK(passive_frames_pending(),"queued window messages wake an otherwise idle browser");
  stage=99;post(EV_CLOSE);return;
 }
 if(!blocked){CHECK(textop("FRAME-CONTENT")!=0,"actual child document text reaches embedded paint");CHECK(image_painted(),"actual decoded child image reaches embedded paint");if(textop("FRAME-CONTENT"))CHECK((textop(PASSIVE_FRAME_SCRIPT_NOTICE)!=0)==(strcmp(mode,"static")!=0),"native preview notice appears only for documents with scripts");CHECK(embed_requests==1&&resource_requests==2,"one child document and its independent stylesheet and image were fetched");const struct paintop *p=textop("FRAME-CONTENT");CHECK(!p||(p->x>=42&&p->x<242&&p->y>=162&&p->y<302),"child text is translated inside the iframe viewport");}
 else CHECK(textop("FRAME-CONTENT")==0&&image_painted()==0,"policy-refused frame exposes no child pixels");
 CHECK(!textop("FORGED-FILE")&&!textop("SECRET-MARKUP"),"passive file and password controls never paint markup values");CHECK(expr("(function(){try{return document.getElementById('f').contentDocument===null}catch(e){return true}})()"),"passive document has no script-visible child DOM");stage=99;post(EV_CLOSE);}
void loader_poll_hook(void){host_clock+=25;if(finished||restored==1)return;if(++polls>800){finish();return;}
 if(active()){
  if(restore_mode()&&!restored&&textop("FRAME-MESSAGE")&&!js_page_entry_active()){
   /* Reenter only after native callbacks unwind, and fence nested load polls.
    * The other tab has a different policy to catch accidental global reuse. */
   restored=1;int original=tabs_active();int other=tabs_new("https://other.test/other.html");
   browser_tab_switch(other);CHECK(tab_cur()->document_policy_known&&tab_cur()->document_csp&&!strcmp(tab_cur()->document_csp,"frame-src 'none'"),"other tab installs an independent restrictive response policy");
   paint_nops=0;mutated=0;browser_tab_switch(original);paint_nops=0;restored=2;post(EV_RESIZE);return;
  }
  const struct paintop *p=textop("FRAME-READY");
  if(!mutated&&p){mutated=1;struct logit_event e={0};e.type=EV_MOUSE;e.button=EV_BTN_LEFT;e.a=p->x+8;e.b=p->y+5;host_post_event(&e);e.type=EV_MOUSE_UP;host_post_event(&e);}
  if(textop("FRAME-MESSAGE")){finish();return;}return;
 }
 if(stage==0){if(!textop("PARENT-RETAINED")||!js_page_live()||js_page_entry_active()||
     !expr("typeof document==='object' && !!document.getElementById('f')"))return;
  if(!strcmp(mode,"geometry")){CHECK(embed_requests==0,"hidden frame waits for an actual viewport");CHECK(expr("document.getElementById('f').style.display='block';true"),"hidden frame can become visible");post(EV_RESIZE);}
  stage=1;}
 if(stage==1&&embed_requests==1&&resource_requests>=2&&!passive_frames_pending()&&!mutated&&(!strcmp(mode,"meta-tighten")||!strcmp(mode,"remove"))){
  CHECK(textop("FRAME-CONTENT")!=0,"lifecycle fixture first observes actual child pixels");mutated=polls;
  CHECK(expr(!strcmp(mode,"remove")?"document.getElementById('f').remove();true":"var m=document.createElement('meta');m.setAttribute('http-equiv','Content-Security-Policy');m.setAttribute('content',\"frame-src 'none'\");document.head.appendChild(m);true"),"parent mutation succeeds");
  paint_nops=0;stage=3;post(EV_RESIZE);return;
 }
 if(stage==3&&polls>mutated+6){paint_nops=0;post(EV_RESIZE);stage=2;return;}
 /* Initial load can poll input after its first paint but before the outer
  * iframe pump. An empty pending queue there is not completion. Require the
  * actual independent resource closure, including in the no-paint control. */
 if(stage==1&&polls>25&&!passive_frames_pending()&&
    ((blocked&&strcmp(mode,"meta-tighten")&&strcmp(mode,"remove"))||(embed_requests==1&&resource_requests>=2))){paint_nops=0;post(EV_RESIZE);stage=2;return;}
 /* poll_event can reenter the hook while draining the SAME event burst,
  * before redraw. Observe actual parent paint after the resize, not merely an
  * empty event queue; otherwise the recorder is cleared and every pixel test
  * fails before the browser gets its chance to paint. */
 if(stage==2&&host_evq_head==host_evq_tail&&textop("PARENT-RETAINED"))finish();}
int main(int argc,char **argv){mode=argc>1?argv[1]:"paint";blocked=!strcmp(mode,"xfo")||!strcmp(mode,"sandbox")||!strcmp(mode,"parent-csp")||!strcmp(mode,"unknown")||!strcmp(mode,"bad-mime")||!strcmp(mode,"meta-tighten")||!strcmp(mode,"remove");
 char page[2000];snprintf(page,sizeof page,"<!doctype html><style>body{margin:0}iframe{position:absolute;left:40px;top:100px;width:200px;height:140px;border:2px solid blue;%s}</style><body>PARENT-RETAINED<iframe id=f src='https://child.test/child.html' %s></iframe>",!strcmp(mode,"geometry")?"display:none":"",!strcmp(mode,"sandbox")?"sandbox=''":"");
 if(active())strcat(page,"<script>var got=0,bad=0,sourceOK=false;var savedWindow=document.getElementById('f').contentWindow;addEventListener('message',function(e){if(e.data==='wrong'){bad++;return;}if(e.data==='clicked'&&e.origin==='https://child.test'){got++;sourceOK=e.source===savedWindow;e.source.postMessage('reply','https://child.test');}});</script>");
 if(restore_mode())policy_retention_checks();
 if(port_mode()){
  /* Same native input and paint consumer, but no ordinary Window reply can
   * finish it: the reply must use a transferred recipient-owned endpoint. */
  *strstr(page,"<script>")=0;
  strcat(page,"<script>var got=0,bad=0,sourceOK=false,remote;var savedWindow=document.getElementById('f').contentWindow;"
    "addEventListener('message',function(e){if(e.data==='wrong'){bad++;return;}"
    "if(e.data!=='handoff'||e.origin!=='https://child.test')return;sourceOK=e.source===savedWindow;"
    "if(!Object.isFrozen(e.ports)||e.ports.length!==1||!(e.ports[0] instanceof MessagePort))throw Error('ports');"
    "remote=e.ports[0];remote.onmessage=function(e){if(e.data==='clicked'&&e.isTrusted&&e.target===remote){got++;remote.postMessage('reply')}};remote.postMessage('ready');});</script>");
 }
 const char *child="<!doctype html><head><base href='https://child.test/'><link rel=stylesheet href='/child.css'></head><body><span id=inside>FRAME-CONTENT</span><img src='/qr.svg' width=32 height=32><input type=file value=FORGED-FILE><input type=password value=SECRET-MARKUP><video></video><canvas width=2 height=2></canvas><script>childRan=true</script><script src='/never.js'></script><iframe src='/nested.html'></iframe></body>";
 fake_site_reset();fake_site_add(parent,page);char child_page[1800];snprintf(child_page,sizeof child_page,"%s%s",!strcmp(mode,"base-blocked")?"<base href='https://forbidden.test/'>":"",child);if(!strcmp(mode,"static")){char *a;while((a=strstr(child_page,"<script"))!=0){char *b=strstr(a,"</script>");if(!b)break;memmove(a,b+9,strlen(b+9)+1);}}if(active())strcpy(child_page,"<!doctype html><style>body{margin:0}#inside{display:block;width:190px;height:70px;background:#9f9}</style><body><div id=inside>FRAME-PENDING</div><script src='/active.js'></script></body>");fake_site_add("https://child.test/child.html",child_page);fake_site_add("https://child.test/child.css","body{margin:0}#inside{display:block;color:#c02020;font-size:12px}");fake_site_add("https://child.test/qr.svg","<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><rect width='32' height='32' fill='white'/><path d='M0 0h12v12H0zM20 0h12v12H20zM0 20h12v12H0zM18 18h8v8h-8z' fill='black'/></svg>");
 fake_site_add("https://child.test/active.js","var childRan=true;var box=document.getElementById('inside');box.textContent='FRAME-READY';box.addEventListener('click',function(e){if(!e.isTrusted||e.clientX<0||e.clientX>=200)throw Error('input coordinates');parent.postMessage('wrong','https://not-parent.test');parent.postMessage('clicked','https://parent.test');});addEventListener('message',function(e){if(e.origin==='https://parent.test'&&e.source===parent&&e.data==='reply')box.textContent='FRAME-MESSAGE';});");
 if(port_mode()){
  fake_site_reset();fake_site_add(parent,page);fake_site_add("https://child.test/child.html",child_page);
  fake_site_add("https://child.test/active.js",
    "var box=document.getElementById('inside'),channel=new MessageChannel(),discard=new MessageChannel();"
    "parent.postMessage('wrong','https://not-parent.test',[discard.port2]);"
    "channel.port1.onmessage=function(e){if(e.data==='ready')box.textContent='FRAME-READY';"
    "if(e.data==='reply')box.textContent='FRAME-MESSAGE'};"
    "parent.postMessage('handoff','https://parent.test',[channel.port2]);"
    "box.addEventListener('click',function(e){if(!e.isTrusted)throw Error('input');channel.port1.postMessage('clicked')});");
  if(restore_mode())fake_site_add("https://other.test/other.html","<!doctype html><p>OTHER-POLICY-TAB</p>");
 }
 if(!strcmp(mode,"active-large")){
  /* The sentinel is AFTER the former ceiling, so a truncated successful
   * parse cannot satisfy the exact same click/message/pixel observations. */
  int padding=1024*1024;const char *code="var box=document.getElementById('inside');box.textContent='FRAME-READY';box.addEventListener('click',function(){parent.postMessage('clicked','https://parent.test')});addEventListener('message',function(e){if(e.origin==='https://parent.test'&&e.source===parent&&e.data==='reply')box.textContent='FRAME-MESSAGE'});";
  fake_site_reset();fake_site_add(parent,page);fake_site_add("https://child.test/child.html",child_page);
  large_fixture=malloc((size_t)padding+strlen(code)+5);large_fixture[0]='/';large_fixture[1]='*';memset(large_fixture+2,'x',padding);strcpy(large_fixture+padding+2,"*/");strcpy(large_fixture+padding+4,code);fake_site_add("https://child.test/active.js",large_fixture);
 }
 tabs_set_store(&memfs);char session[300];snprintf(session,sizeof session,"logit-browser-session\t1\t0\n0\t%s\tframe\t0\n",parent);memfs_write(SESSION_PATH,session,strlen(session));struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();CHECK(finished,"bounded app_main completed iframe observations");passive_frames_reset();free(large_fixture);printf("passive-frame %s: %s\n",mode,fail?"FAIL":"PASS");return fail?1:0;}
