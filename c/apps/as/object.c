#include "as.h"

/* OOM wrappers. On a NULL allocation they stamp as_err + raise g_oom (cleared in
 * reset_stack / as_compile_module). The VM dispatch loop polls g_oom and unwinds
 * to a catchable "out of memory"; compile-/lex-time callers poll it directly.
 * Allocation sites that grow-then-store guard the store with `if (g_oom) return`
 * so no NULL is dereferenced before the unwind. */
int g_oom = 0;
void *as_malloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { snprintf(as_err, sizeof as_err, "out of memory"); g_oom = 1; }
    return p;
}
void *as_realloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) { snprintf(as_err, sizeof as_err, "out of memory"); g_oom = 1; }
    return q;
}

/* Every heap object is registered in one contiguous array. Sweeping walks it
 * linearly and compacts in place, which is a dense pass over cache lines the
 * prefetcher can see coming -- the intrusive `next` chain this replaced was a
 * dependent load per object, in allocation order, i.e. scattered. It also gets
 * the pointer out of the object header (see struct Obj). */
static Obj  **g_objs = NULL;
static long   live_objects = 0;    /* entries in g_objs (for gc_stats + as_gc_live) */
static long   objs_cap = 0;
static int    gc_disabled = 0;     /* >0 disables collection (compile / load only, see below) */

/* Two triggers, whichever trips first. Counting objects alone -- what this used
 * to do -- meant a heap of a thousand 1 MB strings and a heap of a thousand
 * empty lists collected at exactly the same point, so the case that actually
 * needs collecting on a 24 MiB static arena was the one the trigger ignored.
 * Counting bytes alone has the opposite blind spot: a million tiny objects are
 * cheap in bytes but expensive in registry space and sweep time. Neither
 * subsumes the other, so both are watched.
 *
 * gc_account() is called from the object allocator and from every site that
 * grows or frees an object-owned buffer, so `heap_bytes` tracks what a collection
 * could actually reclaim. (as_malloc/as_realloc are NOT the choke point: the
 * lexer's token buffer and the string builders in vm.c go through them for
 * memory the GC never owns.) */
static size_t heap_bytes = 0;
static size_t next_gc_bytes = 1u << 20;      /* first collection after 1 MiB ... */
static long   next_gc_objs = 1024;           /* ... or 1024 objects, whichever comes first */
#define GC_MIN_HEAP (1u << 20)
#define GC_MIN_OBJS 1024

static void gc_account(long delta)
{
    if (delta < 0) {
        size_t d = (size_t)(-delta);
        heap_bytes = heap_bytes > d ? heap_bytes - d : 0;
    } else {
        heap_bytes += (size_t)delta;
    }
}

/* Temporary GC roots: objects that exist but are not reachable from the VM yet.
 * The alternative -- and what this replaces at every runtime site -- was to turn
 * the collector OFF for the duration, which means the heap grows without bound
 * for as long as the operation runs. `"...".split()` on a large string did
 * exactly that: one disable spanning thousands of allocations. Protecting the
 * few in-flight objects instead keeps the collector running throughout.
 * Compile and load still disable outright; see gc_collect. */
#define GC_TEMPS_MAX 16
static Obj *temps[GC_TEMPS_MAX];
static int  ntemps = 0;
static int  temps_overflow = 0;   /* protects that did not fit; each holds a gc_disable */

void as_gc_protect(Obj *o)
{
    if (!o) { temps_overflow++; gc_disabled++; return; }   /* still a slot to release */
    if (ntemps < GC_TEMPS_MAX) { temps[ntemps++] = o; return; }
    temps_overflow++; gc_disabled++;      /* never drop a root; fall back to the blunt instrument */
}
/* Protect/release nest strictly, so anything that overflowed is by construction
 * more recent than anything on the array -- release those first or the pairing
 * would come apart. */
void as_gc_release(int n)
{
    while (n-- > 0) {
        if (temps_overflow > 0) { temps_overflow--; if (gc_disabled > 0) gc_disabled--; }
        else if (ntemps > 0) ntemps--;
    }
}

static Obj  **gray = NULL;          /* GC mark worklist (raw realloc'd buffer, NOT a GC object) */
static int    gray_count = 0, gray_cap = 0;

/* --- type descriptors -----------------------------------------------------
 * Everything the runtime needs to know about an ObjType, in one row. It used to
 * be spread over parallel switch statements (allocate / trace / free / print /
 * str), so adding a type meant finding all of them and a miss was silent: an
 * untraced type is a use-after-free that only shows up under GC stress, an
 * unfinalized one is a steady leak. Now a new type is one row, and the compiler
 * flags a missing case in the one remaining switch (the formatter in value.c).
 *
 * `trace` marks the objects this one references (NULL = leaf); `finalize` frees
 * the sub-allocations this object owns, never the objects it points at -- those
 * are separate entries on the g_objs chain that sweep handles individually. */
typedef struct {
    const char *name;               /* for diagnostics (as_obj_type_name) */
    size_t      size;               /* allocation size; alloc_obj takes only the type */
    void      (*trace)(Obj *);
    void      (*finalize)(Obj *);
} ObjInfo;

