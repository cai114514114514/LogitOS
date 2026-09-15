/* SPDX-License-Identifier: MIT */
#include "clib.h"
#include "highheap_verify.h"
int main(int argc,char **argv)
{
    int failed=0;
    long mod=_sys(SYS_MODULE_LOAD,(long)"/lib/modules/highheap.ko",0,0);
    if(mod<1){outs("HIGHHEAP_MODULE_FAIL ");outn(mod);outs("\n");return 1;}
    if(argc>1&&argv[1][0]=='m'){outs("HIGHHEAP_MODULE_PASS\n");return 0;}
    if(_sys(SYS_MEMINFO,0,MMCTL_HIGHHEAP_VERIFY,0)){outs("HIGHHEAP_GUEST_FAIL capacity\n");return 1;}
    int fds[2],pids[4];if(sys_pipe(fds))return 2;
    for(int i=0;i<4;i++){
        int pid=sys_fork();if(pid<0)return 3;
        if(!pid){sys_close(fds[1]);char ch;if(sys_read(fds[0],&ch,1)!=1)return 4;sys_close(fds[0]);return _sys(SYS_MEMINFO,0,MMCTL_HIGHHEAP_VERIFY,1)?1:0;}
        pids[i]=pid;
    }
    sys_close(fds[0]);if(sys_write(fds[1],"go!!",4)!=4)return 5;sys_close(fds[1]);
    for(int i=0;i<4;i++){int st=-1;if(sys_waitpid(pids[i],&st)!=pids[i]||st)failed++;}
    outs(failed?"HIGHHEAP_GUEST_FAIL\n":"HIGHHEAP_GUEST_PASS\n");return failed?1:0;
}
