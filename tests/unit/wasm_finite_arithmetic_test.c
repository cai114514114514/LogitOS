/* Normal finite arithmetic acceptance for the browser's reader codegen.
 * Every module is self-authored, valid and bounded. Both codegen choices must
 * give the same arithmetic answers; performance is measured separately, never
 * turned into a host wall-clock pass/fail threshold. No browser lifecycle or
 * network dependency is needed to exercise the unchanged reader bodies. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../../c/lib/wasm/wasm_exec.h"
#ifndef WASM_FINITE_BASELINE
#define WASM_BROWSER_UNITY_READERS
#endif
#include "../../c/lib/wasm/wasm_parse.c"
#ifdef WASM_BROWSER_UNITY_READERS
#undef WASM_BROWSER_UNITY_READERS
#endif
#include "../../c/lib/wasm/wasm_valid.c"
#include "../../c/lib/wasm/wasm_exec.c"

struct bytes { unsigned char b[512]; unsigned n; };
static void byte(struct bytes *b,unsigned x){ b->b[b->n++]=(unsigned char)x; }
static void leb(struct bytes *b,unsigned x){ do {unsigned q=x&127;x>>=7;byte(b,q|(x?128:0));}while(x); }
static void raw(struct bytes *b,const void *p,unsigned n){memcpy(b->b+b->n,p,n);b->n+=n;}
static void section(struct bytes *m,unsigned id,struct bytes *b){byte(m,id);leb(m,b->n);raw(m,b->b,b->n);}

static void signed_leb(struct bytes *b, int32_t value)
{
    int more=1;
    while(more){
        unsigned x=(uint32_t)value&127u;
        /* Division expressed from unsigned bits to avoid depending on the
         * implementation-defined signed-right-shift in the fixture encoder. */
        uint32_t u=(uint32_t)value;
        u=(u>>7)|((u&0x80000000u)?0xfe000000u:0u);
        value=(int32_t)u;
        more=!((value==0 && !(x&64)) || (value==-1 && (x&64)));
        byte(b,x|(more?128:0));
    }
}
static void op_u(struct bytes *b, unsigned op, unsigned n){byte(b,op);leb(b,n);}
static struct bytes valid_module(int wide, int32_t constant, int helper)
{
    struct bytes m={0},s={0},body={0};
    unsigned i=wide?128:1, sum=wide?129:2;
    const unsigned char magic[]={0,97,115,109,1,0,0,0};raw(&m,magic,8);
    const unsigned char ty[]={1,0x60,1,0x7f,1,0x7f};raw(&s,ty,sizeof ty);section(&m,1,&s);
    s.n=0;leb(&s,helper?2:1);byte(&s,0);if(helper)byte(&s,0);section(&m,3,&s);
    s.n=0;byte(&s,1);byte(&s,3);raw(&s,"sum",3);byte(&s,0);byte(&s,0);section(&m,7,&s);
    byte(&body,1);leb(&body,wide?129:2);byte(&body,0x7f);
    byte(&body,0x02);byte(&body,0x40);byte(&body,0x03);byte(&body,0x40);
    op_u(&body,0x20,i);op_u(&body,0x20,0);byte(&body,0x4f);op_u(&body,0x0d,1);
    op_u(&body,0x20,sum);byte(&body,0x41);signed_leb(&body,constant);
    if(helper)op_u(&body,0x10,1);
    byte(&body,0x6a);op_u(&body,0x21,sum);
    op_u(&body,0x20,i);byte(&body,0x41);signed_leb(&body,1);byte(&body,0x6a);op_u(&body,0x21,i);
    op_u(&body,0x0c,0);byte(&body,0x0b);byte(&body,0x0b);op_u(&body,0x20,sum);byte(&body,0x0b);
    s.n=0;byte(&s,helper?2:1);leb(&s,body.n);raw(&s,body.b,body.n);
    if(helper){const unsigned char id[]={0,0x20,0,0x0b};leb(&s,sizeof id);raw(&s,id,sizeof id);}
    section(&m,10,&s);return m;
}
int main(void)
{
    const int32_t constants[]={-123456,-65,-64,-1,0,1,63,64,123456};
    const uint32_t counts[]={0,1,7,127,128,1000};
    unsigned checks=0,failed=0,modules=0;
    for(int wide=0;wide<2;wide++)for(int helper=0;helper<2;helper++)
    for(unsigned c=0;c<sizeof(constants)/sizeof(constants[0]);c++){
        struct bytes b=valid_module(wide,constants[c],helper);
        void *mem=malloc(16u*1024u*1024u);if(!mem)return 2;
        struct wasm_arena arena;struct wasm_module m;struct wasm_instance *in=NULL;uint32_t fi=0;
        wasm_arena_init(&arena,mem,16u*1024u*1024u);
        int e=wasm_load(b.b,b.n,&m,&arena);if(!e)e=wasm_instantiate(&in,&m,&arena,NULL,0);
        if(!e&&!wasm_export_index(in,"sum",3,WASM_EXT_FUNC,&fi))e=WASM_TRAP_UNLINKABLE;
        if(e){fprintf(stderr,"APPARATUS setup=%d wide=%d constant=%d helper=%d\n",e,wide,constants[c],helper);free(mem);return 2;}
        modules++;
        for(unsigned n=0;n<sizeof(counts)/sizeof(counts[0]);n++){
            union wasm_val a={.i32=counts[n]},r={0};
            uint32_t expected=counts[n]*(uint32_t)constants[c];
            e=wasm_invoke(in,fi,&a,&r);checks++;
            if(e||r.i32!=expected){failed++;fprintf(stderr,"FAIL wide=%d constant=%d helper=%d n=%u expected=%u actual=%u status=%d\n",wide,constants[c],helper,counts[n],expected,r.i32,e);}
        }
        free(mem);
    }
    printf("valid finite immediates: %u modules, %u checks, %u failed\n",modules,checks,failed);
    return failed?1:0;
}