static void tr_upvalue(Obj *o) { gc_mark_value(((ObjUpvalue *)o)->closed); }
static void tr_fn(Obj *o)
{
    ObjFn *fn = (ObjFn *)o;
    if (fn->name) gc_mark_obj((Obj *)fn->name);
    for (int i = 0; i < fn->kcount; i++) gc_mark_value(fn->consts[i]);
    /* fn->module IS traced. It used to be skipped, on the premise that modules
     * are permanently rooted via modules[] -- but that premise is false in one
     * real case: a FAILED import drops its partial module from the table on
     * purpose (so it cannot poison later imports), while frames belonging to it
     * are still unwinding. The module then has no root, and anything reading
     * fn->module afterwards -- the traceback, or global_slot's
     * &fn->module->vars[i] -- touches freed memory. ASan caught this the moment
     * the traceback started reading it under GC stress.
     * A function's globals live in its module, so needing the module alive for
     * as long as the function is alive is simply correct. */
    gc_mark_obj((Obj *)fn->module);
}
static void tr_closure(Obj *o)
{
    ObjClosure *c = (ObjClosure *)o;
    gc_mark_obj((Obj *)c->fn);
    for (int i = 0; i < c->upvalue_count; i++) gc_mark_obj((Obj *)c->upvalues[i]);
}
static void tr_list(Obj *o)
{
    ObjList *l = (ObjList *)o;
    for (int i = 0; i < l->count; i++) gc_mark_value(l->items[i]);
}
static void tr_dict(Obj *o)
{
    ObjDict *d = (ObjDict *)o;
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind == AS_DK_STR) gc_mark_obj((Obj *)e->kstr);
        if (e->kind == AS_DK_STR || e->kind == AS_DK_INT) gc_mark_value(e->val);
    }
}
static void tr_module(Obj *o)
{
    ObjModule *m = (ObjModule *)o;
    gc_mark_obj((Obj *)m->name);
    for (int i = 0; i < m->count; i++) { gc_mark_obj((Obj *)m->vars[i].name); gc_mark_value(m->vars[i].val); }
}
static void tr_class(Obj *o)
{
    ObjClass *k = (ObjClass *)o;
    gc_mark_obj((Obj *)k->name);
    gc_mark_obj((Obj *)k->super);     /* gc_mark_obj is NULL-safe */
    gc_mark_obj((Obj *)k->methods);
}
static void tr_instance(Obj *o)
{
    ObjInstance *in = (ObjInstance *)o;
    gc_mark_obj((Obj *)in->klass);
    gc_mark_obj((Obj *)in->shape);
    /* Only the slots the shape names: anything past nslots is spare capacity
     * holding whatever was there before, and tracing it would resurrect it. */
    if (in->shape) for (int i = 0; i < in->shape->nslots; i++) gc_mark_value(in->slots[i]);
}
static void tr_buf(Obj *o)
{
    /* The bytes hold machine words, never Values -- only the shape is an object.
     * (A pointer field holds a raw address the GC knows nothing about; that is
     * the same bargain addr() already makes.) */
    gc_mark_obj((Obj *)((ObjBuf *)o)->shape);
}
static void tr_shape(Obj *o)
{
    Shape *s = (Shape *)o;
    gc_mark_obj((Obj *)s->sname);
    for (int i = 0; i < s->nslots; i++) gc_mark_obj((Obj *)s->names[i]);
    /* Edges keep child shapes alive. A shape tree is per field-name-sequence and
     * shared by every instance built that way, so it is small and long-lived by
     * design -- holding the children is what makes a repeated `init` land on the
     * same shape instead of building a new one each time. */
    for (int i = 0; i < s->nedges; i++) {
        gc_mark_obj((Obj *)s->edges[i].name);
        gc_mark_obj((Obj *)s->edges[i].to);
    }
}
static void tr_bound(Obj *o)
{
    ObjBoundMethod *bm = (ObjBoundMethod *)o;
    gc_mark_value(bm->receiver);
    gc_mark_obj((Obj *)bm->method);
}

/* Each finalizer gives back exactly what its allocation site charged, so
 * heap_bytes stays honest across a collection. gcache is deliberately not
 * accounted: it is a runtime memo the GC cannot reclaim by collecting. */
static void fin_str(Obj *o)
{
    ObjStr *s = (ObjStr *)o;
    gc_account(-(long)s->len - 1);
    free(s->chars);
}
static void fin_fn(Obj *o)
{
    ObjFn *f = (ObjFn *)o;
    gc_account(-(long)f->cap - (long)((size_t)f->kcap * sizeof(Value)));
    free(f->code); free(f->consts); free(f->gcache); free(f->pcache);
}
static void fin_list(Obj *o)
{
    ObjList *l = (ObjList *)o;
    gc_account(-(long)((size_t)l->cap * sizeof(Value)));
    free(l->items);
}
static void fin_dict(Obj *o)
{
    ObjDict *d = (ObjDict *)o;
    gc_account(-(long)((size_t)d->cap * sizeof(DictEntry)));
    free(d->entries);
}
static void fin_module(Obj *o)
{
    ObjModule *m = (ObjModule *)o;
    gc_account(-(long)((size_t)m->cap * sizeof(NameVal)));
    free(m->vars);
}
static void fin_instance(Obj *o)
{
    ObjInstance *in = (ObjInstance *)o;
    gc_account(-(long)((size_t)in->slotcap * sizeof(Value)));
    free(in->slots);
}
static void fin_shape(Obj *o)
{
    Shape *s = (Shape *)o;
    gc_account(-(long)((size_t)s->nslots * sizeof(ObjStr *))
               - (long)((size_t)s->edgecap * sizeof(struct ShapeEdge))
               - (long)(s->specs ? (size_t)s->nslots * sizeof(SlotSpec) : 0));
    free(s->names); free(s->edges); free(s->specs);
}
static void fin_buf(Obj *o)
{
    ObjBuf *b = (ObjBuf *)o;
    gc_account(-(long)b->nbytes);
    free(b->raw);
}
static void fin_closure(Obj *o)
{
    ObjClosure *c = (ObjClosure *)o;
    gc_account(-(long)((size_t)c->upvalue_count * sizeof(ObjUpvalue *)));
    free(c->upvalues);
}

