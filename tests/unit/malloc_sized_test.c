/* SPDX-License-Identifier: MIT
 * Lock count, allocation semantics, and the actual QuickJS consumer. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "quickjs.h"
#include "../../c/apps/libc/include/logit_malloc.h"
void *lmalloc(size_t);void lfree(void *);size_t lmalloc_usable_size(void *);
size_t sized_test_live_bytes(void);
void sized_test_flush(void);
#define malloc_cur sized_test_live_bytes()
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned long locks,unlocks;
static int failures;
void __libc_lock(volatile int *unused){(void)unused;pthread_mutex_lock(&lock);locks++;}
void __libc_unlock(volatile int *unused){(void)unused;unlocks++;pthread_mutex_unlock(&lock);}
static void ck(int ok,const char *name){printf("%s: %s\n",ok?"ok":"FAIL",name);if(!ok)failures++;}
static void *churn(void *arg)
{
    uintptr_t marker=(uintptr_t)arg;int bad=0;
    for(int i=0;i<4000;i++){
        size_t cap=0,n=17+(i%500);unsigned char *p=__libc_malloc_size(n,&cap);
        if(!p||cap<n){bad=1;break;}memset(p,(int)marker,n);
        size_t next=0;unsigned char *q=__libc_realloc_size(p,n+800,&next);
        if(!q){__libc_free_size(p);bad=1;break;}
        for(size_t j=0;j<n;j++)if(q[j]!=marker)bad=1;
        if(next<n+800||__libc_free_size(q)!=next)bad=1;
    }
    return (void *)(uintptr_t)bad;
}
int main(void)
{
    size_t cap=0;unsigned long before=locks;
    void *p=__libc_malloc_size(33,&cap);
    ck(p&&cap>=33&&locks-before==1,"allocation and capacity take one lock transaction");
    ck(lmalloc_usable_size(p)==cap,"capacity agrees with ordinary usable-size query");
    void *neighbor=lmalloc(200);lfree(neighbor);before=locks;
    ck(__libc_free_size(p)==cap&&locks-before==1,"free reports pre-coalescing capacity under one lock");
    ck(__libc_free_size(p)==0&&__libc_free_size(NULL)==0,"double and null free stay harmless");
    cap=99;ck(!__libc_malloc_size(SIZE_MAX,&cap)&&cap==0,"overflow refusal clears reported capacity");
    p=__libc_malloc_size(48,&cap);memset(p,23,48);size_t rejected=99;
    ck(!__libc_realloc_size(p,SIZE_MAX,&rejected)&&rejected==0&&((unsigned char *)p)[47]==23&&lmalloc_usable_size(p)==cap,"failed realloc preserves original allocation and reports zero");
    __libc_free_size(p);
    p=__libc_realloc_size(NULL,33,&cap);memset(p,31,33);
    neighbor=lmalloc(200);memset(neighbor,42,200);before=locks;
    void *grown=__libc_realloc_size(p,4096,&cap);
    ck(grown&&grown!=p&&cap>=4096&&locks-before==1&&((unsigned char *)grown)[32]==31&&((unsigned char *)neighbor)[199]==42,"moving realloc preserves both payloads in one transaction");
    p=__libc_realloc_size(grown,17,&cap);
    ck(p==grown&&cap>=17&&cap<4096&&((unsigned char *)p)[16]==31,"shrinking realloc reports actual capacity and preserves retained bytes");
    cap=99;ck(!__libc_realloc_size(p,0,&cap)&&cap==0,"zero-sized realloc frees and reports zero capacity");
    lfree(neighbor);p=__libc_malloc_size(17,NULL);
    ck(p&&__libc_free_size(p)>=17,"capacity output is optional without changing allocation semantics");
    pthread_t threads[4];int made=0,bad=0;
    for(int i=0;i<4;i++){if(pthread_create(&threads[i],NULL,churn,(void *)(uintptr_t)(i+1))){bad=1;break;}made++;}
    for(int i=0;i<made;i++){void *result;pthread_join(threads[i],&result);if(result)bad=1;}
    ck(!bad&&malloc_cur==0&&locks==unlocks,"four concurrent allocator consumers preserve bytes and release all memory");
    /* Cache contents after threaded churn depend on interleaving, and can
     * legitimately change later usable capacities. Start both accounting
     * comparisons from the same physically coalesced empty arena. */
    sized_test_flush();locks=unlocks=0;
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=rt?JS_NewContext(rt):NULL;
    ck(ctx!=NULL,"QuickJS initializes with the actual mini-libc allocator");
    if(!ctx)return 1;
    const char *src="var sum=0;for(var i=0;i<12000;i++){var a=[];for(var j=0;j<12;j++)a.push({x:i+j,y:'ab'+j});sum+=a[7].x;}sum";
    JSValue v=JS_Eval(ctx,src,strlen(src),"<allocation consumer>",JS_EVAL_TYPE_GLOBAL);int32_t sum=0;
    ck(!JS_IsException(v)&&JS_ToInt32(ctx,&sum,v)==0&&sum==72078000,"allocation-heavy JS preserves computed output");JS_FreeValue(ctx,v);
    JS_RunGC(rt);JSMemoryUsage usage;JS_ComputeMemoryUsage(rt,&usage);
    printf("QJS_ACCOUNT count=%lld size=%lld result=%d\n",(long long)usage.malloc_count,(long long)usage.malloc_size,sum);
    size_t live=malloc_cur;JS_SetMemoryLimit(rt,(size_t)usage.malloc_size+128);
    void *denied=js_malloc_rt(rt,1024*1024);
    ck(!denied&&malloc_cur==live,"runtime memory ceiling still refuses allocation without heap growth");
    if(denied)js_free_rt(rt,denied);
    JS_SetMemoryLimit(rt,SIZE_MAX);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    ck(malloc_cur==0&&locks==unlocks,"QuickJS teardown leaves exact zero live allocator bytes");
    printf("QJS_LOCKS %lu\n",locks);
    return failures?1:0;
}
