#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "text.h"
#include "ttf.h"
static int fail;
static void dump_pgm(const char *path, const uint8_t *cov, int w, int h){
    FILE *o=fopen(path,"wb"); fprintf(o,"P5\n%d %d\n255\n",w,h);
    for(int i=0;i<w*h;i++){ uint8_t v=255-cov[i]; fwrite(&v,1,1,o);} fclose(o);
}
static void test(struct ttf_font *f, uint32_t cp, const char *pgm){
    static uint8_t cov[768*768]; int w,h,ox,oy;
    int gid=ttf_glyph_id(f,cp);
    int rc=text_raster(f,gid,48,cov,sizeof cov,&w,&h,&ox,&oy);
    if(rc||w<=0||h<=0){printf("FAIL raster %x rc=%d w=%d h=%d\n",cp,rc,w,h);fail=1;return;}
    int nz=0,mid=0,full=0;
    for(int i=0;i<w*h;i++){ if(cov[i]){nz++; if(cov[i]==255)full++; else mid++;} }
    printf("U+%04X gid=%d %dx%d ox=%d oy=%d  nz=%d mid=%d full=%d\n",cp,gid,w,h,ox,oy,nz,mid,full);
    if(nz==0){printf("FAIL %x: all blank\n",cp);fail=1;}
    if(mid==0){printf("FAIL %x: NO intermediate alpha -> not anti-aliased\n",cp);fail=1;}
    dump_pgm(pgm,cov,w,h);
}
int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"fsroot/fonts/ui.ttf";
    FILE*fp=fopen(path,"rb"); if(!fp){printf("no %s\n",path);return 2;}
    fseek(fp,0,SEEK_END);long n=ftell(fp);fseek(fp,0,SEEK_SET);
    uint8_t*buf=malloc(n); if(fread(buf,1,n,fp)!=(size_t)n)return 2; fclose(fp);
    struct ttf_font f; if(ttf_parse(buf,(int)n,&f)){printf("parse fail\n");return 2;}
    test(&f,0x41,"/tmp/A.pgm");
    test(&f,0x4F60,"/tmp/ni.pgm");     /* 你 */
    test(&f,0x597D,"/tmp/hao.pgm");    /* 好 */
    printf(fail?"SOME FAILED\n":"ALL PASS\n");
    return fail;
}