/* Designated initializers on purpose: the row is bound to its ObjType by name,
 * so reordering the enum can't silently shift the table. */
static const ObjInfo OBJ_INFO[] = {
    [O_STR]          = { "str",          sizeof(ObjStr),         NULL,        fin_str     },
    [O_FN]           = { "fn",           sizeof(ObjFn),          tr_fn,       fin_fn      },
    [O_NATIVE]       = { "native fn",    sizeof(ObjNative),      NULL,        NULL        },
    [O_LIST]         = { "list",         sizeof(ObjList),        tr_list,     fin_list    },
    [O_PTR]          = { "ptr",          sizeof(ObjPtr),         NULL,        NULL        },
    [O_MODULE]       = { "module",       sizeof(ObjModule),      tr_module,   fin_module  },
    [O_DICT]         = { "dict",         sizeof(ObjDict),        tr_dict,     fin_dict    },
    [O_CLOSURE]      = { "fn",           sizeof(ObjClosure),     tr_closure,  fin_closure },
    [O_UPVALUE]      = { "upvalue",      sizeof(ObjUpvalue),     tr_upvalue,  NULL        },
    [O_CLASS]        = { "class",        sizeof(ObjClass),       tr_class,    NULL        },
    [O_INSTANCE]     = { "instance",     sizeof(ObjInstance),    tr_instance, fin_instance },
    [O_BOUND_METHOD] = { "bound method", sizeof(ObjBoundMethod), tr_bound,    NULL        },
    [O_RANGE]        = { "range",        sizeof(ObjRange),       NULL,        NULL        },
    [O_SHAPE]        = { "shape",        sizeof(Shape),          tr_shape,    fin_shape   },
    [O_BUF]          = { "buffer",       sizeof(ObjBuf),         tr_buf,      fin_buf     },
};
/* A type added to the enum without a row here would read a zeroed descriptor:
 * size 0 (a 0-byte allocation) and no trace (a use-after-free under GC). With
 * designated initializers the array only reaches O__COUNT entries once the last
 * type has a row, so this needs no editing when a type is added -- it just fires.
 * (It already has: O_RANGE was caught by it.) */
_Static_assert(sizeof OBJ_INFO / sizeof OBJ_INFO[0] == O__COUNT,
               "OBJ_INFO is missing a row for some ObjType");

const char *as_obj_type_name(Obj *o) { return OBJ_INFO[o->type].name; }

static void free_object(Obj *o);   /* fwd: free one object + its owned sub-allocations */

static Obj *alloc_obj(ObjType type)
{
    size_t size = OBJ_INFO[type].size;
    if (!gc_disabled) {            /* collect BEFORE the new object exists, so it can't be swept */
#ifdef AS_GC_STRESS
        gc_collect();
#else
        if (heap_bytes >= next_gc_bytes || live_objects >= next_gc_objs) gc_collect();
#endif
    }
    /* Grow the registry BEFORE allocating: a live object that failed to register
     * would be invisible to both the sweep and the teardown, i.e. leaked with no
     * way to ever find it again. */
    if (live_objects + 1 > objs_cap) {
        long nc = objs_cap < 256 ? 256 : objs_cap * 2;
        Obj **nv = (Obj **)realloc(g_objs, (size_t)nc * sizeof(Obj *));
        if (!nv) { snprintf(as_err, sizeof as_err, "out of memory"); g_oom = 1; return NULL; }
        g_objs = nv; objs_cap = nc;
    }
    Obj *o = (Obj *)as_malloc(size);
    if (!o) return NULL;          /* g_oom set; caller propagates, DISPATCH unwinds */
    o->type = (uint8_t)type;
    o->marked = 0;
    g_objs[live_objects++] = o;
    gc_account((long)size);
    return o;
}

static uint32_t hash_str(const char *s, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

ObjStr *as_str_take(char *chars, int len)   /* takes ownership of `chars` */
{
    ObjStr *s = (ObjStr *)alloc_obj(O_STR);
    if (!s) { free(chars); return NULL; }     /* OOM: release the buffer we took */
    s->len = len; s->chars = chars; s->hash = 0; s->hashed = 0;   /* M23: hash lazily */
    gc_account((long)len + 1);          /* as_str_take owns `chars` from here on */
    return s;
}

uint32_t as_str_hash(ObjStr *s)
{
    if (!s->hashed) { s->hash = hash_str(s->chars, s->len); s->hashed = 1; }
    return s->hash;
}

ObjStr *as_str_copy(const char *chars, int len)
{
    char *buf = (char *)as_malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, chars, len); buf[len] = 0;
    return as_str_take(buf, len);
}

