/* SPDX-License-Identifier: MIT
 * The cache must stay bounded, be logically free, and never hide reclaimable
 * memory from a large allocation or bypass a lowered arena commit ceiling. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t heap_mutex=PTHREAD_MUTEX_INITIALIZER;
void __libc_lock(volatile int *unused){(void)unused;pthread_mutex_lock(&heap_mutex);}
void __libc_unlock(volatile int *unused){(void)unused;pthread_mutex_unlock(&heap_mutex);}
void *lmalloc(size_t);void lfree(void *);size_t lmalloc_usable_size(void *);
size_t malloc_arena_size(void);
size_t sized_test_live_bytes(void),sized_test_limit(size_t);
void sized_test_flush(void);unsigned sized_test_cached(void);
static int checks,failures;
static void ck(int ok,const char *name)
{checks++;printf("%s: %s\n",ok?"ok":"FAIL",name);failures+=!ok;}
int main(void)
{
    void *blocks[32][12];int intact=1;
    for(int c=0;c<32;c++)for(int i=0;i<12;i++){
        size_t n=(c+1)*16;blocks[c][i]=lmalloc(n);
        if(!blocks[c][i])return 2;memset(blocks[c][i],c+1,n);
    }
    for(int c=0;c<32;c++)for(int i=0;i<12;i++){
        unsigned char *p=blocks[c][i];
        for(int j=0;j<(c+1)*16;j++)if(p[j]!=c+1)intact=0;
        lfree(p);
    }
    ck(intact&&sized_test_live_bytes()==0,"all size classes preserve payload bytes and logical accounting");
    ck(sized_test_cached()==256,"cache retains at most eight blocks per exact class");
    ck(lmalloc_usable_size(blocks[0][0])==0,"cached block is not a live allocation");
    unsigned count=sized_test_cached();lfree(blocks[0][0]);
    ck(sized_test_cached()==count,"double free cannot enqueue a cached block twice");
    int reused=1;for(int c=0;c<32;c++){
        void *p=lmalloc((c+1)*16);if(p!=blocks[c][7])reused=0;blocks[c][0]=p;
    }
    ck(reused&&sized_test_cached()==224,"exact classes reuse checked cached heads");
    for(int c=0;c<32;c++)lfree(blocks[c][0]);
    size_t whole=malloc_arena_size()-32;void *p=lmalloc(whole);
    ck(p&&lmalloc_usable_size(p)>=whole&&sized_test_cached()==0,"large allocation drains and coalesces cached fragments");
    lfree(p);sized_test_flush();
    void *guard=lmalloc(4096);p=lmalloc(64);memset(guard,19,4096);lfree(p);
    size_t old=sized_test_limit(16);void *denied=lmalloc(64);sized_test_limit(old);
    ck(!denied,"cached reuse obeys the current commit ceiling");
    if(denied)lfree(denied);
    ck(((unsigned char *)guard)[4095]==19,"ceiling refusal does not clobber live neighbor");
    lfree(guard);sized_test_flush();
    /* The ordinary free tail starts above this ceiling, but the cached
     * contiguous prefix fits after coalescing. Refusal must reclaim first,
     * not mistake an unsuitable tail for exhaustion of committed space. */
    void *prefix[8];
    for(int i=0;i<8;i++){prefix[i]=lmalloc(64);if(!prefix[i])return 2;}
    for(int i=0;i<8;i++)lfree(prefix[i]);
    old=sized_test_limit(512);p=lmalloc(256);sized_test_limit(old);
    ck(p&&lmalloc_usable_size(p)>=256,"commit-ceiling refusal reclaims cached prefix before failing");
    lfree(p);sized_test_flush();
    ck(sized_test_live_bytes()==0&&sized_test_cached()==0,"explicit drain leaves no cached or live blocks");
    printf("malloc-small: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
