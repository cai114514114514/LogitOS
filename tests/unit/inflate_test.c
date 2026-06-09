#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "inflate.h"
static uint8_t *slurp(const char *p, int *n){
    FILE *f=fopen(p,"rb"); if(!f){printf("no %s\n",p);exit(2);}
    fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(*n); if(fread(b,1,*n,f)!=(size_t)*n)exit(2); fclose(f); return b;
}
int main(void){
    int on,rn,zn; uint8_t *orig=slurp("/tmp/orig",&on), *raw=slurp("/tmp/raw",&rn), *zl=slurp("/tmp/zl",&zn);
    static uint8_t out[1<<20]; int ol; int fail=0;
    if(inflate_raw(raw,rn,out,sizeof out,&ol)||ol!=on||memcmp(out,orig,on)){printf("FAIL raw: rc/len %d\n",ol);fail=1;}
    else printf("raw  OK (%d -> %d)\n",rn,ol);
    if(zlib_decompress(zl,zn,out,sizeof out,&ol)||ol!=on||memcmp(out,orig,on)){printf("FAIL zlib: len %d\n",ol);fail=1;}
    else printf("zlib OK (%d -> %d)\n",zn,ol);
    printf(fail?"SOME FAILED\n":"ALL PASS\n"); return fail;
}