ObjFn *as_fn_new(void)
{
    ObjFn *fn = (ObjFn *)alloc_obj(O_FN);
    if (!fn) return NULL;             /* g_oom set; caller propagates */
    fn->arity = 0; fn->name = NULL;
    fn->code = NULL; fn->count = fn->cap = 0;
    fn->consts = NULL; fn->kcount = fn->kcap = 0;
    fn->module = NULL;
    fn->upvalue_count = 0;
    fn->gcache = NULL; fn->gcache_n = 0; fn->gcache_gen = 0;
    fn->pcache = NULL; fn->pcache_n = 0;
    return fn;
}

ObjClosure *as_closure_new(ObjFn *fn)
{
    ObjUpvalue **ups = NULL;
    if (fn->upvalue_count > 0) {
        ups = (ObjUpvalue **)as_malloc(sizeof(ObjUpvalue *) * (size_t)fn->upvalue_count);
        if (!ups) return NULL;
        for (int i = 0; i < fn->upvalue_count; i++) ups[i] = NULL;
    }
    ObjClosure *c = (ObjClosure *)alloc_obj(O_CLOSURE);
    if (!c) { free(ups); return NULL; }     /* g_oom set; ups was ours */
    c->fn = fn; c->upvalues = ups; c->upvalue_count = fn->upvalue_count;
    gc_account((long)((size_t)c->upvalue_count * sizeof(ObjUpvalue *)));
    return c;
}
ObjUpvalue *as_upvalue_new(Value *slot)
{
    ObjUpvalue *u = (ObjUpvalue *)alloc_obj(O_UPVALUE);
    if (!u) return NULL;
    u->location = slot; u->closed = NIL_VAL; u->next = NULL;
    return u;
}

/* --- shapes ---------------------------------------------------------------
 * Every instance starts on the empty shape and walks one edge per field its
 * init assigns. Because the edges are memoised on the shape they leave, every
 * instance built by the same init arrives at the same Shape object -- which is
 * what a call site can cache against. */
static Shape   *g_shape_root = NULL;
static uint32_t shape_next_id = 1;      /* 0 is reserved: it means "nothing cached" */

static Shape *shape_new(void)
{
    Shape *s = (Shape *)alloc_obj(O_SHAPE);
    if (!s) return NULL;
    s->id = shape_next_id++;
    s->nslots = 0; s->names = NULL;
    s->edges = NULL; s->nedges = s->edgecap = 0;
    s->specs = NULL; s->sname = NULL; s->bytes = 0;
    return s;
}
static Shape *shape_root(void)
{
    if (!g_shape_root) g_shape_root = shape_new();
    return g_shape_root;
}

/* `from` plus one field named `name`, reusing the edge when it already exists. */
static Shape *shape_transition(Shape *from, ObjStr *name)
{
    for (int i = 0; i < from->nedges; i++) {
        ObjStr *k = from->edges[i].name;
        if (k == name || (k->len == name->len && as_str_hash(k) == as_str_hash(name)
                          && memcmp(k->chars, name->chars, (size_t)k->len) == 0))
            return from->edges[i].to;
    }
    /* shape_new is the only call below that can collect. `from` is the shape of
     * the instance the caller protected, and `name` is in the running function's
     * constant pool, so both survive it -- everything after is plain malloc. */
    Shape *to = shape_new();
    if (!to) return NULL;

    to->nslots = from->nslots + 1;
    to->names = (ObjStr **)as_malloc((size_t)to->nslots * sizeof(ObjStr *));
    if (!to->names) { to->nslots = 0; return NULL; }        /* g_oom set; `to` stays traceable */
    gc_account((long)((size_t)to->nslots * sizeof(ObjStr *)));
    /* The root shape has nslots 0 and names NULL, and memcpy(dst, NULL, 0) is
     * undefined even though it copies nothing -- UBSan flags it. */
    if (from->nslots) memcpy(to->names, from->names, (size_t)from->nslots * sizeof(ObjStr *));
    to->names[from->nslots] = name;

    if (from->nedges + 1 > from->edgecap) {
        int nc = from->edgecap < 4 ? 4 : from->edgecap * 2;
        struct ShapeEdge *ne = (struct ShapeEdge *)as_realloc(from->edges, (size_t)nc * sizeof *ne);
        /* Failing to memoise costs cache hits, not correctness: the next instance
         * builds an equivalent shape with a different id and simply misses. */
        if (!ne) return to;
        gc_account((long)((size_t)(nc - from->edgecap) * sizeof(struct ShapeEdge)));
        from->edges = ne; from->edgecap = nc;
    }
    from->edges[from->nedges].name = name;
    from->edges[from->nedges].to   = to;
    from->nedges++;
    return to;
}

/* --- machine-typed shapes + buffers ---------------------------------------
 * A layout shape is built complete and never transitions: a kernel struct does
 * not grow fields at runtime. Everything is copied in, so the caller's arrays
 * (which live in the VM's stack or a list) are not captured. */
