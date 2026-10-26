#include "ol_internal.h"

static int render(struct ol_device *d, struct gfx_surface *dst, const struct gfx_path *p, int rule,
                  const struct gfx_paint *paint, const struct gfx_rect *clip, int samples)
{
    void *workspace = (unsigned char *)d + align8(sizeof *d);
    return ol_sw_raster_fill(workspace, dst, p, rule, paint, clip, samples, 0);
}
static int execute(struct ol_device *d, const struct command *c, struct gfx_surface *target, void *scratch)
{
    if(c->op==CMD_LAYER) {
        const struct ol_surface_desc *a=&c->image->desc;
        struct gfx_surface source={a->front,a->width,a->height,a->stride};
        struct ol_layer_options options=c->layer;
        options.clip=c->has_clip?&c->effect_clip:0;
        ol_sw_layer(target,&source,c->rect,&options,scratch);return 1;
    }
    if(c->op==CMD_BACKDROP) {
        ol_sw_backdrop_rrect(target,c->rect,c->corner,c->layer.blur_radius,c->passes,c->rgb,c->alpha,scratch);return 1;
    }
    if (c->op == CMD_CLEAR || c->op == CMD_CLEAR_RECT) {
        struct gfx_rect rect = {0, 0, target->w, target->h};
        if (c->op == CMD_CLEAR_RECT)
            rect = ol_damage_move(c->rect, (struct gfx_rect){0,0,0,0}, 0, target->w, target->h);
        for (int y = rect.y; y < rect.y + rect.h; y++) {
            unsigned char *row = target->px + (unsigned long)y * target->stride;
            for (int x = rect.x; x < rect.x + rect.w; x++) {
                row[4 * x] = GFX_R(c->rgb);
                row[4 * x + 1] = GFX_G(c->rgb);
                row[4 * x + 2] = GFX_B(c->rgb);
                row[4 * x + 3] = c->alpha;
            }
        }
        return 1;
    }
    if (c->op == CMD_FILL) {
        if (!c->mask) return render(d,target,&c->path,c->rule,&c->paint,
                                   c->has_clip ? &c->rect : 0,c->samples);
        const struct ol_image_desc *a = &c->mask->desc;
        struct gfx_clip_mask mask = {a->pixels,a->width,a->height,c->mask_x,c->mask_y};
        return ol_sw_raster_fill((unsigned char *)d+align8(sizeof *d), target,&c->path,
                                c->rule,&c->paint,c->has_clip?&c->rect:0,c->samples,&mask);
    }
    if (c->op == CMD_GLYPH) {
        const struct ol_image_desc *a = &c->resource->desc;
        for (int y=0;y<(int)a->height;y++) {
            int dy=c->rect.y+y;
            if(dy<0 || dy>=target->h) continue;
            for(int x=0;x<(int)a->width;x++) {
                int dx=c->rect.x+x;
                if(dx<0 || dx>=target->w) continue;
                gfx_over(target->px+(unsigned long)dy*target->stride+dx*4,
                         GFX_R(c->rgb), GFX_G(c->rgb), GFX_B(c->rgb), c->alpha,
                         a->pixels[(unsigned long)y*a->stride+x]);
            }
        }
        return 1;
    }
    unsigned aw,ah,astride; const unsigned char *apixels;
    if(c->op==CMD_RESOURCE) {
        aw=c->resource->desc.width; ah=c->resource->desc.height;
        astride=c->resource->desc.stride; apixels=c->resource->desc.pixels;
    } else {
        aw=c->image->desc.width; ah=c->image->desc.height;
        astride=c->image->desc.stride; apixels=c->image->desc.front;
    }
    struct gfx_matrix m;
    gfx_m_identity(&m);
    m.a = (int)((long long)c->rect.w * GFX_MONE / aw);
    m.d = (int)((long long)c->rect.h * GFX_MONE / ah);
    m.e = GFX_PX(c->rect.x);
    m.f = GFX_PX(c->rect.y);
    struct gfx_paint paint;
    if (!gfx_paint_image(&paint, apixels, aw, ah, astride, &m, c->samples))
        return 0;
    paint.global_alpha = c->alpha;
    int pts[16], subs[2];
    struct gfx_path p;
    gfx_path_init(&p, pts, 8, subs, 2);
    gfx_path_rect(&p, GFX_PX(c->rect.x), GFX_PX(c->rect.y), GFX_PX(c->rect.w), GFX_PX(c->rect.h));
    return render(d, target, &p, GFX_NONZERO, &paint, 0, GFX_SUBS);
}
static int submit(struct ol_device *d, const struct ol_list *l, struct ol_surface *s, uint64_t *serial,
                  struct ol_submit_info *info,void *scratch,unsigned long scratch_bytes)
{
    if (serial)
        *serial = 0;
    if (!device_ok(d) || !list_ok(l) || !surface_ok(s) || l->device != d || s->device != d)
        return OL_ARGUMENT;
    if (l->error)
        return l->error;
    if (!l->closed)
        return OL_STATE;
    unsigned long required=0;
    int requirement=ol_submit_workspace_size(l,s,&required);
    if(requirement)return requirement;
    if(required && (!scratch || scratch_bytes<required))return OL_LIMIT;
    if(required && (!aligned(scratch) || (unsigned long)scratch>~0ul-scratch_bytes ||
       overlap(scratch,scratch_bytes,s->desc.front,s->desc.front_bytes) ||
       overlap(scratch,scratch_bytes,s->desc.work,s->desc.work_bytes) ||
       overlap(scratch,scratch_bytes,l,l->capacity) || overlap(scratch,scratch_bytes,d,ol_device_size()) ||
       overlap(scratch,scratch_bytes,s,ol_surface_size())))return OL_ARGUMENT;
    if (!lock(d))
        return OL_BUSY;
    int result = OL_OK;
    for (unsigned long o = align8(sizeof *l); o < l->used;) {
        const struct command *c = (const void *)((const unsigned char *)l + o);
        const struct ol_image *sources[2]={c->resource,c->mask};
        uint64_t versions[2]={c->resource_generation,c->mask_generation};
        for(int i=0;i<2;i++) if(sources[i]) {
            const struct ol_image *im=sources[i];
            if(im->magic!=OL_IMAGE_MAGIC || im->generation!=versions[i]) {result=OL_STALE_RESOURCE;break;}
            if(overlap(im->desc.pixels,im->desc.bytes,s->desc.front,s->desc.front_bytes) ||
               overlap(im->desc.pixels,im->desc.bytes,s->desc.work,s->desc.work_bytes)) {result=OL_ARGUMENT;break;}
            if(required && (overlap(scratch,scratch_bytes,im,ol_image_size()) ||
               overlap(scratch,scratch_bytes,im->desc.pixels,im->desc.bytes))) {result=OL_ARGUMENT;break;}
        }
        if(result)break;
        if (c->image) {
            const struct ol_surface *image = c->image;
            if (!surface_ok(image) || image->generation != c->generation) {
                result = OL_STALE_RESOURCE;
                break;
            }
            if (image == s ||
                overlap(image->desc.front, image->desc.front_bytes, s->desc.front,
                        s->desc.front_bytes) ||
                overlap(image->desc.front, image->desc.front_bytes, s->desc.work,
                        s->desc.work_bytes)) {
                result = OL_ARGUMENT;
                break;
            }
            if(required && (overlap(scratch,scratch_bytes,image,ol_surface_size()) ||
               overlap(scratch,scratch_bytes,image->desc.front,image->desc.front_bytes) ||
               overlap(scratch,scratch_bytes,image->desc.work,image->desc.work_bytes))) {
                result=OL_ARGUMENT;break;
            }
        }
        o += c->bytes;
    }
    if (result) {
        unlock(d);
        return result;
    }
    struct gfx_surface target = {s->desc.work, s->desc.width, s->desc.height, s->desc.stride};
    struct gfx_rect damage={0,0,0,0},reads={0,0,0,0}; uint64_t count=0;
    for(unsigned long o=align8(sizeof *l);o<l->used;) {
        const struct command *c=(const void *)((const unsigned char *)l+o);
        struct gfx_rect r=c->rect;
        if(c->op==CMD_CLEAR) r=(struct gfx_rect){0,0,target.w,target.h};
        if(c->op==CMD_FILL) {
            int x0,y0,x1,y1;
            if(!gfx_path_bounds(&c->path,&x0,&y0,&x1,&y1)) r=(struct gfx_rect){0,0,0,0};
            else r=(struct gfx_rect){x0,y0,x1-x0,y1-y0};
        }
        if(c->op==CMD_LAYER) {
            r=ol_sw_effect_bounds(c->rect,c->layer.blur_radius,target.w,target.h);
            if(c->layer.shadow_opacity) {
                struct gfx_rect shadow=c->rect;shadow.x+=c->layer.shadow_dx;shadow.y+=c->layer.shadow_dy;
                shadow=ol_sw_effect_bounds(shadow,c->layer.shadow_radius,target.w,target.h);
                r=ol_damage_move(r,shadow,0,target.w,target.h);
            }
            if(c->has_clip) {
                int right=r.x+r.w,bottom=r.y+r.h;
                if(r.x<c->effect_clip.x)r.x=c->effect_clip.x;
                if(r.y<c->effect_clip.y)r.y=c->effect_clip.y;
                if(right>c->effect_clip.x+c->effect_clip.w)right=c->effect_clip.x+c->effect_clip.w;
                if(bottom>c->effect_clip.y+c->effect_clip.h)bottom=c->effect_clip.y+c->effect_clip.h;
                r.w=right>r.x?right-r.x:0;r.h=bottom>r.y?bottom-r.y:0;
            }
        }
        if(c->op==CMD_BACKDROP)reads=ol_damage_move(reads,
            ol_sw_effect_bounds(r,ol_sw_backdrop_reach(c->layer.blur_radius,c->passes),target.w,target.h),
            0,target.w,target.h);
        damage=ol_damage_move(damage,r,0,target.w,target.h);
        count++; o+=c->bytes;
    }
    /* Backdrop dependencies can lie outside output damage. Initialize their
     * halo from front too; uninitialized work pixels otherwise leak into blur
     * at the edge, even though the committed rectangle itself is correct. */
    struct gfx_rect initial=ol_damage_move(damage,reads,0,target.w,target.h);
    /* A leading full clear defines every work pixel, including backdrop read
     * halos. Commit still happens only after the entire list succeeds. */
    if (count) {
        const struct command *first = (const void *)((const unsigned char *)l + align8(sizeof *l));
        if (first->op == CMD_CLEAR)
            initial = (struct gfx_rect){0, 0, 0, 0};
    }
    for(int y=initial.y;y<initial.y+initial.h;y++)
        copy(target.px+(unsigned long)y*target.stride+initial.x*4,
             s->desc.front+(unsigned long)y*s->desc.stride+initial.x*4,initial.w*4);

