/* SPDX-License-Identifier: MIT */
/* Production clipboard/EVQ/learning code with real host threads. The copyout
 * callback deterministically replaces a selection while a reader owns it.
 * Mutants run this identical fixture, never different expected answers. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "logit_abi.h"
#include "clipboard.h"
#include "evq.h"
#include "ime_learn.h"

static int fails, checks, replace_on_copy;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n",msg); } } while (0)
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
int user_range_ok(const void *p, uint64_t n, int w) { (void)n;(void)w;return p!=NULL; }
int user_copy_from(void *d,const void *s,uint64_t n) { memcpy(d,s,n);return 0; }
int user_copy_to(void *d,const void *s,uint64_t n) {
    if (replace_on_copy) {
        replace_on_copy=0;
        CHECK(clip_set_text("replacement",11)==11,"clipboard replacement succeeds");
    }
    memcpy(d,s,n); return 0;
}
int notify_post(const char *a,const char *b,int c) {(void)a;(void)b;(void)c;return 1;}

static unsigned ev_barrier, learn_barrier;
/* Invoked only by mutants at the exact race window, after a shared read and
 * before its publication. Both writers must observe the same old value. */
void bkl_evq_pause(void) {
    __atomic_add_fetch(&ev_barrier,1,__ATOMIC_SEQ_CST);
    while (__atomic_load_n(&ev_barrier,__ATOMIC_SEQ_CST)<2) sched_yield();
}
void bkl_learn_pause(void) {
    __atomic_add_fetch(&learn_barrier,1,__ATOMIC_SEQ_CST);
    while (__atomic_load_n(&learn_barrier,__ATOMIC_SEQ_CST)<2) sched_yield();
}
static struct evq queue;
static void *event_writer(void *arg) {
    struct logit_event e={EV_KEY,(int)(intptr_t)arg,1234,0,0,0};
    evq_push(&queue,&e);return NULL;
}
static void *learn_writer(void *arg) {
    (void)arg;
    ime_learn_note("nihao",5,(const uint8_t *)"\xe4\xbd\xa0\xe5\xa5\xbd",6);
    return NULL;
}
static void pair(void *(*fn)(void *)) {
    pthread_t a,b;
    CHECK(!pthread_create(&a,0,fn,(void *)(intptr_t)1),"thread A created");
    CHECK(!pthread_create(&b,0,fn,(void *)(intptr_t)2),"thread B created");
    pthread_join(a,0);pthread_join(b,0);
}
int main(void) {
    setbuf(stdout,NULL);
    puts("GUI test: clipboard replacement during copyout");
    CHECK(clip_set_text("old selection",13)==13,"clipboard initial payload");
    char out[64]={0}; replace_on_copy=1;
    long n=clip_syscall(SYS_CLIP_GET,CLIP_F_TEXT,(long)out,sizeof out,42);
    CHECK(n==13&&!memcmp(out,"old selection",13),"clipboard reader retains original bytes");
    CHECK(clip_len(CLIP_F_TEXT)==11,"clipboard publishes new selection");

    puts("GUI test: simultaneous event publication");
    pair(event_writer);
    unsigned mask=0;int count=0;struct logit_event e;
    while(evq_pop(&queue,&e)) { count++; if(e.a>=1&&e.a<=2)mask|=1u<<e.a; }
    CHECK(count==2&&mask==6,"event producers retain both unique events");
    CHECK(evq_queued()==2&&evq_dropped()==0,"event counters match actual delivery");

    puts("GUI test: simultaneous learning commits");
    ime_learn_init(0x1234);pair(learn_writer);
    uint32_t entries=0,commits=0;int dirty=0;
    ime_learn_stats(&entries,&commits,&dirty);
    CHECK(entries==1&&commits==2&&dirty,"learning preserves both commits");
    char snapshot[4096];
    int bytes=ime_learn_serialise(snapshot,sizeof snapshot);
    CHECK(bytes>0&&strstr(snapshot,"nihao \xe4\xbd\xa0\xe5\xa5\xbd 2\n"),"learning snapshot carries complete count");
    printf("BKL_GUI checks=%d failures=%d\n",checks,fails);
    return fails?1:0;
}
