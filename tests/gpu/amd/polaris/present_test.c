#include "amd/polaris/present.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define C(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
#include "sdma/model.h"

static void setup(struct fixture *f)
{
    init(f);
    f->memory[2].bytes=32768;
    f->memory[3].bytes=32768;
    f->memory[3].address=0x110000;
    for(unsigned i=0;i<8192;i++)
        f->memory[2].cpu[i]=f->memory[2].gpu[i]=
        f->memory[3].cpu[i]=f->memory[3].gpu[i]=0x1234abcd;
}
static void render(void)
{
    struct fixture f;setup(&f);struct polaris_sdma_queue q={0};
    struct polaris_present p={0};
    struct polaris_sdma_mapping stage=mapping(&f,2),front=mapping(&f,3);
    C(attach(&f,&q)==0);
    C(polaris_present_init(&p,&q,&stage,&front,63,32,272)==0);
    C(p.active && q.completed==2 && q.filled_bytes==4096 && q.copied_bytes==4096);
    for(unsigned i=0;i<8192;i++)C(front.cpu[i]==0x1234abcd);
    uint32_t pixels[80*32];
    for(unsigned i=0;i<80*32;i++)pixels[i]=0xaa000000u+i;
    C(polaris_present_rect(&p,pixels,sizeof pixels,320,3,2,59,29)==0);
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<68;x++)
        C(front.cpu[y*68+x]==(y>=2&&y<31&&x>=3&&x<62?pixels[y*80+x]:0x1234abcd));
    C(p.frames==1 && p.pixels==59*29 && p.uploaded_bytes==59*29*4 && !f.errors);
    unsigned n=calls(&f);
    C(polaris_present_rect(&p,pixels,8,320,3,2,59,29)==-1);
    C(polaris_present_rect(&p,pixels,sizeof pixels,320,62,2,2,1)==-1);
    C(calls(&f)==n);
    p.lock=1;
    C(polaris_present_rect(&p,pixels,sizeof pixels,320,0,0,1,1)==-2);
    C(calls(&f)==n && !p.quarantined && p.active);p.lock=0;
    f.hang=1;f.step=1000;
    C(polaris_present_rect(&p,pixels,sizeof pixels,320,0,0,1,1)==-2);
    C(p.quarantined && !p.active && q.quarantined);
    n=calls(&f);f.hang=0;execute(&f);
    C(polaris_present_rect(&p,pixels,sizeof pixels,320,0,0,1,1)==-2);
    C(polaris_present_init(&p,&q,&stage,&front,63,32,272)==-2);
    C(calls(&f)==n);
}
static void packed(void)
{
    struct fixture f;setup(&f);struct polaris_sdma_queue q={0};
    struct polaris_present p={0};
    struct polaris_sdma_mapping stage=mapping(&f,2),front=mapping(&f,3);
    C(attach(&f,&q)==0);C(polaris_present_init(&p,&q,&stage,&front,64,32,256)==0);
    uint32_t pixels[64*32];for(unsigned i=0;i<64*32;i++)pixels[i]=i*131+7;
    uint64_t n=q.completed;
    C(polaris_present_rect(&p,pixels,sizeof pixels,256,0,0,64,32)==0);
    C(q.completed==n+1 && !f.errors);
    for(unsigned i=0;i<64*32;i++)C(front.cpu[i]==pixels[i]);
}
static void failed_canary(void)
{
    struct fixture f;setup(&f);struct polaris_sdma_queue q={0};
    struct polaris_present p={0};
    struct polaris_sdma_mapping stage=mapping(&f,2),front=mapping(&f,3);
    C(attach(&f,&q)==0);f.hang=1;f.step=1000;
    C(polaris_present_init(&p,&q,&stage,&front,64,32,256)==-2);
    C(!p.active && p.quarantined && !p.frames);
    for(unsigned i=0;i<8192;i++)C(front.cpu[i]==0x1234abcd);
}
int main(void)
{
    render();packed();failed_canary();
    printf("POLARIS_PRESENT: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
