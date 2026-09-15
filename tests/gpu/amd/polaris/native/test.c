#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd/polaris/native.h"
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
static uint32_t config[64],mmio[65536];
static uint8_t aperture[16384];
static unsigned pci_reads;static uint64_t ticks;
static int read_pci(void *opaque,uint16_t at,uint32_t *out)
{(void)opaque;++pci_reads;if((at&3)||at>252)return -1;*out=config[at/4];return 0;}
static uint64_t time_us(void *opaque){(void)opaque;return ++ticks;}
static struct polaris_native_resources resource(void)
{
    memset(config,0,sizeof config);memset(mmio,0,sizeof mmio);memset(aperture,0,sizeof aperture);
    config[0]=0x67df1002;config[1]=6|(1u<<20);config[2]=0x03000000;
    config[4]=0xc000000c;config[5]=1;config[9]=0xf0000000;config[0x34/4]=0x40;
    config[0x40/4]=1;config[0x44/4]=0;
    /* 4-GiB VRAM at MC 0x100000000; CPU aperture is independently at
     * PCI physical 0x1c0000000. Using either as the other must fail. */
    mmio[0x2024/4]=0x01ff0100;mmio[0x2c04/4]=0x01000000;
    mmio[0x2034/4]=0x100000;mmio[0x2038/4]=0x1fffff;mmio[0x2064/4]=0x5b;
    mmio[0x2c08/4]=0x40000100;mmio[0x5428/4]=4096;mmio[0x5490/4]=3;
    struct polaris_native_resources r={.read_pci32=read_pci,.now_us=time_us,
        .bar0=aperture,.bar5=(void *)mmio,.bar0_physical=0x1c0000000ull,.bar0_bytes=sizeof aperture,
        .bar5_physical=0xf0000000,.bar5_bytes=sizeof mmio,
        .arena={0x100002000ull,8192},.scanout={0x100000000ull,4096}};
    return r;
}
static void success(void)
{
    struct polaris_native n={0};struct polaris_platform p;
    struct polaris_native_resources r=resource();
    CHECK(!polaris_native_bind(&n,&r,&p));CHECK(n.bound && !n.faulted);
    uint32_t id;CHECK(!p.read_identity(p.opaque,&id) && id==0x67df1002);
    volatile uint8_t *cpu=NULL;
    CHECK(!p.resolve_mapping(p.opaque,0x100002040ull,64,&cpu) && cpu==aperture+0x2040);
    CHECK(p.resolve_mapping(p.opaque,0x1c0002040ull,64,&cpu)<0);
    CHECK(p.resolve_mapping(p.opaque,0x100001000ull,4,&cpu)<0); /* unowned gap */
    CHECK(p.resolve_mapping(p.opaque,0x100003ffcull,8,&cpu)<0);
    CHECK(p.resolve_mapping(p.opaque,UINT64_MAX,8,&cpu)<0);
    CHECK(p.resolve_mapping(p.opaque,0x100002000ull,0,&cpu)<0);
    CHECK(!p.write_reg(p.opaque,0x1234,0x11223344));CHECK(mmio[0x1234/4]==0x11223344);
    CHECK(!p.read_reg(p.opaque,0x1234,&id) && id==0x11223344);
    CHECK(p.write_reg(p.opaque,3,7)<0);CHECK(p.read_reg(p.opaque,sizeof mmio,&id)<0);
    CHECK(!p.sync(p.opaque,POLARIS_SDMA_TO_DEVICE,0x100002000ull,4096));
    CHECK(mmio[0x5480/4]==1 && mmio[0x2f30/4]==0);
    CHECK(!p.sync(p.opaque,POLARIS_SDMA_TO_CPU,0x100002000ull,4096));
    CHECK(mmio[0x2f30/4]==1); /* exact HDP_DEBUG0, not adjacent DEBUG1 */
    CHECK(mmio[0x2f34/4]==0);
    CHECK(p.sync(p.opaque,(enum polaris_sdma_sync)99,0x100002000ull,4)<0);
    CHECK(p.now_us(p.opaque)<p.now_us(p.opaque));
    CHECK(polaris_native_bind(&n,&r,&p)<0);
    mmio[0x2c04/4]++;
    CHECK(p.resolve_mapping(p.opaque,0x100002000ull,4,&cpu)<0 && n.faulted);
    mmio[0x2c04/4]--;
    CHECK(p.write_reg(p.opaque,0x1234,9)<0 && mmio[0x1234/4]==0x11223344);
}
static void rejection(void)
{
    const unsigned reg[]={0x2024,0x2c04,0x2c08,0x2f4c,0x5428,0x5490,0x20ac};
    const uint32_t bad[]={0,0,0x40000102,2,2048,1,4};
    for(unsigned i=0;i<sizeof reg/sizeof reg[0];++i) {
        struct polaris_native n={0},saved=n;struct polaris_platform p,old;
        struct polaris_native_resources r=resource();mmio[reg[i]/4]=bad[i];
        memset(&p,0xa5,sizeof p);old=p;
        CHECK(polaris_native_bind(&n,&r,&p)<0);
        CHECK(!memcmp(&n,&saved,sizeof n) && !memcmp(&p,&old,sizeof p));
    }
    const unsigned offset[]={0,4,8,12,0x10,0x14,0x24,0x34,0x40,0x44};
    const uint32_t value[]={0x51591002,2,0x02000000,0x10000,0xc0000001,2,0xe0000000,0x41,0x4001,3};
    for(unsigned i=0;i<sizeof offset/sizeof offset[0];++i) {
        struct polaris_native n={0};struct polaris_platform p;struct polaris_native_resources r=resource();
        config[offset[i]/4]=value[i];unsigned before=pci_reads;
        CHECK(polaris_native_bind(&n,&r,&p)<0);CHECK(pci_reads-before<=60);
    }
    struct polaris_native n={0};struct polaris_platform p;struct polaris_native_resources r=resource();
    r.arena.base=r.scanout.base;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.arena.base+=8192;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar5_bytes=0x1000;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar0_physical=0x100000000ull;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar0=(void *)((uintptr_t)aperture+1);CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar0=(void *)(UINTPTR_MAX-3);CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar0_physical=UINT64_MAX-15;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar5_physical=UINT64_C(1)<<32;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.bar0_physical=r.bar5_physical;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();r.now_us=NULL;CHECK(polaris_native_bind(&n,&r,&p)<0);
    r=resource();CHECK(polaris_native_bind(&n,&r,(void *)&n)<0);
    CHECK(!polaris_native_bind(&n,&r,&p));
    config[0]=0xffffffff;
    CHECK(p.write_reg(p.opaque,0x1234,9)<0 && n.faulted);
    config[0]=0x67df1002;CHECK(p.write_reg(p.opaque,0x1234,9)<0 && mmio[0x1234/4]==0);
}
static void system_aperture(void)
{
    /* Literal oracles deliberately do not share production register macros.
     * The shortened high window still covers arena+scanout but not all VRAM;
     * the widest 28-bit high page checks the inclusive-end conversion. */
    const unsigned offsets[]={0x2034,0x2038,0x2038,0x2034,0x2038,0x2064,0x2064,0x2064,0x2064};
    const uint32_t values[]={0x100001,0x1ffffe,0x0fffff,0x10100000,0x101fffff,0x40,0x18,0x78,0x50};
    for(unsigned i=0;i<sizeof offsets/sizeof offsets[0];++i){
        struct polaris_native n={0},old=n;struct polaris_platform p,saved;
        struct polaris_native_resources r=resource();memset(&p,0xa5,sizeof p);saved=p;
        mmio[offsets[i]/4]=values[i];
        CHECK(polaris_native_bind(&n,&r,&p)<0);
        CHECK(!memcmp(&n,&old,sizeof n) && !memcmp(&p,&saved,sizeof p));
        CHECK(mmio[offsets[i]/4]==values[i]); /* preflight never rewrites GMC */
    }
    struct polaris_native n={0};struct polaris_platform p;
    struct polaris_native_resources r=resource();
    mmio[0x2034/4]=0;mmio[0x2038/4]=0x0fffffff;
    CHECK(!polaris_native_bind(&n,&r,&p));
    CHECK(n.system_low==0 && n.system_high==0x0fffffff && n.l1_mode==0x58);
    volatile uint8_t *cpu=NULL;
    CHECK(!p.resolve_mapping(p.opaque,0x100002000ull,8192,&cpu) && cpu==aperture+8192);
    const unsigned drift_offsets[]={0x2034,0x2038,0x2064};
    const uint32_t drift_values[]={0x0fffff,0x200000,0x78};
    for(unsigned i=0;i<3;++i){
        n=(struct polaris_native){0};r=resource();
        CHECK(!polaris_native_bind(&n,&r,&p));
        unsigned at=drift_offsets[i]/4;uint32_t original=mmio[at];
        mmio[at]=drift_values[i];cpu=aperture+1;
        /* Widening a still-covering window is drift too: ownership must not
         * silently survive a new driver's mapping reconfiguration. */
        CHECK(p.resolve_mapping(p.opaque,0x100002000ull,4,&cpu)<0 && n.faulted);
        CHECK(cpu==aperture+1 && mmio[at]==drift_values[i]);
        mmio[at]=original;
        CHECK(p.resolve_mapping(p.opaque,0x100002000ull,4,&cpu)<0);
        CHECK(p.write_reg(p.opaque,0x1234,9)<0 && mmio[0x1234/4]==0);
    }
}
int main(void){success();rejection();system_aperture();printf("NATIVE: %u checks, %u failures\n",checks,failures);return failures?1:0;}
