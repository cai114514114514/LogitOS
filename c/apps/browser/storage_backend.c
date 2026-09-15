/* Textually included ONCE by js_webapi.c, like browser_rt.c/http_cache.c.
 * Host gates historically link different js_*.c subsets. Requiring each to
 * remember another object silently creates a second platform; keeping this
 * implementation with its sole current consumer makes every link use it.
 * Its header remains independent so future consumers use the same service. */
#include "storage_backend.h"
#include <stdlib.h>
#include <string.h>

struct storage_item { char *key, *value; size_t key_len, value_len; };
struct storage_area {
    char *origin;
    int kind;
    unsigned long long session_id;
    unsigned int count;
    size_t bytes;
    struct storage_item items[STORAGE_ITEM_LIMIT];
};
static struct storage_area storage_areas[STORAGE_AREA_LIMIT];
#ifdef STORAGE_BACKEND_TEST
static int storage_alloc_left = -1;
void storage_backend_fail_alloc_after(int count) { storage_alloc_left = count; }
#endif
static char *storage_copy(const char *p, size_t n)
{
#ifdef STORAGE_BACKEND_TEST
    if (storage_alloc_left == 0) return NULL;
    if (storage_alloc_left > 0) storage_alloc_left--;
#endif
    if (n == (size_t)-1) return NULL;
    char *s = malloc(n + 1);
    if (s) { if (n) memcpy(s, p, n); s[n] = 0; }
    return s;
}
static unsigned long long storage_partition(const struct storage_key *k)
{
#ifdef STORAGE_NO_SESSION_PARTITION
    /* Negative control: restore the shipped origin-only session namespace. */
    (void)k; return 0;
#else
    return k->kind == STORAGE_SESSION ? k->session_id : 0;
#endif
}
static int storage_valid(const struct storage_key *k)
{ return k && k->origin && (k->kind == STORAGE_LOCAL || k->kind == STORAGE_SESSION); }
static struct storage_area *storage_lookup(const struct storage_key *k)
{
    if (!storage_valid(k)) return NULL;
    for (int i = 0; i < STORAGE_AREA_LIMIT; i++) {
        struct storage_area *a = &storage_areas[i];
        if (a->origin && a->kind == k->kind &&
            a->session_id == storage_partition(k) && !strcmp(a->origin, k->origin)) return a;
    }
    return NULL;
}
static int storage_index(struct storage_area *a, const char *key, size_t n)
{
    if (!a || (!key && n)) return -1;
    for (unsigned int i = 0; i < a->count; i++)
        if (a->items[i].key_len == n && (!n || !memcmp(a->items[i].key, key, n))) return (int)i;
    return -1;
}
static void storage_clear_area(struct storage_area *a)
{
    if (!a) return;
    for (unsigned int i = 0; i < a->count; i++) {
        free(a->items[i].key); free(a->items[i].value);
    }
    free(a->origin);
    memset(a, 0, sizeof *a);
}
#include "storage_persistence.inc"

