#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "img.h"
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
static uint8_t *slurp(const char*p,int*n){FILE*f=fopen(p,"rb");if(!f){printf("no %s\n",p);exit(2);}fseek(f,0,SEEK_END);*n=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(*n);if(fread(b,1,*n,f)!=(size_t)*n)exit(2);fclose(f);return b;}
static void ppm(const char*p,struct image*im){FILE*o=fopen(p,"wb");fprintf(o,"P6\n%d %d\n255\n",im->w,im->h);for(int i=0;i<im->w*im->h;i++)fwrite(im->rgba+i*4,1,3,o);fclose(o);}
static int fail;
static void px(struct image*im,int x,int y,int r,int g,int b,int a){
    uint8_t*p=im->rgba+(y*im->w+x)*4;
    if(p[0]!=r||p[1]!=g||p[2]!=b||p[3]!=a){printf("FAIL px(%d,%d)=%d,%d,%d,%d want %d,%d,%d,%d\n",x,y,p[0],p[1],p[2],p[3],r,g,b,a);fail=1;}
}
int main(void){
    int n; struct image im;
    uint8_t*png=slurp("/tmp/t.png",&n);
    if(img_decode(png,n,&im)){printf("FAIL png decode\n");return 1;}
    printf("png %dx%d\n",im.w,im.h);
    if(im.w!=6||im.h!=4){printf("FAIL png size\n");fail=1;}
    px(&im,1,1,0,0,255,128); px(&im,2,0,0,0,255,128); px(&im,0,0,255,0,0,255);
    ppm("/tmp/png.ppm",&im); img_free(&im);
    uint8_t*gif=slurp("/tmp/t.gif",&n);
    if(img_decode(gif,n,&im)){printf("FAIL gif decode\n");return 1;}
    printf("gif %dx%d\n",im.w,im.h);
    if(im.w!=6||im.h!=4){printf("FAIL gif size\n");fail=1;}
    ppm("/tmp/gif.ppm",&im); img_free(&im);
    printf(fail?"SOME FAILED\n":"ALL PASS\n"); return fail;
}
