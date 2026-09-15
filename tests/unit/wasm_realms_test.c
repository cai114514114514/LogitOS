/* SPDX-License-Identifier: MIT
 * Two actual QuickJS runtimes, only the checked-in add/memory/import modules.
 * Each case runs in a child so an old binding cannot turn a stale peer handle
 * into a host crash that masks the named failure. A failed safety guard stops
 * that case before reading any possibly freed bytes. Passing cases fully
 * reset/free both contexts and runtimes, including sanitizer runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "quickjs.h"
#include "js_wasm.h"
#include "wasm_js_modules.inc"

struct realm { JSRuntime *rt; JSContext *ctx; int owns_rt; };
static int checks;

static void put_bytes(JSContext *ctx, const char *name, const unsigned char *p, size_t n)
{
    JSValue g=JS_GetGlobalObject(ctx), b=JS_NewArrayBufferCopy(ctx,p,n);
    JS_SetPropertyStr(ctx,g,name,b); JS_FreeValue(ctx,g);
}
static struct realm open_realm(JSRuntime *shared)
{
    struct realm r={shared?shared:JS_NewRuntime(),NULL,!shared};
    if(!r.rt || !(r.ctx=JS_NewContext(r.rt))) _exit(2);
    js_wasm_install(r.ctx);
    put_bytes(r.ctx,"addBytes",W_add,sizeof W_add);
    put_bytes(r.ctx,"memBytes",W_mem,sizeof W_mem);
    put_bytes(r.ctx,"importBytes",W_imp,sizeof W_imp);
    return r;
}
static void close_realm(struct realm *r)
{
    js_wasm_reset(r->ctx); JS_FreeContext(r->ctx);
    if(r->owns_rt) JS_FreeRuntime(r->rt);
    r->ctx=NULL;
}
static int check(JSContext *ctx,const char *src,const char *name)
{
    checks++;
    JSValue v=JS_Eval(ctx,src,strlen(src),"<ordinary-wasm-realms>",JS_EVAL_TYPE_GLOBAL);
    int ok=!JS_IsException(v)&&JS_ToBool(ctx,v);
    if(JS_IsException(v)) JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);
    if(!ok) printf("FAIL CHECK: %s\n",name);
    return ok;
}
#define REQUIRE(r,src,name) do {if(!check((r).ctx,src,name))return 1;} while(0)
static const char *SETUP=
    "globalThis.addInst=new WebAssembly.Instance(new WebAssembly.Module(addBytes));"
    "globalThis.mem=new WebAssembly.Memory({initial:1,maximum:4});"
    "globalThis.buf=mem.buffer;new Uint8Array(buf)[0]=37;"
    "globalThis.imported=new WebAssembly.Instance(new WebAssembly.Module(importBytes),"
    "{env:{twice:function(x){return x*2;}}});"
    "addInst.exports.add(20,22)===42&&imported.exports.call4()===8";
static const char *SURVIVES=
    "addInst.exports.add(20,22)===42&&imported.exports.call4()===8&&"
    "mem.buffer===buf&&buf.byteLength===65536&&new Uint8Array(buf)[0]===37";

static int install_cache(void)
{
    struct realm a=open_realm(NULL);
    REQUIRE(a,SETUP,"cache setup");
    struct realm b=open_realm(NULL);
    REQUIRE(a,"mem.buffer===buf","peer install retains existing buffer identity");
    REQUIRE(a,SURVIVES,"peer install preserves add/import/memory");
    close_realm(&b);close_realm(&a);return 0;
}
static int install_defined_memory(void)
{
    struct realm a=open_realm(NULL);
    REQUIRE(a,"globalThis.defined=new WebAssembly.Instance(new WebAssembly.Module(memBytes));"
              "defined.exports.poke(0,81);true","defined memory setup");
    struct realm b=open_realm(NULL);
    REQUIRE(a,"defined.exports.mem.buffer.byteLength===65536",
              "peer install retains defined memory owner link");
    REQUIRE(a,"defined.exports.growit(1)===1&&defined.exports.mem.buffer.byteLength===131072",
              "defined memory link follows interpreter growth after peer install");
    REQUIRE(a,"new Uint8Array(defined.exports.mem.buffer)[0]===81&&defined.exports.peek(0)===81",
              "defined memory and interpreter still agree");
    close_realm(&b);close_realm(&a);return 0;
}
static int reset_peer(int reverse,int shared)
{
    struct realm a=open_realm(NULL),b=open_realm(shared?a.rt:NULL);
    REQUIRE(a,SETUP,"A setup after both installations");
    REQUIRE(b,SETUP,"B setup after both installations");
    if(reverse){
        close_realm(&a);
        REQUIRE(b,SURVIVES,"reset A leaves B resources usable");
        REQUIRE(b,"mem.grow(1)===1&&buf.byteLength===0&&mem.buffer.byteLength===131072",
                  "surviving B memory grows and detaches its own old buffer");
        close_realm(&b);
    }else{
        close_realm(&b);
        REQUIRE(a,SURVIVES,"reset B leaves A resources usable");
        REQUIRE(a,"mem.grow(1)===1&&buf.byteLength===0&&mem.buffer.byteLength===131072",
                  "surviving A memory grows and detaches its own old buffer");
        close_realm(&a);
    }
    return 0;
}
static int same_context_reinstall(void)
{
    struct realm a=open_realm(NULL);
    REQUIRE(a,SETUP,"old installation setup");
    js_wasm_reset(a.ctx);
    REQUIRE(a,"buf.byteLength===0","reset detaches retained external view before native free");
    REQUIRE(a,"(()=>{try{addInst.exports.add(1,2);return false;}catch(e){return true;}})()",
              "retired native method rejects instead of using a new slot");
    REQUIRE(a,"globalThis.oldInst=addInst;globalThis.oldMem=mem;delete globalThis.WebAssembly;true",
              "retain old wrappers across reinstall");
    js_wasm_install(a.ctx);
    REQUIRE(a,SETUP,"same-context new installation creates fresh resources");
    REQUIRE(a,"(()=>{try{oldInst.exports.add(1,2);return false;}catch(e){return true;}})()",
              "old native closure remains retired after reinstall");
    REQUIRE(a,"oldInst=null;oldMem=null;true","release old resource tokens");
    JS_RunGC(a.rt);
    REQUIRE(a,SURVIVES,"old token finalizers cannot free new realm slots");
    close_realm(&a);return 0;
}
static int repeated_realms(void)
{
    struct realm a=open_realm(NULL);
    REQUIRE(a,"globalThis.keep=new WebAssembly.Instance(new WebAssembly.Module(addBytes));true",
              "long-lived addition instance setup");
    for(int i=0;i<12;i++){
        struct realm b=open_realm(NULL);
        REQUIRE(b,SETUP,"temporary realm ordinary resources");
        close_realm(&b);
        REQUIRE(a,"keep.exports.add(7,9)===16","temporary realm reset leaves survivor callable");
        JS_RunGC(a.rt);
    }
    close_realm(&a);return 0;
}
static int imported_exceptions(void)
{
    struct realm a=open_realm(NULL),b=open_realm(NULL);
    const char *src=
        "globalThis.marker={local:true};globalThis.inner=new WebAssembly.Instance("
        "new WebAssembly.Module(importBytes),{env:{twice:function(){throw marker;}}});"
        "globalThis.outer=new WebAssembly.Instance(new WebAssembly.Module(importBytes),"
        "{env:{twice:function(x){try{inner.exports.call4();}catch(e){if(e!==marker)throw e;}return x*3;}}});"
        "outer.exports.call4()===12&&(()=>{try{inner.exports.call4();return false;}catch(e){return e===marker;}})()";
    REQUIRE(a,src,"A nested imported exception keeps identity");
    REQUIRE(b,src,"B nested imported exception keeps identity");
    close_realm(&b);
    REQUIRE(a,"outer.exports.call4()===12","surviving imported callbacks keep their realm");
    close_realm(&a);return 0;
}
int main(void)
{
    const char *names[]={"install-cache","install-defined-memory","reset-peer",
        "reset-reverse","same-context-reinstall","same-runtime-contexts",
        "repeated-realms","imported-exceptions"};
    struct rlimit no_core={0,0};setrlimit(RLIMIT_CORE,&no_core);
    setvbuf(stdout,NULL,_IONBF,0);
    int failed=0;
    for(int k=0;k<8;k++){
        pid_t pid=fork();if(pid<0)return 2;
        if(!pid){
            int rc=k==0?install_cache():k==1?install_defined_memory():
                k==2?reset_peer(0,0):k==3?reset_peer(1,0):
                k==4?same_context_reinstall():k==5?reset_peer(0,1):
                k==6?repeated_realms():imported_exceptions();
            if(!rc){
#ifdef WASM_REALM_TEST_COUNTS
                checks++;
                if(js_wasm_test_owner_count()!=0){
                    puts("FAIL CHECK: all native owners reclaimed after complete teardown");rc=1;
                }
#endif
                if(!rc)printf("PASS CASE: %s (%d checks)\n",names[k],checks);
            }
            _exit(rc);
        }
        int status;if(waitpid(pid,&status,0)!=pid)return 2;
        if(!WIFEXITED(status)||WEXITSTATUS(status)){
            printf("FAIL CASE: %s exit=%d signal=%d\n",names[k],
                WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
            failed++;
        }
    }
    printf("wasm-realms: 8 cases, %d failures\n",failed);
    return failed?1:0;
}
