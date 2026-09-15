/* Actual browser app_main owns install order, HTML parsing, script evaluation
 * and teardown. Capture the CHILD's console side effect; contentWindow is still
 * a parent-realm stand-in and therefore cannot observe child global writes. */
#define main loader_original_main
#include "loader_test.c"
#undef main
#include <unistd.h>
void app_main(void);
static int steps,seen,closing;
static int read_int(const char *source){
 JSContext *ctx=js_page_ctx();if(!ctx)return -999;
 JSValue v=JS_Eval(ctx,source,strlen(source),"<frame-bootstrap-observer>",JS_EVAL_TYPE_GLOBAL);
 int32_t n=-999;if(JS_IsException(v)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}else JS_ToInt32(ctx,&n,v);
 JS_FreeValue(ctx,v);return n;
}
static void post(int type){struct logit_event e={0};e.type=type;if(type==EV_KEY)e.a='\n';host_post_event(&e);}
void loader_poll_hook(void){
 host_clock+=5;if(closing)return;
 if(read_int("typeof frameAuditReady!=='undefined'&&frameAuditReady")>0){
   seen=1;closing=1;CHECK(read_int("parentStable")==1,"two frame documents preserve the parent DOM");
   CHECK(read_int("sandboxRefused")==4,"sandboxed blank frames refuse access through all source forms");post(EV_CLOSE);return;
 }
 if(++steps>1800){CHECK(0,"frame bootstrap fixture completed within bounded polls");closing=1;post(EV_CLOSE);}
}
int main(int argc,char **argv){
 if(argc!=2)return 2;FILE *fp=fopen(argv[1],"rb");if(!fp)return 2;
 fseek(fp,0,SEEK_END);long len=ftell(fp);rewind(fp);char *page=malloc((size_t)len+1);if(!page)return 2;
 if(fread(page,1,(size_t)len,fp)!=(size_t)len)return 2;fclose(fp);page[len]=0;
 fake_site_reset();fake_site_add("http://fixture.test/frame-bootstrap.html",page);tabs_set_store(&memfs);
 const char *session="logit-browser-session\t1\t0\n0\thttp://fixture.test/frame-bootstrap.html\tfixture\t0\n";
 memfs_write(SESSION_PATH,session,strlen(session));post(EV_KEY);
 FILE *capture=tmpfile();if(!capture)return 2;fflush(stdout);int saved=dup(STDOUT_FILENO);if(saved<0)return 2;
 dup2(fileno(capture),STDOUT_FILENO);
 if(setjmp(host_exit_jmp)==0)app_main();
 fflush(stdout);dup2(saved,STDOUT_FILENO);close(saved);
 fseek(capture,0,SEEK_END);long outlen=ftell(capture);rewind(capture);char *output=malloc((size_t)outlen+1);if(!output)return 2;
 if(fread(output,1,(size_t)outlen,capture)!=(size_t)outlen)return 2;output[outlen]=0;fclose(capture);fputs(output,stdout);
 const char *mark="[frame] FRAME-WIDGET-RAN count=1";int ran=0;for(char *p=output;(p=strstr(p,mark));p+=strlen(mark))ran++;
 printf("frame bootstrap actual child executions=%d expected=2\n",ran);
 CHECK(ran==2,"parser and dynamic srcdoc both execute the same child script exactly once");
 CHECK(strstr(output,"[frame] FRAME-SANDBOX-BYPASS-RAN")==0,"sandboxed blank frames never execute the injection probe");
 CHECK(seen&&host_exited&&host_exit_code==0,"real frame browser loop completes and closes normally");
 free(output);free(page);printf("frame-bootstrap-wiring: %s\n",fail?"FAIL":"PASS");return fail?1:0;
}