    for (unsigned long o = align8(sizeof *l); o < l->used;) {
        const struct command *c = (const void *)((const unsigned char *)l + o);
        if (!execute(d, c, &target, scratch)) {
            result = OL_RENDER_FAILED;
            break;
        }
        o += c->bytes;
    }
#ifdef OPENLOGIT_NO_COMMIT
    result = OL_RENDER_FAILED; /* Private control: ordinary rendering must fail. */
#endif
    if (!result) {
        for(int y=damage.y;y<damage.y+damage.h;y++)
            copy(s->desc.front+(unsigned long)y*s->desc.stride+damage.x*4,
                 target.px+(unsigned long)y*target.stride+damage.x*4,damage.w*4);
        if(info){info->damage=damage; info->copied_bytes=((uint64_t)damage.w*damage.h+(uint64_t)initial.w*initial.h)*4;
                 info->commands=count; info->completion_serial=d->completed+1;}
        s->generation++;
        d->completed++;
        if (serial)
            *serial = d->completed;
    }
    unlock(d);
    return result;
}
int ol_submit(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,uint64_t *serial)
{return submit(d,l,s,serial,0,0,0);}
int ol_submit_damage(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,struct ol_submit_info *info)
{
    if(!info||info->size<sizeof *info)return OL_ARGUMENT;
    *info=(struct ol_submit_info){.size=sizeof *info};return submit(d,l,s,0,info,0,0);
}

