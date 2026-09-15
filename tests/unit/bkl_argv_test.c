/* SPDX-License-Identifier: MIT
 * Included after the production argv length validator. Usercopy enforces a
 * readable byte interval; no fixture bytes are exposed for invalid accesses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uintptr_t low,high;
static int calls,checks,failures;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
int user_copy_from(void *dst,const void *src,uint64_t len)
{
    calls++;uintptr_t p=(uintptr_t)src;
    if(p<low || p>high || len>high-p)return -1;
    memcpy(dst,src,(size_t)len);return 0;
}
int main(void)
{
    void *arena=NULL;if(posix_memalign(&arena,4096,8192))return 2;
    low=(uintptr_t)arena;high=low+4096;memset(arena,'x',8192);((char *)arena)[4095]=0;
    CHECK(user_strnlen(arena,8192)==4095,"NUL at final mapped byte does not touch unmapped next page");
    CHECK(calls==16,"argv validation uses sixteen copy calls per 4096-byte page");
    calls=0;CHECK(user_strnlen((char *)arena+4095,2)==0,"unaligned final-byte NUL stays valid");
    CHECK(calls==1,"page edge checks exactly one mapped byte");
    ((char *)arena)[4095]='x';CHECK(user_strnlen(arena,4096)==-2,"missing NUL preserves E2BIG result");
    CHECK(user_strnlen(arena,4097)==-1,"crossing unmapped page preserves EFAULT result");
    CHECK(user_strnlen(NULL,1)==-1,"invalid user pointer remains refused");
    free(arena);printf("BKL_ARGV checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
