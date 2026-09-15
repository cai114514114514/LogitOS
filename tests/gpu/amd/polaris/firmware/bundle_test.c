#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd/polaris/firmware/bundle.h"
static unsigned checks,failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint8_t files[8][512];
static struct polaris_fw_sources sources;
static void init(void)
{
    memset(files,0,sizeof files);
    for(unsigned k=0;k<8;++k) {
        uint8_t *p=files[k];put32(p,512);put32(p+4,k<2?52:k==6?104:k==7?36:44);
        p[8]=k==6?2:1;p[10]=k<2?1:0;p[12]=k<2?3:k==7?7:8;p[14]=k<2?1:k==7?2:0;
        put32(p+16,100+k);put32(p+20,64);put32(p+24,256);
        if(k==5){put32(p+36,8);put32(p+40,4);}
        if(k==6) for(unsigned j=0;j<4;++j){put32(p+72+j*8,16);put32(p+76+j*8,320+j*16);}
        if(k==7) put32(p+32,0x20000);
        for(unsigned j=256;j<512;++j)p[j]=(uint8_t)(k*19+j);
        if(k<7)sources.file[k]=(struct polaris_fw_blob){p,512};
    }
}
static void parser(void)
{
    struct polaris_fw_view out,old;
    for(unsigned k=0;k<8;++k) {
        CHECK(!polaris_fw_parse((enum polaris_fw_kind)k,files[k],512,&out));
        CHECK(out.bytes==64 && out.ucode==files[k]+256);
        for(unsigned n=0;n<512;++n) {
            memset(&out,0xa7,sizeof out);old=out;
            CHECK(polaris_fw_parse((enum polaris_fw_kind)k,files[k],n,&out)<0);
            CHECK(!memcmp(&out,&old,sizeof out));
        }
        const unsigned offsets[]={0,4,8,10,12,14,20,24};
        for(unsigned i=0;i<sizeof offsets/sizeof offsets[0];++i) {
            uint8_t bad[512];memcpy(bad,files[k],512);put32(bad+offsets[i],UINT32_MAX);
            CHECK(polaris_fw_parse((enum polaris_fw_kind)k,bad,512,&out)<0);
        }
    }
    uint8_t bad[512];
    memcpy(bad,files[5],512);put32(bad+36,UINT32_MAX);
    CHECK(polaris_fw_parse(POLARIS_FW_MEC,bad,512,&out)<0);
    memcpy(bad,files[5],512);put32(bad+40,UINT32_MAX);
    CHECK(polaris_fw_parse(POLARIS_FW_MEC,bad,512,&out)<0);
    for(unsigned i=0;i<4;++i) {
        memcpy(bad,files[6],512);put32(bad+72+i*8,UINT32_MAX);
        CHECK(polaris_fw_parse(POLARIS_FW_RLC,bad,512,&out)<0);
        memcpy(bad,files[6],512);put32(bad+76+i*8,256);
        CHECK(polaris_fw_parse(POLARIS_FW_RLC,bad,512,&out)<0);
        memcpy(bad,files[6],512);put32(bad+76+i*8,510);
        CHECK(polaris_fw_parse(POLARIS_FW_RLC,bad,512,&out)<0);
    }
    memcpy(bad,files[6],512);put32(bad+84,320);
    CHECK(polaris_fw_parse(POLARIS_FW_RLC,bad,512,&out)<0);
    memcpy(bad,files[7],512);put32(bad+32,0);
    CHECK(polaris_fw_parse(POLARIS_FW_SMC,bad,512,&out)<0);
    CHECK(polaris_fw_parse(POLARIS_FW_SMC,files[7],512,(void *)(files[7]+256))<0);
    CHECK(polaris_fw_parse(POLARIS_FW_SMC,(void *)(UINTPTR_MAX-16),512,&out)<0);
    CHECK(polaris_fw_parse((enum polaris_fw_kind)99,files[7],512,&out)<0);
}
static void staging(void)
{
    struct polaris_fw_bundle_info plan,info,old;
    CHECK(!polaris_fw_bundle_plan(&sources,0x123400000ull,&plan));
    CHECK(plan.bytes_used==40960 && plan.inventory.entry_count==9);
    CHECK(plan.inventory.present_mask==0x5fe && plan.inventory.missing_mask==0);
    uint8_t *dst=malloc(plan.bytes_used+32),*before=malloc(plan.bytes_used+32);
    CHECK(dst && before);if(!dst || !before)exit(2);
    memset(dst,0xcd,plan.bytes_used+32);
    CHECK(!polaris_fw_bundle_stage(dst,plan.bytes_used,0x123400000ull,&sources,&info));
    CHECK(!memcmp(&plan,&info,sizeof info));
    CHECK(get32(dst)==1 && get32(dst+4)==9);
    const unsigned kind[]={6,2,3,4,5,5,5,0,1};
    const unsigned ids[]={10,3,4,5,6,7,8,1,2};
    for(unsigned i=0;i<9;++i) {
        const uint8_t *entry=dst+8+i*28;
        size_t off=(i+1)*4096;
        unsigned n=(i==5 || i==6)?16:64;
        const uint8_t *expected=files[kind[i]]+256+((i==5 || i==6)?32:0);
        CHECK(info.image_offset[i]==off);
        CHECK(get32(entry+4)==1 && get32(entry+8)==0x23400000u+off);
        CHECK(entry[0]==ids[i] && entry[1]==0);
        CHECK(get32(entry+20)==(i==4?32:n));
        CHECK(entry[24]==(i==0 || i==4));
        CHECK(!memcmp(dst+off,expected,n));
        for(unsigned j=n;j<4096;++j) if(dst[off+j]) {CHECK(0);break;}
    }
    CHECK(!memcmp(dst+info.image_offset[5],dst+info.image_offset[6],16));
    CHECK(dst[plan.bytes_used]==0xcd);
    for(unsigned i=0;i<7;++i) {
        init();files[i][8]=0xff;memset(dst,0xa5,plan.bytes_used);memcpy(before,dst,plan.bytes_used);
        memset(&info,0x5a,sizeof info);old=info;
        CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used,0x123400000ull,&sources,&info)<0);
        CHECK(!memcmp(dst,before,plan.bytes_used) && !memcmp(&info,&old,sizeof info));
    }
    init();memcpy(before,dst,plan.bytes_used);
    CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used-1,0x123400000ull,&sources,&info)<0);
    CHECK(!memcmp(dst,before,plan.bytes_used));
    CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used,1,&sources,&info)<0);
    CHECK(polaris_fw_bundle_plan(&sources,POLARIS_SMU_TOC_GPU_LIMIT-4096,&info)<0);
    CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used,0,&sources,(void *)dst)<0);
    CHECK(polaris_fw_bundle_stage(files[0],512,0,&sources,&info)<0);
    CHECK(polaris_fw_bundle_plan(&sources,0,(void *)&sources)<0);
    CHECK(polaris_fw_bundle_stage((void *)(UINTPTR_MAX-16),512,0,&sources,&info)<0);
    CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used,0,NULL,&info)<0);
    put32(files[POLARIS_FW_CE]+16,65536);
    CHECK(polaris_fw_bundle_stage(dst,plan.bytes_used,0,&sources,&info)<0);
    free(dst);free(before);
}
int main(void) {init();parser();staging();printf("BUNDLE: %u checks, %u failures\n",checks,failures);return failures?1:0;}
