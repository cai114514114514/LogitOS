/* SPDX-License-Identifier: MIT */
#include "clib.h"
#define MMCTL_BKL_VERIFY 0xb001
#define N 4
static unsigned char payload[8192],checkbuf[8192];
static int child_work(int id,int fd,int bench)
{
    char ch=0;
    if(sys_read(fd,&ch,1)!=1)return 10;
    sys_close(fd);
    if(!bench) {
        long rc=_sys(SYS_MEMINFO,0,MMCTL_BKL_VERIFY,N);
        if(rc) {
            outs(rc==-2?"[bkluser] FAIL interruptible syscall\n":"[bkluser] FAIL simultaneous kernel entry\n");return 11;
        }
    }
    for(int iter=0;iter<(bench?2048:32);iter++) {
        unsigned long n=8ul<<30;
        volatile unsigned long *p=sys_mmap(n,3);
        if(!p||(unsigned long)p<0x10000000000ul)return 12;
        unsigned long at[]={0,4096,4ul<<30,n-4096};
        for(int i=0;i<4;i++)*(volatile unsigned long*)((unsigned long)p+at[i])=0x900000ul+id*1000+iter*10+i;
        if(!bench && !(iter&7)) {
            int kid=sys_fork();if(kid<0)return 13;
            if(!kid) {
                for(int i=0;i<4;i++) {volatile unsigned long*q=(void*)((unsigned long)p+at[i]);if(*q!=0x900000ul+id*1000+iter*10+i)return 14;*q=7;}
                return 0;
            }
            int st=-1;if(sys_waitpid(kid,&st)!=kid||st)return 15;
        }
        if(_sys(SYS_MPROTECT,(long)p,4096,1))return 16;
        if(_sys(SYS_MPROTECT,(long)p,4096,3))return 17;
        for(int i=0;i<4;i++)if(*(volatile unsigned long*)((unsigned long)p+at[i])!=0x900000ul+id*1000+iter*10+i)return 18;
        if(sys_munmap((void*)p,n))return 19;
    }
    char path[]="/bkl-0.dat";path[5]=(char)('0'+id);
    for(int i=0;i<8192;i++)payload[i]=(unsigned char)(i*13+id*37);
    for(int round=0;round<8;round++) {
        int f=sys_open(path,O_WRONLY|O_CREAT|O_TRUNC);if(f<0)return 20;
        if(sys_write(f,payload,8192)!=8192)return 21;sys_close(f);
        f=sys_open(path,0);if(f<0)return 22;
        if(sys_read(f,checkbuf,8192)!=8192)return 23;sys_close(f);
        for(int i=0;i<8192;i++)if(checkbuf[i]!=payload[i])return 24;
    }
    if(_sys(SYS_DELETE_FILE,(long)path,0,0))return 25;
    return 0;
}
int main(int argc,char**argv)
{
    int bench=argc>1 && argv[1][0]=='b';
    int fds[2],pids[N];if(sys_pipe(fds))return 2;
    unsigned long long before=monotonic_ns();
    for(int i=0;i<N;i++) {
        int pid=sys_fork();if(pid<0)return 3;
        if(!pid) {sys_close(fds[1]);return child_work(i,fds[0],bench);}
        pids[i]=pid;
    }
    sys_close(fds[0]);if(sys_write(fds[1],"go!!",N)!=N)return 4;sys_close(fds[1]);
    int failed=0;
    for(int i=0;i<N;i++) {int st=-1;int got=sys_waitpid(pids[i],&st);if(got!=pids[i]||st){failed++;outs("[bkluser] child status=");outn(st);outs("\n");}}
    unsigned long long elapsed=monotonic_ns()-before;
    outs("BKL_WORK_MS ");outn((long)(elapsed/1000000));outs(" children=4\n");
    outs(failed?"BKL_GUEST_FAIL\n":"BKL_GUEST_PASS\n");return failed?1:0;
}
