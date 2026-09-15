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
#include "bfetch.h"
void app_main(void);
static int stage,polls,finished,embed_requests,resource_requests,mutated;
static const char *mode;
static const char *parent="https://parent.test/page.html";
static int blocked;
/* Policy-bearing response metadata is a transport fixture, not a production
 * fallback. Native transport separately tests all raw header occurrences. */
int bfetch_response_policy_known(int id){(void)id;return strcmp(mode,"unknown")!=0;}
const char *bfetch_response_header(int id,const char *name){
 const char *u=bfetch_url(id);
 if(!strcmp(name,"content-type")&&!strcmp(mode,"bad-mime")&&strstr(u,"child.html"))return "text/html-unknown";
 if(!strcmp(name,"content-type"))return strstr(u,".css")?"text/css":strstr(u,".svg")?"image/svg+xml":"text/html";
 if(!strcmp(name,"content-security-policy")){
  if(!strcmp(mode,"parent-csp")&&!strcmp(u,parent))return "frame-src 'none'";
  if(!strcmp(mode,"base-blocked")&&strstr(u,"child.html"))return "base-uri 'none'";
  if(!strcmp(mode,"allowed-csp")&&strstr(u,"child.html"))return "script-src 'none'; style-src 'unsafe-inline' https:; img-src https:";
  return "";
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
static void finish(void){finished=1;CHECK(!strcmp(js_page_location(),parent),"embedding never navigates or replaces the parent document");CHECK(tabs_count()==1,"embedding never opens a second tab");CHECK(textop("PARENT-RETAINED")!=0,"parent still paints after the child context is restored");CHECK(expr("document.querySelector('#inside')===null && typeof childRan==='undefined'"),"child DOM and scripts do not enter the parent realm");CHECK(fake_site_fetched("never.js")==0&&fake_site_fetched("nested.html")==0,"child scripts and nested documents never fetch");
 if(!blocked){CHECK(textop("FRAME-CONTENT")!=0,"actual child document text reaches embedded paint");CHECK(image_painted(),"actual decoded child image reaches embedded paint");if(textop("FRAME-CONTENT"))CHECK((textop(PASSIVE_FRAME_SCRIPT_NOTICE)!=0)==(strcmp(mode,"static")!=0),"native preview notice appears only for documents with scripts");CHECK(embed_requests==1&&resource_requests==2,"one child document and its independent stylesheet and image were fetched");const struct paintop *p=textop("FRAME-CONTENT");CHECK(!p||(p->x>=42&&p->x<242&&p->y>=162&&p->y<302),"child text is translated inside the iframe viewport");}
 else CHECK(textop("FRAME-CONTENT")==0&&image_painted()==0,"policy-refused frame exposes no child pixels");
 CHECK(!textop("FORGED-FILE")&&!textop("SECRET-MARKUP"),"passive file and password controls never paint markup values");CHECK(expr("(function(){try{return document.getElementById('f').contentDocument===null}catch(e){return true}})()"),"passive document has no script-visible child DOM");stage=99;post(EV_CLOSE);}
void loader_poll_hook(void){host_clock+=25;if(finished)return;if(++polls>160){finish();return;}
 if(stage==0){if(!textop("PARENT-RETAINED"))return;
  if(!strcmp(mode,"geometry")){CHECK(embed_requests==0,"hidden frame waits for an actual viewport");CHECK(expr("document.getElementById('f').style.display='block';true"),"hidden frame can become visible");post(EV_RESIZE);}
  stage=1;}
 if(stage==1&&polls>25&&!passive_frames_pending()&&!mutated&&(!strcmp(mode,"meta-tighten")||!strcmp(mode,"remove"))){
  CHECK(textop("FRAME-CONTENT")!=0,"lifecycle fixture first observes actual child pixels");mutated=polls;
  CHECK(expr(!strcmp(mode,"remove")?"document.getElementById('f').remove();true":"var m=document.createElement('meta');m.setAttribute('http-equiv','Content-Security-Policy');m.setAttribute('content',\"frame-src 'none'\");document.head.appendChild(m);true"),"parent mutation succeeds");
  paint_nops=0;stage=3;post(EV_RESIZE);return;
 }
 if(stage==3&&polls>mutated+6){paint_nops=0;post(EV_RESIZE);stage=2;return;}
 if(stage==1&&polls>25&&!passive_frames_pending()){paint_nops=0;post(EV_RESIZE);stage=2;return;}
 /* poll_event can reenter the hook while draining the SAME event burst,
  * before redraw. Observe actual parent paint after the resize, not merely an
  * empty event queue; otherwise the recorder is cleared and every pixel test
  * fails before the browser gets its chance to paint. */
 if(stage==2&&host_evq_head==host_evq_tail&&textop("PARENT-RETAINED"))finish();}
int main(int argc,char **argv){mode=argc>1?argv[1]:"paint";blocked=!strcmp(mode,"xfo")||!strcmp(mode,"sandbox")||!strcmp(mode,"parent-csp")||!strcmp(mode,"unknown")||!strcmp(mode,"bad-mime")||!strcmp(mode,"meta-tighten")||!strcmp(mode,"remove");
 char page[900];snprintf(page,sizeof page,"<!doctype html><style>body{margin:0}iframe{position:absolute;left:40px;top:100px;width:200px;height:140px;border:2px solid blue;%s}</style><body>PARENT-RETAINED<iframe id=f src='https://child.test/child.html' %s></iframe>",!strcmp(mode,"geometry")?"display:none":"",!strcmp(mode,"sandbox")?"sandbox=''":"");
 const char *child="<!doctype html><head><base href='https://child.test/'><link rel=stylesheet href='/child.css'></head><body><span id=inside>FRAME-CONTENT</span><img src='/qr.svg' width=32 height=32><input type=file value=FORGED-FILE><input type=password value=SECRET-MARKUP><video></video><canvas width=2 height=2></canvas><script>childRan=true</script><script src='/never.js'></script><iframe src='/nested.html'></iframe></body>";
 fake_site_reset();fake_site_add(parent,page);char child_page[1800];snprintf(child_page,sizeof child_page,"%s%s",!strcmp(mode,"base-blocked")?"<base href='https://forbidden.test/'>":"",child);if(!strcmp(mode,"static")){char *a;while((a=strstr(child_page,"<script"))!=0){char *b=strstr(a,"</script>");if(!b)break;memmove(a,b+9,strlen(b+9)+1);}}fake_site_add("https://child.test/child.html",child_page);fake_site_add("https://child.test/child.css","body{margin:0}#inside{display:block;color:#c02020;font-size:12px}");fake_site_add("https://child.test/qr.svg","<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><rect width='32' height='32' fill='white'/><path d='M0 0h12v12H0zM20 0h12v12H20zM0 20h12v12H0zM18 18h8v8h-8z' fill='black'/></svg>");
 tabs_set_store(&memfs);char session[300];snprintf(session,sizeof session,"logit-browser-session\t1\t0\n0\t%s\tframe\t0\n",parent);memfs_write(SESSION_PATH,session,strlen(session));struct logit_event e={0};e.type=EV_KEY;e.a='\n';host_post_event(&e);
 if(setjmp(host_exit_jmp)==0)app_main();CHECK(finished,"bounded app_main completed iframe observations");passive_frames_reset();printf("passive-frame %s: %s\n",mode,fail?"FAIL":"PASS");return fail?1:0;}