Shape *as_layout_shape_new(ObjStr *sname, int bytes, ObjStr **names, SlotSpec *specs, int n)
{
    Shape *s = shape_new();              /* the only call here that can collect */
    if (!s) return NULL;
    s->bytes = bytes;
    s->sname = sname;
    if (n > 0) {
        s->names = (ObjStr **)as_malloc((size_t)n * sizeof(ObjStr *));
        if (!s->names) return NULL;
        s->specs = (SlotSpec *)as_malloc((size_t)n * sizeof(SlotSpec));
        if (!s->specs) return NULL;      /* names is freed by fin_shape via nslots=0 */
        memcpy(s->names, names, (size_t)n * sizeof(ObjStr *));
        memcpy(s->specs, specs, (size_t)n * sizeof(SlotSpec));
        s->nslots = n;                   /* only now are both arrays traceable */
        gc_account((long)((size_t)n * (sizeof(ObjStr *) + sizeof(SlotSpec))));
    }
    return s;
}

ObjBuf *as_buf_new(Shape *shape, int nbytes)
{
    /* Protect the shape: alloc_obj can collect, and a freshly built layout shape
     * may not be reachable from anything yet. */
    as_gc_protect((Obj *)shape);
    ObjBuf *b = (ObjBuf *)alloc_obj(O_BUF);
    as_gc_release(1);
    if (!b) return NULL;
    b->shape = shape; b->raw = NULL; b->nbytes = 0;
    if (nbytes > 0) {
        b->raw = (uint8_t *)as_malloc((size_t)nbytes);
        if (!b->raw) return NULL;                 /* g_oom set; nbytes stays 0 */
        memset(b->raw, 0, (size_t)nbytes);        /* a kernel struct starts zeroed */
        b->nbytes = nbytes;
        gc_account((long)nbytes);
    }
    return b;
}

/* Read `width` bytes at `off`. Little-endian on both the host and the target, so
 * the load is a memcpy rather than a byte loop. */
static uint64_t buf_load(const uint8_t *p, int width)
{
    uint64_t u = 0;
    memcpy(&u, p, (size_t)width);
    return u;
}

int as_buf_get(ObjBuf *b, const SlotSpec *sp, Value *out)
{
    if (sp->off + sp->width > b->nbytes) return 0;    /* a layout wider than its buffer */
    if (sp->kind == SK_SPAN) {
        ObjStr *s = as_str_copy((const char *)(b->raw + sp->off), (int)sp->width);
        if (!s) return 0;
        *out = OBJ_VAL(s);                            /* fixed length, NOT NUL-trimmed */
        return 1;
    }
    uint64_t u = buf_load(b->raw + sp->off, sp->width);
    if (sp->kind == SK_I && sp->width < 8) {          /* sign-extend from width bytes */
        int bits = sp->width * 8;
        int64_t m = (int64_t)1 << (bits - 1);
        *out = INT_VAL(((int64_t)u ^ m) - m);
    } else {
        *out = INT_VAL((int64_t)u);                   /* SK_U/SK_PTR: raw bits */
    }
    return 1;
}

int as_buf_set(ObjBuf *b, const SlotSpec *sp, Value v)
{
    if (sp->off + sp->width > b->nbytes) return 0;
    if (sp->kind == SK_SPAN) {
        if (!IS_STR(v)) return 0;
        ObjStr *s = AS_STR(v);
        if (s->len > (int)sp->width) return 0;        /* refuse to truncate silently */
        memcpy(b->raw + sp->off, s->chars, (size_t)s->len);
        memset(b->raw + sp->off + s->len, 0, (size_t)sp->width - (size_t)s->len);
        return 1;
    }
    if (!IS_INT(v)) return 0;
    uint64_t u = (uint64_t)AS_INT(v);
    memcpy(b->raw + sp->off, &u, (size_t)sp->width);  /* LE: the low bytes are first */
    return 1;
}

ObjClass *as_class_new(ObjStr *name)
{
    ObjClass *c = (ObjClass *)alloc_obj(O_CLASS);
    if (!c) return NULL;
    c->name = name; c->super = NULL;
    c->methods = NULL;                    /* set before the dict alloc: tr_class reads it */
    as_gc_protect((Obj *)c);              /* c is unreachable while as_dict_new allocates */
    c->methods = as_dict_new();
    as_gc_release(1);
    return c;
}
ObjInstance *as_instance_new(ObjClass *klass)
{
    ObjInstance *in = (ObjInstance *)alloc_obj(O_INSTANCE);
    if (!in) return NULL;
    in->klass = klass;
    in->shape = NULL;                     /* tr_instance reads it during the alloc below */
    in->slots = NULL; in->slotcap = 0;
    as_gc_protect((Obj *)in);             /* in is unreachable while the root shape allocates */
    in->shape = shape_root();
    as_gc_release(1);
    return in;
}

int as_shape_find(Shape *s, ObjStr *name)
{
    for (int i = 0; i < s->nslots; i++) {
        ObjStr *k = s->names[i];
        if (k == name) return i;                      /* same constant-pool string: common */
        if (k->len == name->len && as_str_hash(k) == as_str_hash(name)
            && memcmp(k->chars, name->chars, (size_t)k->len) == 0) return i;
    }
    return -1;
}

int as_instance_get(ObjInstance *in, ObjStr *name, Value *out)
{
    int i = as_shape_find(in->shape, name);
    if (i < 0) return 0;
    *out = in->slots[i];
    return 1;
}