const char *storage_backend_get(const struct storage_key *k, const char *key, size_t n, size_t *len)
{
    struct storage_area *a = storage_lookup(k);
    int i = storage_index(a, key, n);
    if (len) *len = i < 0 ? 0 : a->items[i].value_len;
    return i < 0 ? NULL : a->items[i].value;
}
const char *storage_backend_key(const struct storage_key *k, unsigned int index, size_t *len)
{
    struct storage_area *a = storage_lookup(k);
    if (len) *len = a && index < a->count ? a->items[index].key_len : 0;
    return a && index < a->count ? a->items[index].key : NULL;
}
unsigned int storage_backend_length(const struct storage_key *k)
{ struct storage_area *a = storage_lookup(k); return a ? a->count : 0; }
int storage_backend_set(const struct storage_key *k, const char *key, size_t kn, const char *value, size_t vn)
{
    if (!storage_valid(k) || (!key && kn) || (!value && vn)) return STORAGE_INVALID;
    if(storage_backend_status(k)!=STORAGE_OK)return storage_backend_status(k);
    struct storage_area *a = storage_lookup(k);
    int i = storage_index(a, key, kn);
    /* Subtract the old entry first, then compare without overflowing size_t.
     * Quota counts encoded key+value bytes, preserving the old 256 KiB policy. */
    size_t retained = a ? a->bytes : 0;
    if (i >= 0) retained -= a->items[i].key_len + a->items[i].value_len;
    if (kn > STORAGE_BYTE_LIMIT || vn > STORAGE_BYTE_LIMIT - kn ||
        retained > STORAGE_BYTE_LIMIT - kn - vn ||
        (i < 0 && a && a->count == STORAGE_ITEM_LIMIT)) return STORAGE_QUOTA;
    if (!a) {
        for (int j = 0; j < STORAGE_AREA_LIMIT; j++)
            if (!storage_areas[j].origin) { a = &storage_areas[j]; break; }
        if (!a) return STORAGE_QUOTA;
    }
    char *nv = storage_copy(value, vn);
    if (!nv) return STORAGE_NOMEM;
    char *nk = i < 0 ? storage_copy(key, kn) : NULL;
    if (i < 0 && !nk) { free(nv); return STORAGE_NOMEM; }
    char *origin = !a->origin ? storage_copy(k->origin, strlen(k->origin)) : NULL;
    if (!a->origin && !origin) { free(nv); free(nk); return STORAGE_NOMEM; }
    struct storage_area candidate=*a;
    if(origin){candidate.origin=origin;candidate.kind=k->kind;candidate.session_id=storage_partition(k);}
    int ci=i;
    if(ci<0){ci=(int)candidate.count++;candidate.items[ci].key=nk;candidate.items[ci].key_len=kn;}
    candidate.items[ci].value=nv;candidate.items[ci].value_len=vn;candidate.bytes=retained+kn+vn;
    int persisted=k->kind==STORAGE_LOCAL?storage_persist_candidate(a,&candidate):STORAGE_OK;
    if(persisted!=STORAGE_OK){free(nv);free(nk);free(origin);return persisted;}
    /* Commit only after ALL allocations succeed: replacing an existing value
     * must not lose it, and failure creating an origin must not consume a slot. */
    if (origin) { a->origin = origin; a->kind = k->kind; a->session_id = storage_partition(k); }
    if (i < 0) { i = (int)a->count++; a->items[i].key = nk; a->items[i].key_len = kn; }
    else free(a->items[i].value);
    a->items[i].value = nv; a->items[i].value_len = vn;
    a->bytes = retained + kn + vn;
    return STORAGE_OK;
}
int storage_backend_remove(const struct storage_key *k, const char *key, size_t n)
{
    if(!storage_valid(k) || (!key && n))return STORAGE_INVALID;
    if(storage_backend_status(k)!=STORAGE_OK)return storage_backend_status(k);
    struct storage_area *a=storage_lookup(k);int i=storage_index(a,key,n);
    if(i<0)return STORAGE_OK;
    struct storage_area candidate=*a;
    candidate.bytes-=a->items[i].key_len+a->items[i].value_len;
    for(unsigned j=(unsigned)i;j+1<candidate.count;j++)candidate.items[j]=candidate.items[j+1];
    memset(&candidate.items[--candidate.count],0,sizeof candidate.items[0]);
    int rc=k->kind==STORAGE_LOCAL?storage_persist_candidate(a,candidate.count?&candidate:NULL):STORAGE_OK;
    if(rc!=STORAGE_OK)return rc;
    free(a->items[i].key);free(a->items[i].value);*a=candidate;
    if(!a->count)storage_clear_area(a);return STORAGE_OK;
}
int storage_backend_clear(const struct storage_key *k)
{
    if(!storage_valid(k))return STORAGE_INVALID;
    if(storage_backend_status(k)!=STORAGE_OK)return storage_backend_status(k);
    struct storage_area *a=storage_lookup(k);if(!a)return STORAGE_OK;
    int rc=k->kind==STORAGE_LOCAL?storage_persist_candidate(a,NULL):STORAGE_OK;
    if(rc==STORAGE_OK)storage_clear_area(a);return rc;
}
void storage_backend_drop_session(unsigned long long id)
{
    for (int i = 0; i < STORAGE_AREA_LIMIT; i++)
        if (storage_areas[i].origin && storage_areas[i].kind == STORAGE_SESSION &&
            storage_areas[i].session_id == id) storage_clear_area(&storage_areas[i]);
}
