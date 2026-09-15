/* SPDX-License-Identifier: MIT */
#define main blk_sequential_main
#include "blkreq_test.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
struct parallel_medium { atomic_int callbacks, overlap; };
static int parallel_read(void *ctx,uint64_t lba,uint32_t n,void *buf)
{
    struct parallel_medium *m=ctx;
    if(atomic_fetch_add(&m->callbacks,1)) {
        atomic_fetch_add(&m->overlap,1);
        /* Fail at the ownership violation itself. Letting the faulty engine
         * continue would race stack request reuse before the final assertion. */
        puts("FAIL: one medium callback at a time on four CPUs");
        fflush(stdout);
        _Exit(1);
    }
    for(int i=0;i<4;i++)sched_yield();
    memset(buf,(int)(lba&255),(size_t)n*BLK_SECTOR);
    atomic_fetch_sub(&m->callbacks,1);
    return 0;
}
static const struct blk_ops parallel_ops={.read=parallel_read};
struct parallel_arg { struct blkdev *dev; unsigned id; atomic_int *errors; };
static void *parallel_worker(void *arg)
{
    struct parallel_arg *a=arg;uint8_t data[BLK_SECTOR];
    for(unsigned i=0;i<1000;i++) {
        unsigned sector=a->id*1000+i;
        struct blk_req req;blk_req_init(&req,a->dev,BLK_OP_READ,sector,1,data);
        if(blk_wait(&req)||!sector_is(data,sector))atomic_fetch_add(a->errors,1);
    }
    return 0;
}
int main(void)
{
    if(blk_sequential_main())return 1;
    struct parallel_medium m={0};atomic_int errors=0;
    struct blkdev *d=blk_register("parallel",&parallel_ops,&m,100000);
    ck(d!=0,"parallel medium allocated");
    pthread_t t[4];struct parallel_arg args[4];
    for(unsigned i=0;i<4;i++){args[i]=(struct parallel_arg){d,i,&errors};pthread_create(&t[i],0,parallel_worker,&args[i]);}
    for(int i=0;i<4;i++)pthread_join(t[i],0);
    ckeq(atomic_load(&m.overlap),0,"one medium callback at a time on four CPUs");
    ckeq(atomic_load(&errors),0,"4000 completed requests retain their data");
    blk_dev_offline(d);
    uint8_t data[BLK_SECTOR];struct blk_req req;
    blk_req_init(&req,d,BLK_OP_READ,1,1,data);
    ck(blk_wait(&req)<0,"offline medium refuses new submissions");
    ckeq(atomic_load(&m.callbacks),0,"offline has drained backend callbacks");
    printf("BKL block: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
