#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "elf.h"
#include "exechost/space.h"
static int checks,failed;
#define CHECK(c,n) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
static uint16_t u16(const void *p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t u32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static void put64(void*p,uint64_t v){memcpy(p,&v,8);}
static uint64_t sym(const unsigned char *b,const char *name)
{
    const unsigned char *sh=b+u64(b+40);int n=u16(b+60);
    for(int i=0;i<n;i++){const unsigned char *s=sh+i*64;if(u32(s+4)!=2)continue;
        const char *str=(const char*)b+u64(sh+u32(s+40)*64+24);
        const unsigned char *tab=b+u64(s+24);uint64_t len=u64(s+32);
        for(uint64_t j=0;j<len;j+=24)if(!strcmp(str+u32(tab+j),name))return u64(tab+j+8);
    }fprintf(stderr,"missing symbol %s\n",name);exit(2);
}
static struct elf64_phdr *ph(unsigned char *b,int type)
{struct elf64_phdr *p=(void*)(b+u64(b+32));for(int i=0;i<u16(b+56);i++)if(p[i].p_type==(unsigned)type)return p+i;return 0;}
static uint64_t rela_off(unsigned char*b)
{
    struct elf64_phdr*d=ph(b,2),*p=(void*)(b+u64(b+32));uint64_t va=0;
    for(uint64_t i=0;i<d->p_filesz;i+=16)if(u64(b+d->p_offset+i)==7)va=u64(b+d->p_offset+i+8);
    for(int i=0;i<u16(b+56);i++)if(p[i].p_type==1&&va>=p[i].p_vaddr&&va-p[i].p_vaddr<p[i].p_filesz)return p[i].p_offset+va-p[i].p_vaddr;
    abort();
}
static void refused(unsigned char*b,size_t n,const char*name)
{struct elf_image im;space_reset();CHECK(elf_load_image(b,n,&im)!=0,name);CHECK(space_frames_used()==0,"refusal before allocating frames");}
int main(int argc,char**argv)
{
    if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);
    unsigned char*b=malloc(n),*m=malloc(n);if(fread(b,1,n,f)!=n)return 2;fclose(f);
    space_set_nx(1);space_quiet(1);uint64_t oldbase=0;
    for(int pass=0;pass<2;pass++){
        struct elf_image im;space_reset();int rc=elf_load_image(b,n,&im);CHECK(rc==0,"static PIE loads");if(rc){fprintf(stderr,"loader: %s\n",space_last_msg());return 1;}
        CHECK(im.load_bias!=oldbase,"two different load biases");oldbase=im.load_bias;
        CHECK(im.load_bias>=MM_USER_WIDE_BASE,"PIE uses wide window");
        CHECK(im.relocations>=4,"global/function/TLS pointers relocated");
        uint64_t data=im.load_bias+sym(b,"pie_data"),pointer=im.load_bias+sym(b,"pie_pointer"),function=im.load_bias+sym(b,"pie_function"),fn=im.load_bias+sym(b,"pie_fn");
        CHECK(*(long*)data==42,"global data preserved");CHECK(*(uint64_t*)pointer==data,"global pointer resolves");
        CHECK(*(uint64_t*)function==fn,"function pointer resolves");
        CHECK(((long(*)(void))fn)()==27,"mapped x86 code executes");
        CHECK(*(long*)(im.tls_va+sym(b,"pie_tls"))==11,"TLS integer initialiser");
        CHECK(*(uint64_t*)(im.tls_va+sym(b,"pie_tls_pointer"))==data,"TLS copies relocated pointer");
        CHECK(!space_writable(im.load_bias+sym(b,"pie_relro")),"RELRO rejects real store");
        CHECK(space_writable(pointer),"ordinary data remains writable");CHECK(!space_writable(fn),"text rejects real store");CHECK(space_nx(pointer),"data NX");
        CHECK(im.entry==im.load_bias+u64(b+24),"entry biased once");
        struct elf64_phdr *p=(void*)im.phdr_va;CHECK(p[0].p_vaddr==ph(b,6)->p_vaddr,"auxv points at unmodified headers");
        printf("PIE_HOST_BIAS 0x%llx\n",(unsigned long long)im.load_bias);
    }
    uint64_t ro=rela_off(b);
    memcpy(m,b,n);put64(m+ro+8,1);refused(m,n,"symbol relocation refused");
    memcpy(m,b,n);put64(m+ro+8,((uint64_t)1<<32)|8);refused(m,n,"relative with symbol refused");
    memcpy(m,b,n);put64(m+ro,sym(b,"pie_fn"));refused(m,n,"text relocation refused");
    memcpy(m,b,n);put64(m+ro,UINT64_MAX-2);refused(m,n,"target overflow refused");
    memcpy(m,b,n);put64(m+ro+16,UINT64_C(0x7fffffffffffffff));refused(m,n,"pointer out of range refused");
    memcpy(m,b,n);struct elf64_phdr*d=ph(m,2);put64(m+d->p_offset,1);refused(m,n,"DT_NEEDED refused");
    memcpy(m,b,n);d=ph(m,2);put64(m+d->p_offset,36);refused(m,n,"DT_RELR refused");
    memcpy(m,b,n);d=ph(m,2);for(uint64_t i=0;i<d->p_filesz;i+=16)put64(m+d->p_offset+i,4);refused(m,n,"missing DT_NULL refused");
    memcpy(m,b,n);d=ph(m,2);d->p_type=3;refused(m,n,"PT_INTERP refused");
    memcpy(m,b,n);d=ph(m,7);d->p_vaddr=0x100000;refused(m,n,"unmapped TLS initialiser refused");
    memcpy(m,b,n);put64(m+ro+8,0);struct elf_image im;space_reset();CHECK(elf_load_image(m,n,&im)==0,"R_NONE accepted");
    space_reset();free(m);free(b);printf("PIE_HOST %d checks %d failed\n",checks,failed);return failed?1:0;
}
