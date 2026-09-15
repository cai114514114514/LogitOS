/* SPDX-License-Identifier: MIT */
#include "amd/polaris/sdma/queue.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define C(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
#include "model.h"
static void good(void)
{
    struct fixture f;init(&f);struct polaris_sdma_queue q={0};
    struct polaris_sdma_mapping src=mapping(&f,2),dst=mapping(&f,3);
    C(attach(&f,&q)==0);C(q.attached&&!f.submissions&&!f.syncs);
    C(polaris_sdma_queue_copy(&q,&src,8,&dst,12,64)==0);
    for(unsigned i=0;i<16;i++)C(dst.cpu[3+i]==src.cpu[2+i]);
    C(dst.cpu[2]==0x555500c2&&dst.cpu[19]==0x555500d3);
    C(q.submitted==1&&q.completed==1&&q.copied_bytes==64&&f.packets==2);
    C(f.wptr==64&&f.rptr==64&&q.fence.cpu[0]==1&&!f.errors);
    f.reenter=&q;
    C(polaris_sdma_queue_fill(&q,&dst,0,0xdecafbad,256)==0);
    for(unsigned i=0;i<64;i++)C(dst.cpu[i]==0xdecafbad);
    C(q.sequence==2&&q.completed==2&&q.filled_bytes==256&&f.wptr==128);
    unsigned n=calls(&f);C(attach(&f,&q)==POLARIS_SDMA_QUEUE_BUSY);C(calls(&f)==n);
    for(unsigned i=0;i<8;i++)C(polaris_sdma_queue_copy(&q,&src,0,&dst,0,256)==0);
    C(q.completed==10&&!f.errors&&f.wptr==128);

    init(&f);q=(struct polaris_sdma_queue){0};f.rptr=f.wptr=240;
    C(attach(&f,&q)==0);C(polaris_sdma_queue_copy(&q,&src,0,&dst,0,256)==0);
    C(f.wptr==64&&f.rptr==64&&q.completed==1&&!f.errors);
    for(unsigned i=0;i<64;i++)C(dst.cpu[i]==src.cpu[i]);
}
static void rejects(void)
{
    for(unsigned i=0;i<15;i++) {
        struct fixture f;init(&f);struct polaris_sdma_queue q={0};
        switch(i){
        case 0:f.id=0x51591002;break;case 1:f.control&=~1u;break;
        case 2:f.control|=0x1000;break;case 3:f.control|=0x200;break;
        case 4:f.control|=0x1000000;break;case 5:f.f32=1;break;
        case 6:f.base++;break;case 7:f.high=1;break;
        case 8:f.rptr=4;break;case 9:f.poll=1;break;
        case 10:f.doorbell=0x10000000;break;case 11:f.unmapped=1;break;
        case 12:f.cntl=0x40000;break;case 13:f.cntl=8;break;case 14:f.cntl=16;break;
        }
        C(attach(&f,&q)==POLARIS_SDMA_QUEUE_UNCONFIGURED);
        C(!q.attached&&!f.submissions&&!f.syncs&&!f.errors);
        if(i==0)C(f.reads==0);
    }
    struct fixture f;init(&f);struct polaris_sdma_queue q={0};
    struct polaris_sdma_mapping dst=mapping(&f,3),src=mapping(&f,2);
    C(polaris_sdma_queue_fill(&q,&dst,0,0,4)==POLARIS_SDMA_QUEUE_UNCONFIGURED);
    C(calls(&f)==0);C(attach(&f,&q)==0);
    unsigned n=calls(&f);
    C(polaris_sdma_queue_copy(&q,&src,0,&src,0,4)==POLARIS_SDMA_QUEUE_INVALID);
    C(polaris_sdma_queue_fill(&q,&q.ring,0,1,4)==POLARIS_SDMA_QUEUE_INVALID);
    C(polaris_sdma_queue_fill(&q,&dst,252,1,8)==POLARIS_SDMA_QUEUE_INVALID);
    C(polaris_sdma_queue_fill(&q,&dst,0,1,0)==POLARIS_SDMA_QUEUE_INVALID);
    C(calls(&f)==n&&!f.submissions);
    f.control=0;
    C(polaris_sdma_queue_fill(&q,&dst,0,1,4)==POLARIS_SDMA_QUEUE_UNCONFIGURED);
    C(!f.submissions&&!f.syncs&&q.quarantined);
    for(unsigned i=0;i<7;i++) {
        init(&f);q=(struct polaris_sdma_queue){0};C(attach(&f,&q)==0);
        if(i==0)f.rptr=4;
        if(i==1)f.unmapped=1;
        if(i==2)f.bad_reg=1;
        if(i==3)f.wptr=f.rptr=64;
        if(i==4)f.cntl=0x40000;
        if(i==5)f.cntl=8;
        if(i==6)f.cntl=16;
        C(polaris_sdma_queue_fill(&q,&dst,0,1,4)==
          (i==2?POLARIS_SDMA_QUEUE_IO:POLARIS_SDMA_QUEUE_UNCONFIGURED));
        C(q.quarantined&&!f.submissions&&!q.completed);
        n=calls(&f);
        /* Restoring the old values is not a hardware reset or DMA drain. */
        f.rptr=f.wptr=f.cntl=0;f.unmapped=f.bad_reg=0;
        C(polaris_sdma_queue_fill(&q,&dst,0,1,4)==POLARIS_SDMA_QUEUE_QUARANTINED);
        C(calls(&f)==n);
    }
}
static void failures_test(void)
{
    struct fixture f;struct polaris_sdma_queue q;struct polaris_sdma_mapping dst;
    for(unsigned i=0;i<5;i++) {
        init(&f);q=(struct polaris_sdma_queue){0};dst=mapping(&f,3);
        C(attach(&f,&q)==0);
        if(i==0){f.hang=1;f.step=1000;}
        if(i==1){f.hang=1;f.step=0;}
        if(i==2)f.corrupt=1;
        if(i==3)f.fail_write=1;
        if(i==4){f.clock=UINT64_MAX;f.step=1;}
        int rc=polaris_sdma_queue_fill(&q,&dst,0,0x33333333,256);
        C(rc==(i==2?POLARIS_SDMA_QUEUE_READBACK:i==3?POLARIS_SDMA_QUEUE_IO:POLARIS_SDMA_QUEUE_TIMEOUT));
        C(q.quarantined&&q.submitted==1&&q.completed==0);
        unsigned n=calls(&f);
        C(attach(&f,&q)==POLARIS_SDMA_QUEUE_QUARANTINED);
        C(calls(&f)==n);
    }
    /* The late-fence mutant specifically mistakes completion after timeout
     * for recovery. The original destination must stay owned and quarantined. */
    init(&f);q=(struct polaris_sdma_queue){0};dst=mapping(&f,3);
    C(attach(&f,&q)==0);f.hang=1;f.step=1000;
    C(polaris_sdma_queue_fill(&q,&dst,0,0x11223344,256)==POLARIS_SDMA_QUEUE_TIMEOUT);
    execute(&f);f.memory[1].cpu[0]=f.memory[1].gpu[0];
    unsigned n=calls(&f);
    C(polaris_sdma_queue_fill(&q,&dst,0,9,256)==POLARIS_SDMA_QUEUE_QUARANTINED);
    C(calls(&f)==n&&q.completed==0&&q.quarantined);
    init(&f);q=(struct polaris_sdma_queue){0};dst=mapping(&f,3);C(attach(&f,&q)==0);
    f.fail_sync=1;
    C(polaris_sdma_queue_fill(&q,&dst,0,1,4)==POLARIS_SDMA_QUEUE_IO);
    C(!f.submissions&&q.quarantined&&!q.completed);
}
int main(void)
{
    good();rejects();failures_test();
    printf("POLARIS_SDMA_QUEUE: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
