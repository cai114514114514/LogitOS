/* A LogitOS-ABI binary, never a claim that Linux syscalls are supported.
 * Volatile indirect accesses prevent the optimiser from erasing the very
 * relocations exercised by the test. TLS includes a relocated pointer so
 * copying the raw file into a new TLS block would be detected. */
#include <stdint.h>
#include "logit.h"
#ifndef PIE_ALLOW_FIXED
#define PIE_ALLOW_FIXED 0
#endif
_Static_assert(SYS_SET_TLS == 112, "pie_thread.asm TLS syscall number must match ABI");
long pie_data = 42;
long *volatile pie_pointer = &pie_data;
long pie_fn(void) { return 27; }
long (*volatile pie_function)(void) = pie_fn;
long *const pie_relro = &pie_data;
__thread long pie_tls = 11;
__thread long *pie_tls_pointer = &pie_data;
static unsigned char pie_bss[97];
static unsigned char child_stack[65536] __attribute__((aligned(4096)));
static unsigned char child_tls[8192] __attribute__((aligned(4096)));
extern void pie_thread_start(void);
extern char **environ;
struct phdr { uint32_t type, flags; uint64_t off, va, pa, filesz, memsz, align; };
static void say(const char *s) { long n=0; while(s[n])n++; _sys(SYS_WRITE,1,(long)s,n); }
static void hex(uint64_t n) { char b[19]="0x0000000000000000\n"; for(int i=17;i>=2;i--){ b[i]="0123456789abcdef"[n&15];n>>=4; } _sys(SYS_WRITE,1,(long)b,19); }
static int eq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
void pie_worker(void)
{
    int ok = pie_tls == 11 && pie_tls_pointer == &pie_data && *pie_tls_pointer == 42;
    pie_tls = 91;
    _sys(SYS_THREAD_EXIT,ok ? 123 : 99,0,0);
    for(;;){}
}
int main(int argc, char **argv)
{
    int bad=0;
    int cap_child=argc==2 && eq(argv[1],"capchild");
    if (*pie_pointer!=42 || pie_function()!=27 || pie_relro!=&pie_data) {say("PIE_FAIL pointers\n");bad++;}
    if(pie_tls!=11 || pie_tls_pointer!=&pie_data || *pie_tls_pointer!=42){say("PIE_FAIL main TLS\n");bad++;}
    for(unsigned i=0;i<sizeof pie_bss;i++)if(pie_bss[i])bad++;
    char **env=environ; int envok=0;
    while(*env){if(eq(*env,"PIE_TEST=yes"))envok=1;env++;}
    uint64_t *aux=(uint64_t*)(env+1), phva=0, phnum=0, entry=0, interp=1, execfn=0, uid=~0ull, gid=~0ull, egid=~0ull;
    for(;aux[0];aux+=2){switch(aux[0]){case 3:phva=aux[1];break;case 5:phnum=aux[1];break;case 7:interp=aux[1];break;case 9:entry=aux[1];break;case 11:uid=aux[1];break;case 13:gid=aux[1];break;case 14:egid=aux[1];break;case 31:execfn=aux[1];break;}}
    struct phdr *ph=(struct phdr*)phva,*tls=0;uint64_t bias=0;
    for(uint64_t i=0;i<phnum;i++){if(ph[i].type==6)bias=phva-ph[i].va;if(ph[i].type==7)tls=ph+i;}
    if((!bias&&!PIE_ALLOW_FIXED)||!entry||interp||!execfn||!eq((char*)execfn,argv[0])||(!cap_child&&!envok)){say("PIE_FAIL auxv/env ");hex((uint64_t)envok);bad++;}
    if(uid!=(uint64_t)_sys(SYS_GETUID,0,0,0)||gid!=(uint64_t)_sys(SYS_GETGID,0,0,0)||egid!=gid){say("PIE_FAIL credentials\n");bad++;}
    if(!tls||tls->memsz>4096||tls->align>4096)bad++;
    else {
        uint64_t align=tls->align<8?8:tls->align, size=(tls->memsz+align-1)&~(align-1);
        unsigned char *src=(unsigned char*)(bias+tls->va);
        for(uint64_t i=0;i<tls->filesz;i++)child_tls[i]=src[i];
        uint64_t tp=(uint64_t)child_tls+size;*(uint64_t*)tp=tp;
        struct logit_thread_spec spec={(uint64_t)pie_thread_start,(uint64_t)child_stack+sizeof child_stack,0,0,tp,0};
        long tid=_sys(SYS_THREAD_CREATE,(long)&spec,0,0);uint64_t result=0;
        if(tid<0||_sys(SYS_THREAD_JOIN,tid,(long)&result,0)<0||result!=123||pie_tls!=11){say("PIE_FAIL thread ");hex(result);bad++;}
    }
    say("PIE_BIAS ");hex(bias);
    if(bad){say("PIE_FAIL initial checks ");hex((uint64_t)bad);}
    /* CAP_SPAWN intentionally has no envp field in its ABI, unlike execve.
     * It must still deliver argv, auxv and usable TLS on its first instruction. */
    if(cap_child){say(bad?"PIE_CAP_FAIL\n":"PIE_CAP_PASS\n");return bad?1:0;}
    if(argc==2 && eq(argv[1],"child")){say(bad?"PIE_CHILD_FAIL\n":"PIE_CHILD_PASS\n");return bad?1:0;}
    long pid=_sys(SYS_FORK,0,0,0);
    if(pid==0){char *args[]={argv[0],"child",0};char *ev[]={"PIE_TEST=yes",0};_sys(SYS_EXECVE,(long)argv[0],(long)args,(long)ev);return 97;}
    int status=-1;
    if(pid<0||_sys(SYS_WAITPID,pid,(long)&status,0)<0||status!=0){say("PIE_FAIL fork status ");hex((uint64_t)status);bad++;}
    char *capargs[]={argv[0],"capchild",0};
    struct logit_capreq req={0};
    req.caps=(unsigned long)_sys(SYS_CAP_QUERY,0,0,0);
    long cp=_sys(SYS_CAP_SPAWN,(long)argv[0],(long)capargs,(long)&req);
    status=-1;
    if(cp<0||_sys(SYS_WAITPID,cp,(long)&status,0)<0||status!=0){say("PIE_FAIL cap status ");hex((uint64_t)status);bad++;}
    say(bad?"PIE_PROGRAM_FAIL\n":"PIE_PROGRAM_PASS\n");return bad?1:0;
}
