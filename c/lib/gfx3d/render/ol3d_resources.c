#include "ols_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
struct ol3d_buffer { unsigned kind; size_t count, stride; uint64_t version; unsigned char data[]; };
struct ol3d_pipeline_object { struct ol3d_pipeline state; struct ol_shader_program vertex,pixel; };
static int diagnostic(struct ol_shader_error *e,unsigned line,const char *s)
{if(e){e->line=line;snprintf(e->message,sizeof e->message,"%s",s);}return OL_ARGUMENT;}
int ol3d_buffer_create(unsigned kind,const void *data,size_t count,struct ol3d_buffer **out)
{
    if(!out)return OL_ARGUMENT;*out=0;
    size_t stride=kind==OL3D_VERTEX_BUFFER?sizeof(struct ol3d_vertex):sizeof(uint32_t);
    size_t limit=kind==OL3D_VERTEX_BUFFER?OL3D_MAX_VERTICES:OL3D_MAX_INDICES;
    if((kind!=OL3D_VERTEX_BUFFER&&kind!=OL3D_INDEX_BUFFER)||!data||!count||count>limit)return OL_ARGUMENT;
    struct ol3d_buffer *b=malloc(sizeof *b+stride*count);if(!b)return OL_LIMIT;
    b->kind=kind;b->count=count;b->stride=stride;b->version=1;
    memcpy(b->data,data,stride*count);*out=b;return OL_OK;
}
int ol3d_buffer_update(struct ol3d_buffer *b,size_t first,size_t count,const void *data)
{
    if(!b||!data||first>b->count||count>b->count-first)return OL_ARGUMENT;
    if(!count)return OL_OK;
    memmove(b->data+first*b->stride,data,count*b->stride);b->version++;return OL_OK;
}
struct ol3d_buffer_view ol3d_buffer_view(const struct ol3d_buffer *b)
{return (struct ol3d_buffer_view){b,b?b->version:0};}
void ol3d_buffer_destroy(struct ol3d_buffer *b){free(b);}
int ol3d_pipeline_validate(const struct ol3d_pipeline *p,const struct ol3d_bindings *bindings,struct ol_shader_error *e)
{
    if(p&&(p->blend_op>=GFX_OP_COUNT||p->depth_func>=OL3D_DEPTH_FUNC_COUNT||p->cull>=OL3D_CULL_COUNT))
        return diagnostic(e,0,"pipeline raster state out of range");
    if(!p||p->varying_count>OL3D_MAX_VARYINGS||ol_shader_validate(p->vertex,e)||ol_shader_validate(p->pixel,e))return OL_ARGUMENT;
    if(p->vertex->stage!=OLS_VERTEX||p->pixel->stage!=OLS_PIXEL)return diagnostic(e,0,"pipeline requires vertex and pixel stages");
    unsigned written=0;
    for(unsigned i=0;i<p->vertex->count;i++)if(p->vertex->code[i].op==OLS_VARYING)
        written|=1u<<p->vertex->code[i].dst;
    for(int stage=0;stage<2;stage++) {
        const struct ol_shader_program *program=stage?p->pixel:p->vertex;
        for(unsigned i=0;i<program->count;i++) {
            const struct ols_instruction *c=&program->code[i];
            if(stage&&c->op==OLS_INPUT&&(c->a>=p->varying_count||!(written&(1u<<c->a))))
                return diagnostic(e,i+1,"pixel input has no linked vertex varying");
        }
        /* Creation checks stage linkage; a draw additionally checks the same
         * uniform/sampler contract used by screen-space materials. */
        if(bindings && ols_bindings_validate(program,bindings,e))return OL_ARGUMENT;
    }
    if(e)*e=(struct ol_shader_error){0};return OL_OK;
}
int ol3d_pipeline_create(const struct ol3d_pipeline *p,struct ol3d_pipeline_object **out,struct ol_shader_error *e)
{
    if(!out)return OL_ARGUMENT;*out=0;
    if(ol3d_pipeline_validate(p,0,e))return OL_ARGUMENT;
    struct ol3d_pipeline_object *o=malloc(sizeof *o);if(!o)return OL_LIMIT;
    o->vertex=*p->vertex;o->pixel=*p->pixel;o->state=*p;
    o->state.vertex=&o->vertex;o->state.pixel=&o->pixel;*out=o;return OL_OK;
}
void ol3d_pipeline_destroy(struct ol3d_pipeline_object *p){free(p);}
int ol3d_draw_buffers(struct ol3d_context *c,const struct ol3d_pipeline_object *p,
                       const struct ol3d_bindings *bindings,struct ol3d_buffer_view v,
                       struct ol3d_buffer_view ix,size_t first,size_t count)
{
    if(!p||!bindings||!v.buffer||!ix.buffer||v.buffer->kind!=OL3D_VERTEX_BUFFER||
       ix.buffer->kind!=OL3D_INDEX_BUFFER||first>ix.buffer->count||count>ix.buffer->count-first)
        return ol3d_abort(c,OL_ARGUMENT);
    if(v.version!=v.buffer->version||ix.version!=ix.buffer->version)return ol3d_abort(c,OL_STALE_RESOURCE);
    struct ol3d_draw draw={p->state,*bindings,(const struct ol3d_vertex *)v.buffer->data,v.buffer->count,
                            (const uint32_t *)ix.buffer->data+first,count};
    return ol3d_draw_indexed(c,&draw);
}
void ol3d_texture_mip_size(unsigned w, unsigned h, unsigned *levels, size_t *bytes)
{
    unsigned n = 0;
    size_t total = 0;
    while (w > 1 || h > 1) {
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        total += (size_t)w * h * 4;
        n++;
    }
    if (levels)
        *levels = n;
    if (bytes)
        *bytes = total;
}
int ol3d_texture_build_mips(const struct ol3d_texture *t, struct ol3d_mip_level *levels,
                            unsigned max_levels, unsigned char *storage, size_t storage_bytes,
                            unsigned *count)
{
    if (count)
        *count = 0;
    if (!t || !t->pixels || !levels || !storage || !count || !t->width || !t->height ||
        t->width > 4096 || t->height > 4096 || t->stride < t->width * 4 ||
        t->bytes < (size_t)(t->height - 1) * t->stride + t->width * 4)
        return OL_ARGUMENT;
    unsigned n;
    size_t need;
    ol3d_texture_mip_size(t->width, t->height, &n, &need);
    if (n > max_levels || need > storage_bytes)
        return OL_LIMIT;
    const unsigned char *src = t->pixels;
    unsigned sw = t->width, sh = t->height, sstride = t->stride;
    unsigned char *dst = storage;
    for (unsigned l = 0; l < n; l++) {
        unsigned dw = sw > 1 ? sw >> 1 : 1, dh = sh > 1 ? sh >> 1 : 1;
        for (unsigned y = 0; y < dh; y++)
            for (unsigned x = 0; x < dw; x++) {
                unsigned x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1,
                         y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
                const unsigned char *p00 = src + (size_t)(2 * y) * sstride + 2 * x * 4,
                                    *p10 = src + (size_t)(2 * y) * sstride + x1 * 4,
                                    *p01 = src + (size_t)y1 * sstride + 2 * x * 4,
                                    *p11 = src + (size_t)y1 * sstride + x1 * 4;
                for (int k = 0; k < 4; k++)
                    dst[((size_t)y * dw + x) * 4 + k] =
                        (unsigned char)((p00[k] + p10[k] + p01[k] + p11[k] + 2) >> 2);
            }
        levels[l] = (struct ol3d_mip_level){dst, dw, dh, dw * 4, (size_t)dw * dh * 4};
        src = dst;
        sw = dw;
        sh = dh;
        sstride = dw * 4;
        dst += (size_t)dw * dh * 4;
    }
    *count = n;
    return OL_OK;
}
