/* SPDX-License-Identifier: MIT */
#include "clib.h"
__thread volatile long retained_tls=0x517e;
static volatile unsigned long retained_data=0xabcdef;
extern long ptinterp_fork_fp(int *ok);
int main(void)
{
    int fp_ok=0;long fp_pid=ptinterp_fork_fp(&fp_ok);
    if(!fp_pid)return fp_ok?0:95;
    int fp_status=-1;
    if(fp_pid<0||sys_waitpid(fp_pid,&fp_status)!=fp_pid||fp_status||!fp_ok){outs("PTINTERP_FP_FAIL\n");return 95;}
    outs("PTINTERP_FP_PASS x87 MXCSR XMM0-15 parent child\n");
    if(_sys(SYS_CHMOD,(long)"/lib/ld-logit-test.so",0755,0)<0 ||
       _sys(SYS_CHMOD,(long)"/lib/ld-logit-loop.so",0755,0)<0 ||
       _sys(SYS_CHMOD,(long)"/lib/ld-logit-junk.so",0755,0)<0 ||
       _sys(SYS_CHMOD,(long)"/lib/ld-logit-deny.so",0644,0)<0)return 89;
    const char *bad[]={"/bin/interp-missing","/bin/interp-denied","/bin/interp-nested","/bin/interp-junk"};
    int failures=0,pipes[2];unsigned long caps=_sys(SYS_CAP_QUERY,0,0,0);
    if(sys_pipe(pipes))return 90;
    struct logit_meminfo before,after;
    for(int repeat=0;repeat<33;repeat++) {
        if(repeat==1)sys_meminfo(&before);
        for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {
            char *args[]={(char*)bad[i],0};char *env[]={"PIE_TEST=yes",0};
            long rc=_sys(SYS_EXECVE,(long)bad[i],(long)args,(long)env);
            char c=0;
            if(rc>=0||retained_tls!=0x517e||retained_data!=0xabcdef||
               caps!=(unsigned long)_sys(SYS_CAP_QUERY,0,0,0)||
               sys_write(pipes[1],"z",1)!=1||sys_read(pipes[0],&c,1)!=1||c!='z')failures++;
        }
    }
    sys_meminfo(&after);sys_close(pipes[0]);sys_close(pipes[1]);
    /* GUI/background allocations are live; report both values. The host gate
     * checks exact transient references and every injected allocation failure. */
    outs("PTINTERP_ROLLBACK frames_before=");outn(before.frames_used);outs(" frames_after=");outn(after.frames_used);outs("\n");
    if(after.frames_used>before.frames_used+2048||after.mm_bugs!=before.mm_bugs)failures++;
    const char *good[]={"/bin/ptinterp","/bin/ptinterp-aex","/bin/ptinterp-fixed"};
    for(unsigned i=0;i<sizeof good/sizeof good[0];i++) {
        int pid=sys_fork();if(pid==0){char *args[]={(char*)good[i],0};char*env[]={"PIE_TEST=yes",0};_sys(SYS_EXECVE,(long)good[i],(long)args,(long)env);return 91;}
        int status=-1;if(pid<0||sys_waitpid(pid,&status)!=pid||status)failures++;
    }
    if(sys_open_path("/apps/ptinterp.aex")<0)failures++;
    if(sys_open_path("/apps/ptinterp-tiny.aex")<0)failures++;
    outs(failures?"PTINTERP_GUEST_FAIL\n":"PTINTERP_GUEST_PASS\n");return failures?1:0;
}
