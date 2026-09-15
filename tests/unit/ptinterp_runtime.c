/* SPDX-License-Identifier: MIT
 * Acceptance interpreter, not the future system dynamic linker. It handles
 * only RELATIVE and one static TLS block, and refuses all symbol imports. */
#include <stdint.h>
#include "logit.h"
#include "elf.h"
extern uint64_t _DYNAMIC[];
static long self_value = 73;
static const unsigned char file_bytes[12288]={[0]=0x65,[4096]=0x73,[12287]=0x9a};
long *volatile interp_pointer = &self_value;
static unsigned char tls_buffer[8192] __attribute__((aligned(4096)));
static void say(const char *s) { long n=0;while(s[n])n++;_sys(SYS_WRITE,1,(long)s,n); }
static void fail(const char *s) { say("PTINTERP_RUNTIME_FAIL ");say(s);say("\n");_sys(SYS_EXIT,96,0,0);for(;;){} }
static void relocate(uint64_t base, uint64_t *d)
{
    uint64_t rel=0, size=0, ent=0;
    for(int i=0; d[0] && i<512; i++,d+=2) {
        if(d[0]==1) fail("shared library fixture unsupported");
        if(d[0]==7)rel=base+d[1];if(d[0]==8)size=d[1];if(d[0]==9)ent=d[1];
    }
    if(size && ent!=24)fail("rela size");
    for(uint64_t i=0;i<size;i+=24) {
        uint64_t *r=(void*)(rel+i);
        if(r[1]!=8)fail("nonrelative");
        *(uint64_t*)(base+r[0])=base+r[2];
    }
}
static void seal(uint64_t base, struct elf64_phdr *ph, uint64_t n)
{
    for(uint64_t i=0;i<n;i++)if(ph[i].p_type==0x6474e552) {
        uint64_t lo=(base+ph[i].p_vaddr)&~4095ull;
        uint64_t hi=(base+ph[i].p_vaddr+ph[i].p_memsz)&~4095ull;
        if(hi>lo && _sys(SYS_MPROTECT,lo,hi-lo,1)<0)fail("relro protect");
    }
}
uint64_t ptinterp_boot(uint64_t *sp)
{
    uint64_t *env=sp+sp[0]+2;while(*env)env++;
    uint64_t base=0, entry=0, phva=0, n=0, random=0;
    for(uint64_t *a=env+1;*a;a+=2) {
        if(a[0]==AT_BASE)base=a[1];if(a[0]==AT_ENTRY)entry=a[1];
        if(a[0]==AT_PHDR)phva=a[1];if(a[0]==AT_PHNUM)n=a[1];
        if(a[0]==AT_RANDOM)random=a[1];
    }
    if(!base||!entry||!phva||!n)fail("auxv");
    relocate(base,_DYNAMIC);
    if(interp_pointer!=&self_value||*interp_pointer!=73)fail("self relocation");
    const volatile unsigned char *payload=file_bytes;
    if(payload[0]!=0x65||payload[4096]!=0x73||payload[12287]!=0x9a)fail("interpreter file pages");
    struct elf64_phdr *ph=(void*)phva,*tls=0;uint64_t bias=0;uint64_t *dyn=0;
    for(uint64_t i=0;i<n;i++)if(ph[i].p_type==6)bias=phva-ph[i].p_vaddr;
    for(uint64_t i=0;i<n;i++) {
        if(ph[i].p_type==2)dyn=(void*)(bias+ph[i].p_vaddr);
        if(ph[i].p_type==7)tls=ph+i;
    }
    if(dyn)relocate(bias,dyn);
    if(tls) {
        uint64_t align=tls->p_align<8?8:tls->p_align;
        uint64_t size=(tls->p_memsz+align-1)&~(align-1);
        if(size>4096)fail("tls cap");
        for(uint64_t i=0;i<tls->p_filesz;i++)tls_buffer[i]=((unsigned char*)(bias+tls->p_vaddr))[i];
        uint64_t tp=(uint64_t)tls_buffer+size;*(uint64_t*)tp=tp;
        if(_sys(SYS_SET_TLS,tp,0,0)<0)fail("tls install");
    }
    seal(bias,ph,n);
    /* The interpreter's own headers are at offset zero in this fixture. */
    uint64_t po=*(uint64_t*)(base+32);uint16_t pn=*(uint16_t*)(base+56);
    seal(base,(void*)(base+po),pn);
    uint64_t selftop=0;struct elf64_phdr *iph=(void*)(base+po);
    for(unsigned i=0;i<pn;i++)if(iph[i].p_type==1&&iph[i].p_vaddr+iph[i].p_memsz>selftop)selftop=iph[i].p_vaddr+iph[i].p_memsz;
    uint64_t metadata[2]={random&~4095ull,base+((selftop+4095)&~4095ull)};
    for(int i=0;i<2;i++) {
        if(*(volatile uint32_t*)metadata[i]!=0x464c457f)fail("metadata image");
        uint64_t area=(uint64_t)_sys(SYS_MMAP,4096,3,metadata[i]);
        if(!area||area==metadata[i])fail("metadata mmap exclusion");
        if(*(volatile uint64_t*)area)fail("anonymous page not zero");
        if(_sys(SYS_MUNMAP,area,4096,0)<0)fail("anonymous release");
    }
    say("PTINTERP_RUNTIME_PASS\n");
    return entry;
}
