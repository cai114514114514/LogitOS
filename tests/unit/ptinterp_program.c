/* SPDX-License-Identifier: MIT
 * Reuse the static PIE workload so globals, TLS on both threads, argv/envp,
 * fork+exec and capability spawn exercise the same code across both modes. */
#define main pie_main
#define PIE_ALLOW_FIXED 1
#include "pie_program.c"
#undef main
static const unsigned char main_file_bytes[12288]={[0]=41,[4096]=83,[12287]=117};
int main(int argc,char **argv)
{
    const volatile unsigned char *payload=main_file_bytes;
    if(payload[0]!=41||payload[4096]!=83||payload[12287]!=117){say("PTINTERP_MAIN_FAIL file pages\n");return 98;}
    /* The underlying static-PIE check expects AT_BASE=0. Verify the real
     * interpreter value here, then adapt only that test's static expectation. */
    uint64_t *a=(void*)environ;while(*a)a++;a++;
    uint64_t *base=0;
    for(uint64_t *p=a;*p;p+=2)if(*p==7)base=p+1;
    if(!base||*base<(1ull<<40)||*base==(uint64_t)pie_fn){say("PTINTERP_MAIN_FAIL auxv\n");return 98;}
    uint64_t saved=*base;*base=0;
    int rc=pie_main(argc,argv);
    *base=saved;
    say(rc?"PTINTERP_MAIN_FAIL\n":"PTINTERP_MAIN_PASS\n");
    return rc;
}
