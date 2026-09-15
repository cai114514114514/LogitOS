/* Native MessagePort broker. The previous platform-only WeakMap channel is
 * retained as a fallback when this module is not linked. It cannot transfer:
 * closures and JSValues belong to one runtime. Here packets own serialized
 * bytes and endpoint capabilities, and each realm pumps ONLY its own ports.
 *
 * Transfer has prepare/commit/read stages. All fallible cloning precedes
 * detachment; generation checks run again after author getters have returned.
 * A discarded Window message closes its in-transit ports, not a new owner
 * that happened to reuse a slot. Closing a sender does not erase already
 * queued messages at its live peer. No callback runs from postMessage.
 *
 * Bounds: 256 endpoints, 16 transfers/packet, 64 KiB/packet, 8 MiB total
 * packet bytes, 64 queued messages/endpoint, 32 listeners/port. Endpoint
 * wrappers are retained until close/transfer/realm teardown (not a full
 * reachability-based port GC implementation). No native identifier is public.
 */
#include "js_ports.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#define PORT_MAX 256
#define PORT_XFER 16
#define PORT_BYTES (64*1024)
#define PORT_TOTAL (8*1024*1024)
#define PORT_QUEUE 64
#define PORT_LISTENERS 32
struct port_realm {struct port_realm *next;JSContext *ctx;JSValue event;unsigned cursor;};
struct port_listener {JSValue fn;unsigned id;int once;};
struct endpoint {
    uint64_t id,peer,generation;struct port_realm *owner;
    JSValue object,handler;struct port_listener listeners[PORT_LISTENERS];int nlistener,started,nqueue;
    unsigned listener_id;
    struct js_port_packet *head,*tail;
};
struct port_binding {uint64_t id,generation;};
struct js_port_packet {
    struct js_port_packet *next;unsigned char *bytes;size_t length;uint64_t seq;
    int count,committed;uint64_t ids[PORT_XFER],generations[PORT_XFER];
};
static struct endpoint endpoints[PORT_MAX];
static struct port_realm *realms;
static uint64_t next_id=1,next_sequence=1;
static size_t packet_bytes;
static JSClassID port_class;
static struct endpoint *by_id(uint64_t id)
{if(id)for(int i=0;i<PORT_MAX;i++)if(endpoints[i].id==id)return &endpoints[i];return NULL;}
static struct port_realm *realm(JSContext *ctx)
{for(struct port_realm *r=realms;r;r=r->next)if(r->ctx==ctx)return r;return NULL;}
static JSValue error(JSContext *ctx,const char *name,const char *message)
{
    JSValue e=JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx,e,"name",JS_NewString(ctx,name),JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx,e,"message",JS_NewString(ctx,message),JS_PROP_CONFIGURABLE);
    return JS_Throw(ctx,e);
}
static struct endpoint *owned(JSContext *ctx,JSValueConst v)
{
    struct port_binding *b=JS_GetOpaque(v,port_class);if(!b)return NULL;
    struct endpoint *e=by_id(b->id);
    return e&&e->generation==b->generation&&e->owner&&e->owner->ctx==ctx?e:NULL;
}
static void release_wrapper(struct endpoint *e)
{
    if(!e->owner)return;JSContext *ctx=e->owner->ctx;
    JS_FreeValue(ctx,e->handler);e->handler=JS_UNDEFINED;
    for(int i=0;i<e->nlistener;i++)JS_FreeValue(ctx,e->listeners[i].fn);
    e->nlistener=0;JS_FreeValue(ctx,e->object);e->object=JS_UNDEFINED;
    e->owner=NULL;e->started=0;
}
static void close_endpoint(struct endpoint *e)
{
    if(!e||!e->id)return;
    struct endpoint *peer=by_id(e->peer);if(peer&&peer->peer==e->id)peer->peer=0;
    struct js_port_packet *p=e->head;e->head=e->tail=NULL;e->nqueue=0;
    release_wrapper(e);e->id=0;e->peer=0;
    /* Mark dead before recursive packet disposal: a transit packet can own
     * another endpoint whose queue contains an endpoint already being closed. */
    while(p){struct js_port_packet *n=p->next;js_ports_discard(p);p=n;}
}
void js_ports_discard(struct js_port_packet *p)
{
    if(!p)return;
    if(p->committed)for(int i=0;i<p->count;i++){
        struct endpoint *e=by_id(p->ids[i]);
        if(e&&!e->owner&&e->generation==p->generations[i])close_endpoint(e);
    }
    packet_bytes-=p->length;free(p->bytes);free(p);
}
size_t js_ports_packet_size(const struct js_port_packet *p){return p?p->length:0;}
struct js_port_packet *js_ports_prepare(JSContext *ctx,JSValueConst data,JSValueConst transfer)
{
    struct js_port_packet *p=calloc(1,sizeof *p);if(!p){JS_ThrowOutOfMemory(ctx);return NULL;}
    if(!JS_IsUndefined(transfer)){
        if(JS_IsArray(ctx,transfer)!=1){error(ctx,"TypeError","transfer must be an array in this implementation");goto fail;}
        JSValue l=JS_GetPropertyStr(ctx,transfer,"length");uint32_t n=0;
        int ok=JS_ToUint32(ctx,&n,l);JS_FreeValue(ctx,l);if(ok<0)goto fail;
        if(n>PORT_XFER){error(ctx,"QuotaExceededError","too many transferred ports");goto fail;}
        for(uint32_t i=0;i<n;i++){
            JSValue v=JS_GetPropertyUint32(ctx,transfer,i);
            if(JS_IsException(v)){JS_FreeValue(ctx,v);goto fail;}
            struct endpoint *e=owned(ctx,v);JS_FreeValue(ctx,v);
            if(!e){error(ctx,"DataCloneError","transfer requires a live owned MessagePort");goto fail;}
            for(int j=0;j<p->count;j++)if(p->ids[j]==e->id){error(ctx,"DataCloneError","duplicate transferred port");goto fail;}
            p->ids[p->count]=e->id;p->generations[p->count++]=e->generation;
        }
    }
    size_t len=0;uint8_t *raw=JS_WriteObject(ctx,&len,data,JS_WRITE_OBJ_REFERENCE);
    if(!raw){JS_FreeValue(ctx,JS_GetException(ctx));error(ctx,"DataCloneError","message cannot be cloned");goto fail;}
    if(len>PORT_BYTES||packet_bytes>PORT_TOTAL-len){js_free(ctx,raw);error(ctx,"QuotaExceededError","message byte budget exceeded");goto fail;}
    p->bytes=malloc(len?len:1);if(!p->bytes){js_free(ctx,raw);JS_ThrowOutOfMemory(ctx);goto fail;}
    memcpy(p->bytes,raw,len);js_free(ctx,raw);p->length=len;packet_bytes+=len;return p;
fail:js_ports_discard(p);return NULL;
}
int js_ports_commit(JSContext *ctx,struct js_port_packet *p)
{
    if(!p||p->committed){error(ctx,"DataCloneError","invalid message transfer state");return 0;}
    for(int i=0;i<p->count;i++){
        struct endpoint *e=by_id(p->ids[i]);
        if(!e||!e->owner||e->owner->ctx!=ctx||e->generation!=p->generations[i]){
            error(ctx,"DataCloneError","port ownership changed during serialization");return 0;
        }
    }
    for(int i=0;i<p->count;i++){
        struct endpoint *e=by_id(p->ids[i]);
#ifndef PORT_TEST_NO_DETACH
        release_wrapper(e);e->generation++;
#endif
        p->generations[i]=e->generation;
    }
    p->committed=1;return 1;
}
static void finalize(JSRuntime *rt,JSValue v)
{(void)rt;free(JS_GetOpaque(v,port_class));}
static JSValue wrap(JSContext *ctx,struct endpoint *e)
{
    struct port_realm *r=realm(ctx);if(!r)return error(ctx,"DataCloneError","receiver has no port realm");
    JSValue o=JS_NewObjectClass(ctx,port_class);if(JS_IsException(o))return o;
    struct port_binding *b=malloc(sizeof *b);if(!b){JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}
    *b=(struct port_binding){e->id,e->generation};JS_SetOpaque(o,b);
    e->owner=r;e->object=JS_DupValue(ctx,o);e->handler=JS_UNDEFINED;e->started=0;return o;
}
JSValue js_ports_read(JSContext *ctx,struct js_port_packet *p,JSValue *ports)
{
    *ports=JS_UNDEFINED;
    if(!p||!p->committed)return error(ctx,"DataCloneError","message was not committed");
    JSValue data=JS_ReadObject(ctx,p->bytes,p->length,JS_READ_OBJ_REFERENCE);
    if(JS_IsException(data))return data;
    JSValue list=JS_NewArray(ctx);if(JS_IsException(list)){JS_FreeValue(ctx,data);return list;}
    for(int i=0;i<p->count;i++){
        struct endpoint *e=by_id(p->ids[i]);
        if(!e||e->owner||e->generation!=p->generations[i]){error(ctx,"DataCloneError","in-transit port no longer available");goto fail;}
        JSValue o=wrap(ctx,e);if(JS_IsException(o))goto fail;
        if(JS_DefinePropertyValueUint32(ctx,list,(uint32_t)i,o,JS_PROP_ENUMERABLE)<0)goto fail;
    }
    /* MessageEvent.ports is a FrozenArray. Build it without calling mutable
     * author globals such as Object.freeze during cross-realm deserialization. */
    if(JS_DefinePropertyValueStr(ctx,list,"length",JS_NewInt32(ctx,p->count),0)<0||
       JS_PreventExtensions(ctx,list)<0)goto fail;
    p->count=0;*ports=list;return data;
fail:
    for(int i=0;i<p->count;i++){
        struct endpoint *e=by_id(p->ids[i]);
        if(e&&e->generation==p->generations[i])close_endpoint(e);
    }
    p->count=0;JS_FreeValue(ctx,list);JS_FreeValue(ctx,data);return JS_EXCEPTION;
}
static JSValue channel(JSContext *ctx,JSValueConst nt,int argc,JSValueConst *argv)
{
    (void)argc;(void)argv;
    JSValue proto=JS_GetPropertyStr(ctx,nt,"prototype");if(JS_IsException(proto))return proto;
    JSValue o=JS_IsObject(proto)?JS_NewObjectProto(ctx,proto):JS_NewObject(ctx);JS_FreeValue(ctx,proto);
    if(JS_IsException(o))return o;
    struct endpoint *a=NULL,*b=NULL;
    for(int i=0;i<PORT_MAX;i++)if(!endpoints[i].id){if(!a)a=&endpoints[i];else{b=&endpoints[i];break;}}
    if(!b){JS_FreeValue(ctx,o);return error(ctx,"QuotaExceededError","MessageChannel endpoint limit");}
    memset(a,0,sizeof *a);memset(b,0,sizeof *b);a->id=next_id++;b->id=next_id++;
    a->generation=b->generation=1;a->peer=b->id;b->peer=a->id;
    JSValue p1=wrap(ctx,a),p2=wrap(ctx,b);
    if(JS_IsException(p1)||JS_IsException(p2)||JS_IsException(o)){
        JS_FreeValue(ctx,p1);JS_FreeValue(ctx,p2);JS_FreeValue(ctx,o);close_endpoint(a);close_endpoint(b);return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx,o,"port1",p1,JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,o,"port2",p2,JS_PROP_ENUMERABLE);return o;
}
static JSValue operation(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int op)
{
    if(!JS_GetOpaque(self,port_class))return JS_ThrowTypeError(ctx,"Illegal MessagePort receiver");
    struct endpoint *e=owned(ctx,self);
    if(!e)return JS_UNDEFINED; /* a detached/closed wrapper cannot control a new owner */
    if(op==1){e->started=1;return JS_UNDEFINED;}
    if(op==2){close_endpoint(e);return JS_UNDEFINED;}
    if(argc<1)return JS_ThrowTypeError(ctx,"postMessage requires data");
    JSValue xfer=argc>1?JS_DupValue(ctx,argv[1]):JS_UNDEFINED;
    if(JS_IsObject(xfer)&&JS_IsArray(ctx,xfer)==0){JSValue opt=xfer;xfer=JS_GetPropertyStr(ctx,opt,"transfer");JS_FreeValue(ctx,opt);}
    if(JS_IsException(xfer))return xfer;
    struct js_port_packet *p=js_ports_prepare(ctx,argv[0],xfer);JS_FreeValue(ctx,xfer);
    if(!p)return JS_EXCEPTION;
    e=owned(ctx,self);if(!e){js_ports_discard(p);return JS_UNDEFINED;}
    for(int i=0;i<p->count;i++)if(p->ids[i]==e->id||p->ids[i]==e->peer){
        js_ports_discard(p);return error(ctx,"DataCloneError","cannot transfer either endpoint through its own channel");
    }
    struct endpoint *dest=by_id(e->peer);
    if(dest&&dest->nqueue==PORT_QUEUE){js_ports_discard(p);return error(ctx,"QuotaExceededError","port queue limit");}
    if(!js_ports_commit(ctx,p)){js_ports_discard(p);return JS_EXCEPTION;}
    if(!dest){js_ports_discard(p);return JS_UNDEFINED;}
    p->seq=next_sequence++;if(dest->tail)dest->tail->next=p;else dest->head=p;
    dest->tail=p;dest->nqueue++;return JS_UNDEFINED;
}
static JSValue get_handler(JSContext *ctx,JSValueConst self)
{struct endpoint *e=owned(ctx,self);return e?JS_DupValue(ctx,e->handler):JS_NULL;}
static JSValue set_handler(JSContext *ctx,JSValueConst self,JSValueConst v)
{
    struct endpoint *e=owned(ctx,self);if(!e)return JS_UNDEFINED;
    JS_FreeValue(ctx,e->handler);e->handler=JS_IsFunction(ctx,v)?JS_DupValue(ctx,v):JS_NULL;
    if(JS_IsFunction(ctx,v))e->started=1;return JS_UNDEFINED;
}
static JSValue listener(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int remove)
{
    struct endpoint *e=owned(ctx,self);if(!e)return JS_UNDEFINED;
    if(argc<2)return JS_ThrowTypeError(ctx,"listener requires type and callback");
    const char *s=JS_ToCString(ctx,argv[0]);if(!s)return JS_EXCEPTION;
    int message=!strcmp(s,"message");JS_FreeCString(ctx,s);if(!message||JS_IsNull(argv[1])||JS_IsUndefined(argv[1]))return JS_UNDEFINED;
    int once=0;
    if(!remove&&argc>2&&JS_IsObject(argv[2])){JSValue v=JS_GetPropertyStr(ctx,argv[2],"once");if(JS_IsException(v))return v;once=JS_ToBool(ctx,v);JS_FreeValue(ctx,v);}
    e=owned(ctx,self);if(!e)return JS_UNDEFINED;
    if(!JS_IsObject(argv[1]))return JS_UNDEFINED;
    for(int i=0;i<e->nlistener;i++)if(JS_VALUE_GET_PTR(e->listeners[i].fn)==JS_VALUE_GET_PTR(argv[1])){
        if(remove){JS_FreeValue(ctx,e->listeners[i].fn);memmove(e->listeners+i,e->listeners+i+1,(size_t)(--e->nlistener-i)*sizeof *e->listeners);}return JS_UNDEFINED;
    }
    if(remove)return JS_UNDEFINED;
    if(e->nlistener==PORT_LISTENERS)return error(ctx,"QuotaExceededError","port listener limit");
    e->listeners[e->nlistener++]=(struct port_listener){JS_DupValue(ctx,argv[1]),++e->listener_id,once};return JS_UNDEFINED;
}
static void report(JSContext *ctx)
{JSValue ex=JS_GetException(ctx);const char *s=JS_ToCString(ctx,ex);fprintf(stderr,"[port] callback failed: %s\n",s?s:"exception");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,ex);}
int js_ports_pending(JSContext *ctx)
{for(int i=0;i<PORT_MAX;i++){struct endpoint *e=&endpoints[i];if(e->id&&e->owner&&e->owner->ctx==ctx&&e->started&&e->head)return 1;}return 0;}
int js_ports_pump(JSContext *ctx)
{
    struct port_realm *r=realm(ctx);if(!r)return 0;uint64_t limit=next_sequence-1;int ran=0;
    /* Bounded snapshot. New messages and owner changes never recursively
     * dispatch; the caller returns through its normal input/paint boundary. */
    for(int k=0;k<PORT_MAX&&ran<8;k++){
        struct endpoint *e=&endpoints[r->cursor++%PORT_MAX];
        if(!e->id||e->owner!=r||!e->started||!e->head||e->head->seq>limit)continue;
        struct js_port_packet *p=e->head;e->head=p->next;if(!e->head)e->tail=NULL;e->nqueue--;ran++;
        uint64_t id=e->id,generation=e->generation;JSValue object=JS_DupValue(ctx,e->object),ports;
        JSValue data=js_ports_read(ctx,p,&ports);js_ports_discard(p);
        if(JS_IsException(data)){report(ctx);JS_FreeValue(ctx,ports);JS_FreeValue(ctx,object);continue;}
        JSValue init=JS_NewObject(ctx);JS_SetPropertyStr(ctx,init,"data",data);JS_SetPropertyStr(ctx,init,"ports",ports);
        JSValue args[2]={JS_NewString(ctx,"message"),init};
        JSValue ev=JS_CallConstructor(ctx,r->event,2,(JSValueConst *)args);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,init);
        if(JS_IsException(ev)){report(ctx);JS_FreeValue(ctx,object);continue;}
        JS_DefinePropertyValueStr(ctx,ev,"isTrusted",JS_TRUE,JS_PROP_ENUMERABLE);
        JS_DefinePropertyValueStr(ctx,ev,"target",JS_DupValue(ctx,object),JS_PROP_ENUMERABLE);
        JS_DefinePropertyValueStr(ctx,ev,"currentTarget",JS_DupValue(ctx,object),JS_PROP_CONFIGURABLE);
        e=by_id(id);if(e&&e->generation==generation&&e->owner==r){
            struct port_listener snap[PORT_LISTENERS+1];int n=0;
            if(JS_IsFunction(ctx,e->handler))snap[n++]=(struct port_listener){JS_DupValue(ctx,e->handler),0,0};
            for(int j=0;j<e->nlistener;j++){snap[n]=e->listeners[j];snap[n++].fn=JS_DupValue(ctx,e->listeners[j].fn);}
            for(int j=0;j<n;j++){
                e=by_id(id);int call=e&&e->generation==generation&&e->owner==r;
                if(call&&snap[j].id){call=0;for(int z=0;z<e->nlistener;z++)if(e->listeners[z].id==snap[j].id){
                    call=1;if(snap[j].once){JS_FreeValue(ctx,e->listeners[z].fn);memmove(e->listeners+z,e->listeners+z+1,(size_t)(--e->nlistener-z)*sizeof *e->listeners);}break;}}
                if(call){JSValue fn=JS_IsFunction(ctx,snap[j].fn)?JS_DupValue(ctx,snap[j].fn):JS_GetPropertyStr(ctx,snap[j].fn,"handleEvent");
                    JSValue v=JS_IsException(fn)?JS_EXCEPTION:JS_Call(ctx,fn,JS_IsFunction(ctx,snap[j].fn)?object:snap[j].fn,1,(JSValueConst *)&ev);
                    if(JS_IsException(v))report(ctx);JS_FreeValue(ctx,v);JS_FreeValue(ctx,fn);}
                JS_FreeValue(ctx,snap[j].fn);
            }
        }
        JS_DefinePropertyValueStr(ctx,ev,"currentTarget",JS_NULL,JS_PROP_CONFIGURABLE);
        JS_FreeValue(ctx,ev);JS_FreeValue(ctx,object);
    }return ran;
}
static JSValue illegal(JSContext *ctx,JSValueConst nt,int argc,JSValueConst *argv)
{(void)nt;(void)argc;(void)argv;return JS_ThrowTypeError(ctx,"Illegal MessagePort constructor");}
static const JSCFunctionListEntry functions[]={
    JS_CFUNC_MAGIC_DEF("postMessage",1,operation,0),JS_CFUNC_MAGIC_DEF("start",0,operation,1),
    JS_CFUNC_MAGIC_DEF("close",0,operation,2),JS_CGETSET_DEF("onmessage",get_handler,set_handler),
    JS_CFUNC_MAGIC_DEF("addEventListener",2,listener,0),JS_CFUNC_MAGIC_DEF("removeEventListener",2,listener,1)
};
int js_ports_install(JSContext *ctx)
{
    if(realm(ctx))return 1;
    if(!port_class)JS_NewClassID(&port_class);
    JSRuntime *rt=JS_GetRuntime(ctx);JSClassDef def={"MessagePort",.finalizer=finalize};
    if(!JS_IsRegisteredClass(rt,port_class)&&JS_NewClass(rt,port_class,&def)<0)return 0;
    struct port_realm *r=calloc(1,sizeof *r);if(!r)return 0;r->ctx=ctx;
    JSValue g=JS_GetGlobalObject(ctx),event=JS_GetPropertyStr(ctx,g,"MessageEvent");
    if(!JS_IsFunction(ctx,event)){
        JS_FreeValue(ctx,event);
        static const char fallback[]="(function(){function MessageEvent(type,init){init=init||{};Object.defineProperties(this,{type:{value:String(type)},data:{value:init.data},ports:{value:init.ports||[]},origin:{value:''},source:{value:null},isTrusted:{value:false,configurable:true}})}return MessageEvent})()";
        event=JS_Eval(ctx,fallback,sizeof fallback-1,"<worker MessageEvent>",JS_EVAL_TYPE_GLOBAL);
        if(JS_IsException(event)){JS_FreeValue(ctx,g);free(r);return 0;}
        JS_SetPropertyStr(ctx,g,"MessageEvent",JS_DupValue(ctx,event));
    }
    r->event=event;r->next=realms;realms=r;
    JSValue proto=JS_NewObject(ctx);JS_SetPropertyFunctionList(ctx,proto,functions,sizeof functions/sizeof *functions);
    JSValue ctor=JS_NewCFunction2(ctx,illegal,"MessagePort",0,JS_CFUNC_constructor,0);
    JS_SetConstructor(ctx,ctor,proto);JS_SetClassProto(ctx,port_class,proto);
    JS_SetPropertyStr(ctx,g,"MessagePort",ctor);
    JSValue channel_proto=JS_NewObject(ctx);
    JSValue channel_ctor=JS_NewCFunction2(ctx,channel,"MessageChannel",0,JS_CFUNC_constructor,0);
    JS_SetConstructor(ctx,channel_ctor,channel_proto);JS_FreeValue(ctx,channel_proto);
    JS_SetPropertyStr(ctx,g,"MessageChannel",channel_ctor);
    JS_FreeValue(ctx,g);return 1;
}
void js_ports_close(JSContext *ctx)
{
    struct port_realm **pp=&realms;while(*pp&&(*pp)->ctx!=ctx)pp=&(*pp)->next;
    if(!*pp)return;struct port_realm *r=*pp;
    for(int i=0;i<PORT_MAX;i++)if(endpoints[i].id&&endpoints[i].owner==r)close_endpoint(&endpoints[i]);
    *pp=r->next;JS_FreeValue(ctx,r->event);free(r);
}