int as_instance_set(ObjInstance *in, ObjStr *name, Value val)
{
    int i = as_shape_find(in->shape, name);
    if (i >= 0) { in->slots[i] = val; return 1; }

    /* A new field: move to the shape that has it appended. Protect the instance
     * -- shape_transition allocates, and nothing else roots `in` if this runs
     * from a native. */
    as_gc_protect((Obj *)in);
    Shape *next = shape_transition(in->shape, name);
    as_gc_release(1);
    if (!next) return 0;                              /* g_oom set */
    if (next->nslots > in->slotcap) {
        int nc = in->slotcap < 4 ? 4 : in->slotcap * 2;
        if (nc < next->nslots) nc = next->nslots;
        Value *ns = (Value *)as_realloc(in->slots, (size_t)nc * sizeof(Value));
        if (!ns) return 0;                            /* g_oom set; shape unchanged */
        gc_account((long)((size_t)(nc - in->slotcap) * sizeof(Value)));
        for (int k = in->slotcap; k < nc; k++) ns[k] = NIL_VAL;   /* tr_instance never sees junk */
        in->slots = ns; in->slotcap = nc;
    }
    in->slots[next->nslots - 1] = val;
    in->shape = next;                                 /* only now is the new slot in range */
    return 1;
}
ObjBoundMethod *as_bound_method_new(Value receiver, ObjClosure *method)
{
    ObjBoundMethod *bm = (ObjBoundMethod *)alloc_obj(O_BOUND_METHOD);
    if (!bm) return NULL;
    bm->receiver = receiver; bm->method = method;
    return bm;
}

ObjModule *as_module_new(const char *name, int len)
{
    ObjModule *m = (ObjModule *)alloc_obj(O_MODULE);
    if (!m) return NULL;
    m->name = NULL;                         /* tr_module reads it during the as_str_copy below */
    m->vars = NULL; m->count = m->cap = 0; m->state = 0;
    as_gc_protect((Obj *)m);                /* m is unreachable until the caller registers it */
    m->name = as_str_copy(name, len);
    as_gc_release(1);
    if (!m->name) return NULL;              /* g_oom set; a NULL name would crash module_find */
    return m;
}

/* Bumped whenever a new global name is created (in any module or in builtins).
 * That is the only event that can move where an already-resolved name points, so
 * it is what invalidates every ObjFn's gcache. Starts at 1 so a zeroed
 * gcache_gen never looks current. */
uint32_t as_globals_gen = 1;

int as_module_index(ObjModule *m, ObjStr *name)
{
    for (int i = 0; i < m->count; i++)
        if (as_str_hash(m->vars[i].name) == as_str_hash(name) && m->vars[i].name->len == name->len
            && memcmp(m->vars[i].name->chars, name->chars, name->len) == 0) return i;
    return -1;
}

Value *as_module_slot(ObjModule *m, ObjStr *name, int create)
{
    int at = as_module_index(m, name);
    if (at >= 0) return &m->vars[at].val;
    if (!create) return NULL;
    if (m->count + 1 > m->cap) {
        int nc = m->cap < 8 ? 8 : m->cap * 2;
        NameVal *nv = (NameVal *)as_realloc(m->vars, (size_t)nc * sizeof(NameVal));
        if (!nv) return NULL;                /* g_oom set; old vars + count intact */
        gc_account((long)((size_t)(nc - m->cap) * sizeof(NameVal)));
        m->vars = nv; m->cap = nc;
    }
    m->vars[m->count].name = name;
    m->vars[m->count].val = NIL_VAL;
    as_globals_gen++;                        /* a new name can shadow a builtin */
    return &m->vars[m->count++].val;
}

ObjNative *as_native_new(NativeFn fn, const char *name)
{
    ObjNative *n = (ObjNative *)alloc_obj(O_NATIVE);
    if (!n) return NULL;
    n->fn = fn; n->name = name;
    return n;
}

ObjList *as_list_new(void)
{
    ObjList *l = (ObjList *)alloc_obj(O_LIST);
    if (!l) return NULL;
    l->items = NULL; l->count = l->cap = 0;
    return l;
}

