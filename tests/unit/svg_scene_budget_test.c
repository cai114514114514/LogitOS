/* SPDX-License-Identifier: MIT
 * Use the same registry/allocation shim and actual decoder; a small compile-
 * time budget tests the accounting seam without a large/slow stress payload. */
#define main svg_scene_regular_main
#include "svg_scene_test.c"
#undef main
int main(void)
{
    svg_register();
    struct image im=dec(SVG("<g opacity='.5'><rect width='16' height='16' fill='red'/></g>"));
    check(pixel(&im,8,8,255,0,0,127),"small ordinary opacity group fits budget");done(&im);
    char text[4096];strcpy(text,"<svg width='16' height='16'>");
    for(int i=0;i<20;i++)strcat(text,"<g opacity='.5'/>");
    strcat(text,"</svg>");
    struct image failed={0};int rc=decode((const uint8_t*)text,(int)strlen(text),&failed);
    check(rc!=0,"surface work budget rejects empty opacity groups");free(failed.rgba);
    const char *viewport=SVG("<svg width='16' height='16' viewBox='0 0 .00001 .00001'/> ");
    struct image range={0};int vr=decode((const uint8_t*)viewport,(int)strlen(viewport),&range);
    /* Coordinates below 24.8 precision are a zero viewBox (non-rendering), not
     * a representable magnification, so this case remains a successful image. */
    check(!vr,"zero viewBox is non-rendering without a matrix overflow");free(range.rgba);
    unsigned long size=4UL*1024*1024+128;char *source=malloc(size);
    memset(source,' ',size);memcpy(source,"<svg data-large='",17);memcpy(source+size-3,"'/>",3);
    struct image too_big={0};check(decode((const uint8_t*)source,(int)size,&too_big)!=0,"source limit includes root attributes");free(too_big.rgba);free(source);
    printf("svg-scene-budget: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
