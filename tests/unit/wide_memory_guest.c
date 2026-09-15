/* SPDX-License-Identifier: MIT
 * Native LogitOS ABI acceptance. Physical identity / device swap are verified
 * by the separately linked WIDEVERIFY kernel probe; these accesses run ring 3. */
#include "clib.h"
#include "wide_memory_verify.h"
static int failures;
static void ck(int ok,const char *what){outs(ok?"[wideuser] PASS ":"[wideuser] FAIL ");outs(what);outs("\n");if(!ok)failures++;}
static void sample(struct logit_meminfo *m){*m=(struct logit_meminfo){0};sys_meminfo(m);}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    ck(_sys(SYS_MEMINFO,0,MMCTL_WIDE_VERIFY,0)==0,"kernel high-frame, COW, swap and cache probe");
    struct logit_meminfo before,reserved,touched,after;
    sample(&before);
    const unsigned long n=8ul<<30;
    volatile unsigned long *p=sys_mmap(n,MMAP_PROT_READ|MMAP_PROT_WRITE);
    ck((unsigned long)p>=0x10000000000ul,"8 GiB reservation chooses wide window");
    if(!p)return 1;
    sample(&reserved);
    ck(reserved.mmap_reserved>=before.mmap_reserved+n,"64-bit reservation accounting");
    ck(reserved.frames_used<=before.frames_used+32,"reservation allocates fewer than 32 frames");
    unsigned long offsets[]={0,4096,1ul<<30,4ul<<30,n-4096};
    for(unsigned i=0;i<5;i++){volatile unsigned long *q=(void*)((unsigned long)p+offsets[i]);ck(*q==0,"first touch supplies zero page");*q=0x12340000ul+i;}
    sample(&touched);
    ck(touched.frames_used<reserved.frames_used+64,"five sparse touches consume under 64 frames");
    int child=sys_fork();ck(child>=0,"fork wide mapping");
    if(!child){
        for(unsigned i=0;i<5;i++){volatile unsigned long *q=(void*)((unsigned long)p+offsets[i]);if(*q!=0x12340000ul+i)return 31;*q=0xabc000ul+i;}
        return 0;
    }
    if(child>0){int st=-1;ck(sys_waitpid(child,&st)==child&&st==0,"child reads and privately writes every wide page");}
    for(unsigned i=0;i<5;i++)ck(*(volatile unsigned long*)((unsigned long)p+offsets[i])==0x12340000ul+i,"parent retains COW contents");
    char *message=(char*)p;c_strcpy(message,"[wideuser] high buffer usercopy\n",80);
    ck(sys_write(1,message,c_strlen(message))==c_strlen(message),"usercopy accepts a real wide buffer");
    ck(_sys(SYS_MPROTECT,(long)p,4096,MMAP_PROT_READ)==0,"mprotect high page read-only");
    child=sys_fork();ck(child>=0,"fork permission test");
    if(!child){*p=17;return 99;}
    if(child>0){int st=0;sys_waitpid(child,&st);ck(st!=0&&st!=99,"read-only write is killed by page protection");}
    ck(_sys(SYS_MPROTECT,(long)p,4096,MMAP_PROT_READ|MMAP_PROT_WRITE)==0,"restore high page write permission");
    *p=27;ck(*p==27,"restored writable page accepts stores");
    ck(sys_munmap((void*)p,n)==0,"unmap sparse 8 GiB range");
    sample(&after);
    ck(after.mmap_reserved==before.mmap_reserved,"unmap returns entire virtual reservation");
    ck(after.frames_used<=before.frames_used+32,"unmap releases payload and bounded page tables");
    ck(after.mm_bugs==0,"no allocator invariant violations");
    volatile unsigned long *q=sys_mmap_at(8192,MMAP_PROT_READ|MMAP_PROT_WRITE,(void*)(0x10000000000ul+(1ul<<39)-4096));
    ck((unsigned long)q==0x10000000000ul+(1ul<<39)-4096,"explicit mapping crosses high PML4 boundary");
    if(q){q[0]=123;q[512]=456;ck(q[0]==123&&q[512]==456,"ring 3 reads both PML4 subtrees");sys_munmap((void*)q,8192);}
    void *gap=sys_mmap_at(4096,3,(void*)0x90000000ul);
    /* Hints can fall back; they must never grant access to the kernel gap. */
    unsigned long g=(unsigned long)gap;
    ck(!g||(g>=0x40000000ul&&g<0x80000000ul)||(g>=0x10000000000ul&&g<0x800000000000ul),"invalid hint never maps the MMIO gap");
    if(gap)sys_munmap(gap,4096);
    outs(failures?"WIDE_MEMORY_GUEST_FAIL\n":"WIDE_MEMORY_GUEST_PASS\n");return failures?1:0;
}