void as_list_push(ObjList *l, Value v)
{
    if (l->count + 1 > l->cap) {
        int nc = l->cap < 8 ? 8 : l->cap * 2;
        Value *ni = (Value *)as_realloc(l->items, (size_t)nc * sizeof(Value));
        if (!ni) return;                     /* g_oom set; old items + count intact */
        gc_account((long)((size_t)(nc - l->cap) * sizeof(Value)));
        l->items = ni; l->cap = nc;
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
static uint32_t key_hash(Value k) { return IS_STR(k) ? as_str_hash(AS_STR(k)) : hash_int(AS_INT(k)); }
static int key_match(DictEntry *e, Value k)
{
    if (IS_STR(k)) return e->kind == AS_DK_STR && as_str_hash(e->kstr) == as_str_hash(AS_STR(k))
                          && e->kstr->len == AS_STR(k)->len
                          && memcmp(e->kstr->chars, AS_STR(k)->chars, (size_t)e->kstr->len) == 0;
    return e->kind == AS_DK_INT && e->kint == AS_INT(k);
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
        if (e->kind == AS_DK_EMPTY) return tomb ? tomb : e;
        if (e->kind == AS_DK_TOMB) { if (!tomb) tomb = e; }
        else if (key_match(e, k)) return e;
        i = (i + 1) & mask;
    }
}
ObjDict *as_dict_new(void)
{
    ObjDict *d = (ObjDict *)alloc_obj(O_DICT);
    if (!d) return NULL;
    d->entries = NULL; d->live = d->used = d->cap = 0;
    return d;
}
static void dict_grow(ObjDict *d)
{
    int newcap = d->cap < 8 ? 8 : d->cap * 2;
    DictEntry *ne = (DictEntry *)as_malloc((size_t)newcap * sizeof(DictEntry));
    if (!ne) return;                                          /* g_oom set; old table untouched */
    memset(ne, 0, (size_t)newcap * sizeof(DictEntry));        /* AS_DK_EMPTY == 0 */
    int live = 0;
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind != AS_DK_STR && e->kind != AS_DK_INT) continue;   /* drop tombstones */
        Value k = e->kind == AS_DK_STR ? OBJ_VAL(e->kstr) : INT_VAL(e->kint);
        *find_entry(ne, newcap, k) = *e;
        live++;
    }
    free(d->entries);
    gc_account((long)((size_t)(newcap - d->cap) * sizeof(DictEntry)));
    d->entries = ne; d->cap = newcap; d->used = live; d->live = live;
}
int as_dict_set(ObjDict *d, Value key, Value val)
{
    if (!IS_STR(key) && !IS_INT(key)) return 0;
    /* grow at 0.75 load; cap is a power of two so cap>>2 == cap/4 exactly, and the
     * subtraction form can't overflow int the way (used+1)*4 >= cap*3 would. cap 0 -> grow to 8. */
    if (d->used + 1 >= d->cap - (d->cap >> 2)) { dict_grow(d); if (g_oom) return 0; }
    DictEntry *e = find_entry(d->entries, d->cap, key);
    int existing = (e->kind == AS_DK_STR || e->kind == AS_DK_INT);
    if (!existing) {
        if (e->kind == AS_DK_EMPTY) d->used++;               /* tombstone reuse doesn't add to load */
        d->live++;
        if (IS_STR(key)) { e->kind = AS_DK_STR; e->kstr = AS_STR(key); }
        else             { e->kind = AS_DK_INT; e->kint = AS_INT(key); }
    }
    e->val = val;
    return 1;
}
int as_dict_get(ObjDict *d, Value key, Value *out)
{
    if (d->cap == 0 || (!IS_STR(key) && !IS_INT(key))) return 0;
    DictEntry *e = find_entry(d->entries, d->cap, key);
    if (e->kind != AS_DK_STR && e->kind != AS_DK_INT) return 0;
    *out = e->val; return 1;
}
int as_dict_has(ObjDict *d, Value key) { Value tmp; return as_dict_get(d, key, &tmp); }
int as_dict_remove(ObjDict *d, Value key)
{
    if (d->cap == 0 || (!IS_STR(key) && !IS_INT(key))) return 0;
    DictEntry *e = find_entry(d->entries, d->cap, key);
    if (e->kind != AS_DK_STR && e->kind != AS_DK_INT) return 0;
    e->kind = AS_DK_TOMB; d->live--;                         /* used unchanged: slot still probed-through */
    return 1;
}
ObjList *as_dict_keys(ObjDict *d)
{
    ObjList *l = as_list_new();
    if (!l) return NULL;
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind == AS_DK_STR)      as_list_push(l, OBJ_VAL(e->kstr));
        else if (e->kind == AS_DK_INT) as_list_push(l, INT_VAL(e->kint));
    }
    return l;
}
ObjList *as_dict_values(ObjDict *d)
{
    ObjList *l = as_list_new();
    if (!l) return NULL;
    for (int i = 0; i < d->cap; i++) {
        DictEntry *e = &d->entries[i];
        if (e->kind == AS_DK_STR || e->kind == AS_DK_INT) as_list_push(l, e->val);
    }
    return l;
}

ObjPtr *as_ptr_new(uint64_t addr, int width, int is_signed)
{
    ObjPtr *p = (ObjPtr *)alloc_obj(O_PTR);
    if (!p) return NULL;
    p->addr = addr; p->width = width; p->is_signed = is_signed;
    return p;
}

ObjRange *as_range_new(int64_t start, int64_t step, int64_t count)
{
    ObjRange *r = (ObjRange *)alloc_obj(O_RANGE);
    if (!r) return NULL;
    r->start = start; r->step = step; r->count = count;
    return r;
}

void as_chunk_write(ObjFn *fn, uint8_t b)
{
    if (fn->count + 1 > fn->cap) {
        int nc = fn->cap < 8 ? 8 : fn->cap * 2;
        uint8_t *nb = (uint8_t *)as_realloc(fn->code, (size_t)nc);
        if (nb) gc_account((long)(nc - fn->cap));
        if (!nb) return;                     /* g_oom set; compiler polls g_oom -> aborts the compile */
        fn->code = nb; fn->cap = nc;
    }
    fn->code[fn->count++] = b;
}

