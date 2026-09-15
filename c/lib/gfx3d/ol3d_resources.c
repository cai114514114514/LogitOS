#include "openlogit_3d.h"
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
    size_t limit=kind==OL3D_VERTEX_BUFFER?65536:196608;
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
            if(!bindings)continue; /* Creation validates the stage interface; draw supplies bindings. */
            if((c->op==OLS_UNIFORM&&(!bindings->uniforms||c->a>=bindings->uniform_count))||
               (c->op==OLS_MAT4&&(!bindings->uniforms||c->b+4>bindings->uniform_count)))
                return diagnostic(e,i+1,"missing uniform binding");
            if(c->op==OLS_TEX2D&&(!bindings->textures||c->b>=bindings->texture_count))
                return diagnostic(e,i+1,"missing texture binding");
        }
    }
    if(bindings) {
        if(bindings->uniform_count>OL3D_MAX_UNIFORMS||bindings->texture_count>OL3D_MAX_TEXTURES||
           (bindings->uniform_count&&!bindings->uniforms)||(bindings->texture_count&&!bindings->textures))
            return diagnostic(e,0,"invalid binding table");
        for(unsigned i=0;i<bindings->uniform_count;i++)for(int k=0;k<4;k++)
            if(!isfinite(bindings->uniforms[i][k]))return diagnostic(e,0,"nonfinite uniform");
        for(unsigned i=0;i<bindings->texture_count;i++) {
            const struct ol3d_texture *t=&bindings->textures[i];
            if(!t->pixels||!t->width||!t->height||t->width>4096||t->height>4096||t->stride<t->width*4||
               t->bytes<(size_t)(t->height-1)*t->stride+t->width*4)
                return diagnostic(e,0,"invalid texture storage");
        }
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