int ol_submit_workspace_size(const struct ol_list *l,const struct ol_surface *s,unsigned long *out)
{
    if(!out)return OL_ARGUMENT;*out=0;
    if(!list_ok(l)||!surface_ok(s)||l->device!=s->device)return OL_ARGUMENT;
    if(l->error)return l->error;if(!l->closed)return OL_STATE;
    for(unsigned long o=align8(sizeof *l);o<l->used;) {
        const struct command *c=(const void *)((const unsigned char *)l+o);
        unsigned long need=0;
        if(c->op==CMD_LAYER) {
            int radius=c->layer.blur_radius;
            if(c->layer.shadow_opacity&&c->layer.shadow_radius>radius)radius=c->layer.shadow_radius;
            if(radius)need=(s->desc.width+2*radius)*4*sizeof(unsigned);
        } else if(c->op==CMD_BACKDROP)
            need=ol_sw_backdrop_rrect_size(c->rect,c->corner,c->layer.blur_radius,c->passes,
                                           s->desc.width,s->desc.height);
        if(need>*out)*out=need;o+=c->bytes;
    }
    return OL_OK;
}
int ol_submit_workspace(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,
                         void *scratch,unsigned long bytes,struct ol_submit_info *info)
{
    if(info) {if(info->size<sizeof *info)return OL_ARGUMENT;*info=(struct ol_submit_info){.size=sizeof *info};}
    return submit(d,l,s,0,info,scratch,bytes);
}
