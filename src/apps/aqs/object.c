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
    fn->module = NULL;
    return fn;
}

ObjModule *aqs_module_new(const char *name, int len)
{
    ObjModule *m = (ObjModule *)alloc_obj(sizeof(ObjModule), O_MODULE);
    m->name = aqs_str_copy(name, len);
    m->vars = NULL; m->count = m->cap = 0; m->state = 0;
    return m;
}

Value *aqs_module_slot(ObjModule *m, ObjStr *name, int create)
{
    for (int i = 0; i < m->count; i++)
        if (m->vars[i].name->hash == name->hash && m->vars[i].name->len == name->len
            && memcmp(m->vars[i].name->chars, name->chars, name->len) == 0) return &m->vars[i].val;
    if (!create) return NULL;
    if (m->count + 1 > m->cap) {
        m->cap = m->cap < 8 ? 8 : m->cap * 2;
        m->vars = (NameVal *)realloc(m->vars, m->cap * sizeof(NameVal));
    }
    m->vars[m->count].name = name;
    m->vars[m->count].val = NIL_VAL;
    return &m->vars[m->count++].val;
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

/* ---- dict: open-addressing hash table, string|int keys (M21) ---- */
static uint32_t hash_int(int64_t v)
{
    uint64_t x = (uint64_t)v;                 /* splitmix64 finalizer */
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x =  x ^ (x >> 31);
    return (uint32_t)x;
}
static uint32_t key_hash(Value k) { return IS_STR(k) ? AS_STR(k)->hash : hash_int(AS_INT(k)); }
static int key_match(DictEntry *e, Value k)
{
    if (IS_STR(k)) return e->kind == AQS_DK_STR && e->kstr->hash == AS_STR(k)->hash
                          && e->kstr->len == AS_STR(k)->len
                          && memcmp(e->kstr->chars, AS_STR(k)->chars, (size_t)e->kstr->len) == 0;
    return e->kind == AQS_DK_INT && e->kint == AS_INT(k);
}
/* Return the slot holding `k`, or the slot to insert into (first tombstone, else
 * the terminating empty slot). cap must be > 0 and a power of two. */
static DictEntry *find_entry(DictEntry *es, int cap, Value k)
{
    uint32_t mask = (uint32_t)(cap - 1);
    uint32_t i = key_hash(k) & mask;
    DictEntry *tomb = NULL;
    for (;;) {
        DictEntry *e = &es[i];
        if (e->kind == AQS_DK_EMPTY) return tomb ? tomb : e;
        if (e->kind == AQS_DK_TOMB) { if (!tomb) tomb = e; }
        else if (key_match(e, k)) return e;
        i = (i + 1) & mask;
    }
}
ObjDict *aqs_dict_new(void)
{
    ObjDict *d = (ObjDict *)alloc_obj(sizeof(ObjDict), O_DICT);
    d->entries = NULL; d->live = d->used = d->cap = 0;
    return d;
}
static void dict_grow(ObjDict *d)
{
    int newcap = d->cap < 8 ? 8 : d->cap * 2;
    DictEntry *ne = (DictEntry *)malloc((size_t)newcap * sizeof(DictEntry));
    memset(ne, 0, (size_t)newcap * sizeof(DictEntry));        /* AQS_DK_EMPTY == 0 */
    int live = 0;
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind != AQS_DK_STR && e->kind != AQS_DK_INT) continue;   /* drop tombstones */
        Value k = e->kind == AQS_DK_STR ? OBJ_VAL(e->kstr) : INT_VAL(e->kint);
        *find_entry(ne, newcap, k) = *e;
        live++;
    }
    free(d->entries);
    d->entries = ne; d->cap = newcap; d->used = live; d->live = live;
}
int aqs_dict_set(ObjDict *d, Value key, Value val)
{
    if (!IS_STR(key) && !IS_INT(key)) return 0;
    /* grow at 0.75 load; cap is a power of two so cap>>2 == cap/4 exactly, and the
     * subtraction form can't overflow int the way (used+1)*4 >= cap*3 would. cap 0 -> grow to 8. */
    if (d->used + 1 >= d->cap - (d->cap >> 2)) dict_grow(d);
    DictEntry *e = find_entry(d->entries, d->cap, key);
    int existing = (e->kind == AQS_DK_STR || e->kind == AQS_DK_INT);
    if (!existing) {
        if (e->kind == AQS_DK_EMPTY) d->used++;               /* tombstone reuse doesn't add to load */
        d->live++;
        if (IS_STR(key)) { e->kind = AQS_DK_STR; e->kstr = AS_STR(key); }
        else             { e->kind = AQS_DK_INT; e->kint = AS_INT(key); }
    }
    e->val = val;
    return 1;
}
int aqs_dict_get(ObjDict *d, Value key, Value *out)
{
    if (d->cap == 0 || (!IS_STR(key) && !IS_INT(key))) return 0;
    DictEntry *e = find_entry(d->entries, d->cap, key);
    if (e->kind != AQS_DK_STR && e->kind != AQS_DK_INT) return 0;
    *out = e->val; return 1;
}
int aqs_dict_has(ObjDict *d, Value key) { Value tmp; return aqs_dict_get(d, key, &tmp); }
int aqs_dict_remove(ObjDict *d, Value key)
{
    if (d->cap == 0 || (!IS_STR(key) && !IS_INT(key))) return 0;
    DictEntry *e = find_entry(d->entries, d->cap, key);
    if (e->kind != AQS_DK_STR && e->kind != AQS_DK_INT) return 0;
    e->kind = AQS_DK_TOMB; d->live--;                         /* used unchanged: slot still probed-through */
    return 1;
}
ObjList *aqs_dict_keys(ObjDict *d)
{
    ObjList *l = aqs_list_new();
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind == AQS_DK_STR)      aqs_list_push(l, OBJ_VAL(e->kstr));
        else if (e->kind == AQS_DK_INT) aqs_list_push(l, INT_VAL(e->kint));
    }
    return l;
}
ObjList *aqs_dict_values(ObjDict *d)
{
    ObjList *l = aqs_list_new();
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind == AQS_DK_STR || e->kind == AQS_DK_INT) aqs_list_push(l, e->val);
    }
    return l;
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
        case O_LIST:   free(((ObjList *)o)->items); break;
        case O_DICT:   free(((ObjDict *)o)->entries); break;
        case O_MODULE: free(((ObjModule *)o)->vars); break;
        default: break;
        }
        free(o);
        o = next;
    }
    g_objs = NULL;
}
