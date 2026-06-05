#include "aqs.h"

/* All heap objects are chained so aqs_free_objects() can release them after a run
 * (AquaScript MVP has no GC; a script's objects live until the program exits). */
static Obj *g_objs = NULL;

static Obj *alloc_obj(size_t size, ObjType type)
{
    Obj *o = (Obj *)malloc(size);
    o->type = type;
    o->next = g_objs;
    g_objs = o;
    return o;
}

static uint32_t hash_str(const char *s, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

ObjStr *aqs_str_take(char *chars, int len)   /* takes ownership of `chars` */
{
    ObjStr *s = (ObjStr *)alloc_obj(sizeof(ObjStr), O_STR);
    s->len = len; s->chars = chars; s->hash = hash_str(chars, len);
    return s;
}

ObjStr *aqs_str_copy(const char *chars, int len)
{
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, chars, len); buf[len] = 0;
    return aqs_str_take(buf, len);
}

ObjFn *aqs_fn_new(void)
{
    ObjFn *fn = (ObjFn *)alloc_obj(sizeof(ObjFn), O_FN);
    fn->arity = 0; fn->name = NULL;
    fn->code = NULL; fn->count = fn->cap = 0;
    fn->consts = NULL; fn->kcount = fn->kcap = 0;
    return fn;
}

ObjNative *aqs_native_new(NativeFn fn, const char *name)
{
    ObjNative *n = (ObjNative *)alloc_obj(sizeof(ObjNative), O_NATIVE);
    n->fn = fn; n->name = name;
    return n;
}

ObjList *aqs_list_new(void)
{
    ObjList *l = (ObjList *)alloc_obj(sizeof(ObjList), O_LIST);
    l->items = NULL; l->count = l->cap = 0;
    return l;
}

void aqs_list_push(ObjList *l, Value v)
{
    if (l->count + 1 > l->cap) {
        l->cap = l->cap < 8 ? 8 : l->cap * 2;
        l->items = (Value *)realloc(l->items, l->cap * sizeof(Value));
    }
    l->items[l->count++] = v;
}

ObjPtr *aqs_ptr_new(uint64_t addr, int width, int is_signed)
{
    ObjPtr *p = (ObjPtr *)alloc_obj(sizeof(ObjPtr), O_PTR);
    p->addr = addr; p->width = width; p->is_signed = is_signed;
    return p;
}

void aqs_chunk_write(ObjFn *fn, uint8_t b)
{
    if (fn->count + 1 > fn->cap) {
        fn->cap = fn->cap < 8 ? 8 : fn->cap * 2;
        fn->code = (uint8_t *)realloc(fn->code, fn->cap);
    }
    fn->code[fn->count++] = b;
}

int aqs_chunk_const(ObjFn *fn, Value v)
{
    /* dedupe simple constants so the pool stays small */
    for (int i = 0; i < fn->kcount; i++)
        if (aqs_value_eq(fn->consts[i], v)) return i;
    if (fn->kcount + 1 > fn->kcap) {
        fn->kcap = fn->kcap < 8 ? 8 : fn->kcap * 2;
        fn->consts = (Value *)realloc(fn->consts, fn->kcap * sizeof(Value));
    }
    fn->consts[fn->kcount] = v;
    return fn->kcount++;
}

void aqs_free_objects(void)
{
    Obj *o = g_objs;
    while (o) {
        Obj *next = o->next;
        switch (o->type) {
        case O_STR:  free(((ObjStr *)o)->chars); break;
        case O_FN:   free(((ObjFn *)o)->code); free(((ObjFn *)o)->consts); break;
        case O_LIST: free(((ObjList *)o)->items); break;
        default: break;
        }
        free(o);
        o = next;
    }
    g_objs = NULL;
}
