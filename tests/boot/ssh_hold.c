/* A normal program which delays stdin consumption until a separate command
 * creates a release file. This proves another SSH channel can make progress
 * while this channel's pipe and receive window are full; no host timing is
 * used as a throughput or latency measurement. */
#include "clib.h"
#include "logit_stat.h"
int main(int argc,char **argv)
{
    if(argc==3&&c_streq(argv[1],"--idle")){
        unsigned long long end=monotonic_ns()+(unsigned)c_atoi(argv[2])*1000000000ull;
        while(monotonic_ns()<end)sys_sleep_ms(20);
        outs("IDLE_DONE\n");return 0;
    }
    if(argc!=2)return 2;
    struct logit_stat s;unsigned long long end=monotonic_ns()+60000000000ull;
    while(st_lstat(argv[1],&s)<0){if(monotonic_ns()>end)return 3;sys_sleep_ms(10);}
    char b[4096];for(;;){int n=sys_read(0,b,sizeof b);if(n==SIG_E_INTR)continue;if(n<0)return 4;if(!n)return 0;
        int off=0;while(off<n){int z=sys_write(1,b+off,n-off);if(z==SIG_E_INTR)continue;if(z<=0)return 5;off+=z;}}
}