int as_chunk_const(ObjFn *fn, Value v)
{
    /* dedupe simple constants so the pool stays small */
    for (int i = 0; i < fn->kcount; i++)
        if (as_value_eq(fn->consts[i], v)) return i;
    if (fn->kcount + 1 > fn->kcap) {
        int nk = fn->kcap < 8 ? 8 : fn->kcap * 2;
        Value *nv = (Value *)as_realloc(fn->consts, (size_t)nk * sizeof(Value));
        if (nv) gc_account((long)((size_t)(nk - fn->kcap) * sizeof(Value)));
        if (!nv) return fn->kcount;          /* g_oom set; compiler polls g_oom -> aborts the compile */
        fn->consts = nv; fn->kcap = nk;
    }
    fn->consts[fn->kcount] = v;
    return fn->kcount++;
}

void gc_mark_obj(Obj *o)
{
    if (o == NULL || o->marked) return;
    o->marked = 1;
    if (gray_count + 1 > gray_cap) {
        int nc = gray_cap < 16 ? 16 : gray_cap * 2;
        Obj **ng = (Obj **)as_realloc(gray, (size_t)nc * sizeof(Obj *));
        /* OOM during mark: keep o marked so sweep keeps it conservatively LIVE (a
         * leak is recoverable next GC; freeing a live object would be a UAF), keep
         * g_oom set, and return without enqueuing. Collection finishes with a
         * possibly-incomplete worklist but NO freed-live objects + NO NULL deref;
         * control returns up to alloc_obj -> opcode -> DISPATCH, which unwinds on
         * g_oom before the VM can touch the untraced subgraph. */
        if (!ng) return;                    /* o stays marked; g_oom already set by as_realloc */
        gray = ng; gray_cap = nc;
    }
    gray[gray_count++] = o;
}
void gc_mark_value(Value v) { if (IS_OBJ(v)) gc_mark_obj(AS_OBJ(v)); }

/* Mark everything an already-gray object references (OBJ_INFO[].trace). */
static void blacken(Obj *o)
{
    void (*trace)(Obj *) = OBJ_INFO[o->type].trace;
    if (trace) trace(o);
}

void gc_collect(void)
{
    /* Still an outright disable, but now only for compilation and .la loading:
     * those build a tree of ObjFns that is unreachable from the VM until it is
     * returned, and no temporary-root stack can express "the half-built function
     * the compiler is currently inside". Both are bounded to a single compile of
     * a single module, unlike the runtime disables this replaced. */
    if (gc_disabled) return;
    gray_count = 0;
    as_vm_mark_roots();                                  /* mark + gray the roots (vm.c) */
    for (int i = 0; i < ntemps; i++) gc_mark_obj(temps[i]);   /* allocated, not yet reachable */
    gc_mark_obj((Obj *)g_shape_root);      /* the shape tree hangs off it via edges */
    while (gray_count > 0) blacken(gray[--gray_count]);   /* trace to fixpoint */
    /* OOM during mark (gray worklist grow failed -> g_oom): the trace is incomplete,
     * so sweeping now could free reachable-but-untraced objects (the UAF the mark
     * side conservatively avoids). Leak this cycle instead: clear every mark and
     * bail; the next collection retries from scratch. alloc_obj's as_malloc will
     * report the OOM and DISPATCH unwinds. */
    if (g_oom) {
        for (long i = 0; i < live_objects; i++) g_objs[i]->marked = 0;
        return;
    }
    long w = 0;                                           /* sweep: compact in place */
    for (long i = 0; i < live_objects; i++) {
        Obj *o = g_objs[i];
        if (o->marked) { o->marked = 0; g_objs[w++] = o; }
        else free_object(o);
    }
    live_objects = w;
    next_gc_bytes = heap_bytes * 2;
    if (next_gc_bytes < GC_MIN_HEAP) next_gc_bytes = GC_MIN_HEAP;
    next_gc_objs = live_objects * 2;
    if (next_gc_objs < GC_MIN_OBJS) next_gc_objs = GC_MIN_OBJS;
}

/* Free one object's owned sub-allocations + the object itself. Used by both the
 * GC sweep and the end-of-run teardown. Does NOT touch other objects it references
 * (those are separate entries in the registry). The caller owns the registry
 * bookkeeping -- the sweep compacts, so it cannot also be decrementing a count. */
static void free_object(Obj *o)
{
    void (*finalize)(Obj *) = OBJ_INFO[o->type].finalize;
    if (finalize) finalize(o);        /* accounts its own buffers */
    gc_account(-(long)OBJ_INFO[o->type].size);
    free(o);
}

void as_free_objects(void)
{
    for (long i = 0; i < live_objects; i++) free_object(g_objs[i]);
    free(g_objs); g_objs = NULL; objs_cap = 0;
    free(gray); gray = NULL; gray_count = gray_cap = 0;
    live_objects = 0;          /* defensive: fresh state for the next run */
    heap_bytes = 0;
    next_gc_bytes = GC_MIN_HEAP;
    next_gc_objs = GC_MIN_OBJS;
    gc_disabled = 0;
    ntemps = 0; temps_overflow = 0;
    g_shape_root = NULL;       /* freed with everything else; must not dangle into the next run */
}

long as_gc_live(void) { return live_objects; }
void as_gc_push_disable(void) { gc_disabled++; }
void as_gc_pop_disable(void) { if (gc_disabled > 0) gc_disabled--; }
