#include "openlogit.h"
#include "openlogit_bitmap.h"
#include "openlogit_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks,failed;
#define CHECK(c,s) do{checks++;if(!(c)){failed++;printf("FAIL %s\n",s);}else printf("PASS %s\n",s);}while(0)
int main(void)
{
    void *dm=calloc(1,ol_device_size()),*sm=malloc(ol_surface_size()),*lm=malloc(32768),*im=malloc(ol_image_size());
    struct ol_device *d=0;struct ol_surface *s=0;struct ol_list *l=0;struct ol_image *mask=0;
    unsigned char front[8*8*4]={0},work[sizeof front],coverage[4]={255,128,0,255};
    CHECK(!ol_device_create(dm,ol_device_size(),OL_VERSION(1,0),0,&d),"1.0 callers still create a 1.1 device");
    struct ol_surface_desc sd={sizeof sd,OL_FORMAT_RGBA8_STRAIGHT,8,8,32,front,work,sizeof front,sizeof work};
    CHECK(!ol_surface_create(d,sm,ol_surface_size(),&sd,&s)&&!ol_list_create(d,lm,32768,&l),"1.0 descriptor layouts still work");
    struct ol_image_desc id={sizeof id,OL_FORMAT_A8,2,2,2,coverage,sizeof coverage};
    CHECK(!ol_image_create(d,im,ol_image_size(),&id,&mask),"coverage resources need one A8 buffer");
    int points[32],subs[4];struct gfx_path path;gfx_path_init(&path,points,16,subs,4);
    gfx_path_rect(&path,GFX_PX(1),GFX_PX(1),GFX_PX(2),GFX_PX(2));
    struct gfx_matrix transform;gfx_m_identity(&transform);gfx_m_translate(&transform,GFX_PX(2),GFX_PX(2));
    struct gfx_paint red;gfx_paint_solid(&red,0xff0000,255);
    struct ol_fill_options opt={sizeof opt,&transform,0,mask,3,3,128,16};
    CHECK(!ol_cmd_fill_ex(l,&path,GFX_NONZERO,&red,&opt),"transformed translucent path retains its clip resource");
    CHECK(ol_image_destroy(mask)==OL_BUSY,"recorded path clip retains lifetime");
    ol_list_close(l);struct ol_submit_info info={.size=sizeof info};
    CHECK(!ol_submit_damage(d,l,s,&info)&&front[3*32+3*4]==255&&front[3*32+3*4+3]==128&&
          front[3*32+4*4+3]==64&&front[4*32+3*4+3]==0,"transform opacity and A8 clip affect actual pixels");
    CHECK(info.damage.x==3&&info.damage.y==3&&info.damage.w==2&&info.damage.h==2&&info.copied_bytes==32,
          "small draw copies its damage region rather than the full surface");
    unsigned char replacement=0;uint64_t serial=ol_device_completed(d);unsigned char old[sizeof front];memcpy(old,front,sizeof old);
    CHECK(!ol_image_update(mask,(struct gfx_rect){0,0,1,1},&replacement,1,1)&&
          ol_submit(d,l,s,0)==OL_STALE_RESOURCE&&!memcmp(old,front,sizeof old)&&ol_device_completed(d)==serial,
          "updated clip rejects stale commands and preserves the previous frame");
    ol_list_reset(l);ol_cmd_glyph_mask(l,mask,0,0,0x00ff00,255);ol_list_close(l);
    CHECK(!ol_submit(d,l,s,0)&&front[4+1]==255&&front[4+3]==128,"glyph command consumes coverage as alpha");
    struct gfx_rect dirty=ol_damage_move((struct gfx_rect){2,4,3,5},(struct gfx_rect){8,9,3,5},2,20,20);
    CHECK(dirty.x==0&&dirty.y==2&&dirty.w==13&&dirty.h==14,"moving effect damages old new and expanded bounds");
    struct gfx_surface src={front,8,8,32};unsigned char read[3*3*4];struct gfx_surface out={read,3,3,12};
    CHECK(!ol_bitmap_read(&src,-1,-1,&out)&&read[3]==0&&read[4*5+1]==front[5],"readback includes transparent pixels outside the source");
    uint32_t pix[12]={1,2,3,4,5,6};
    CHECK(!ol_bitmap_reshape(pix,3,3,pix,2,3,99)&&pix[0]==1&&pix[1]==2&&pix[2]==99&&pix[3]==3&&pix[6]==5,
          "in-place resize preserves rows when the stride grows");
    CHECK(!ol_bitmap_reshape(pix,2,3,pix,3,3,99)&&pix[2]==3&&pix[4]==5,"in-place resize preserves rows when the stride shrinks");
    struct ol_display display={0};uint32_t native=0x0000ff;struct ol_pixel_target target={.px=&native,.w=1,.h=1};
    unsigned char rgba[4]={255,0,0,128};
    CHECK(!ol_display_bind(&display,&target,16,8,0),"display channel layout is explicit BGRX");
    ol_display_blit_rgba(&display,0,0,1,1,rgba,1,1);
    CHECK(native==0x80007f,"RGBA to BGRX preserves channel and alpha semantics");
    native=0x0000ff;uint32_t argb=0;
    CHECK(!ol_display_pack_argb(&display,&argb,rgba,1)&&argb==0x80ff0000,"cursor packing preserves straight alpha");
    ol_display_blit_argb(&display,0,0,&argb,1,1,1);
    CHECK(native==0x80007f,"cursor fallback blends through the SDK");
    native=0x0000ff;uint32_t red_native=0xff0000;struct ol_pixel_target layer={.px=&red_native,.w=1,.h=1};
    ol_display_blit_surface_alpha(&display,0,0,1,1,&layer,128);
    CHECK(native==0x80007f,"window layer opacity uses native channel order");
    ol_list_destroy(l);ol_image_destroy(mask);ol_surface_destroy(s);ol_device_destroy(d);free(dm);free(sm);free(lm);free(im);
    printf("OpenLogit 1.1: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
