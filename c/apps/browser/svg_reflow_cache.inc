/* Inline SVG has no resource URL. Retain successful rasters by the EXACT
 * serialized decoder input, not a DOM address or original parser span. The
 * serializer is still run once per node/pass, so live attributes, descendant
 * edits and winning/inherited CSS remain authoritative. Hashes only shortlist;
 * length plus byte comparison proves a hit. No DOM/style pointer survives a
 * pass. Root opacity and outer layout size remain owned by their usual paths.
 *
 * Current-pass borrowers pin entries. A reflow releases its display items and
 * node aliases before starting a new generation, so only unborrowed entries
 * can be evicted. Navigation/context teardown release all retained ownership.
 * Retention is bounded by entries, exact key allocation bytes, and decoded
 * pixels. Decoder temporary allocations and the existing top-page uncached
 * fallback are not a claim of this retained-cache bound. */
#ifndef SVG_REFLOW_MAX
#define SVG_REFLOW_MAX 128
#endif
#ifndef SVG_REFLOW_KEYS
#define SVG_REFLOW_KEYS (2UL * 1024 * 1024)
#endif
#ifndef SVG_REFLOW_PASSIVE_KEYS
#define SVG_REFLOW_PASSIVE_KEYS (256UL * 1024)
#endif
struct svg_reflow_entry {
    char *key;
    int len;
    unsigned long long hash, generation, used;
    struct image *image;
    unsigned long bytes;
};
static struct svg_reflow_entry g_svg_reflow[SVG_REFLOW_MAX];
static int g_svg_reflow_n;
static unsigned long g_svg_reflow_bytes, g_svg_reflow_keys;
static unsigned long long g_svg_reflow_generation = 1, g_svg_reflow_used;

static int svg_reflow_owns(const struct image *image)
{
    if (!image) return 0;
    for (int i=0;i<g_svg_reflow_n;i++)
        if (g_svg_reflow[i].image==image) return 1;
    return 0;
}
static unsigned long svg_held_bytes(void)
{
    unsigned long bytes=g_svg_reflow_bytes;
    for (int i=0;i<g_svg_pass_n;i++) {
        const struct image *image=g_svg_pass[i].image;
        if (image && !svg_reflow_owns(image))
            bytes+=(unsigned long)image->w*(unsigned long)image->h*4;
    }
    return bytes;
}
static void svg_reflow_remove(int at)
{
    struct svg_reflow_entry *e=&g_svg_reflow[at];
    g_svg_reflow_bytes-=e->bytes;g_svg_reflow_keys-=(unsigned long)e->len;
    img_free(e->image);kfree(e->image);kfree(e->key);
    for (int i=at+1;i<g_svg_reflow_n;i++)g_svg_reflow[i-1]=g_svg_reflow[i];
    g_svg_reflow_n--;
}
static int svg_reflow_evict(void)
{
    int at=-1;
    for (int i=0;i<g_svg_reflow_n;i++) {
        const struct svg_reflow_entry *e=&g_svg_reflow[i];
        if (e->generation==g_svg_reflow_generation)continue;
        if (at<0 || e->used<g_svg_reflow[at].used)at=i;
    }
    if (at<0)return 0;
    svg_reflow_remove(at);return 1;
}
static int svg_reflow_room(unsigned long incoming)
{
    unsigned long limit=SVG_PASS_BYTES;
    if (g_layout_passive)
        limit=g_decoded_limit>g_ic_bytes?(unsigned long)(g_decoded_limit-g_ic_bytes):0;
    if (incoming>limit)return 0;
    while (svg_held_bytes()>limit-incoming)
        if (!svg_reflow_evict())return 0;
    return 1;
}
static void svg_reflow_new_pass(void)
{
    /* Only called after all prior display items and aliases were released. */
    if (++g_svg_reflow_generation==0) {
        g_svg_reflow_generation=1;
        for (int i=0;i<g_svg_reflow_n;i++)g_svg_reflow[i].generation=0;
    }
}
static void svg_reflow_reset(void)
{
    while (g_svg_reflow_n)svg_reflow_remove(g_svg_reflow_n-1);
    g_svg_reflow_generation=1;g_svg_reflow_used=0;
}
static unsigned long long svg_reflow_hash(const char *s,int len)
{
    unsigned long long h=1469598103934665603ULL;
    for (int i=0;i<len;i++){h^=(unsigned char)s[i];h*=1099511628211ULL;}
    return h;
}
static struct image *svg_reflow_find(const char *raw,int len)
{
#if defined(LAYOUT_SVG_NO_REFLOW_CACHE) || defined(LAYOUT_SVG_REPEAT_DECODE)
    (void)raw;(void)len;return 0;
#else
    if (!raw || len<=0)return 0;
    unsigned long long hash=svg_reflow_hash(raw,len);
    for (int i=0;i<g_svg_reflow_n;i++) {
        struct svg_reflow_entry *e=&g_svg_reflow[i];
        if (e->hash==hash && e->len==len && !memcmp(e->key,raw,(unsigned long)len)) {
            e->generation=g_svg_reflow_generation;e->used=++g_svg_reflow_used;
            return e->image;
        }
    }
    return 0;
#endif
}
static void svg_reflow_keep(const char *raw,int len,struct image *image)
{
#if defined(LAYOUT_SVG_NO_REFLOW_CACHE) || defined(LAYOUT_SVG_REPEAT_DECODE)
    (void)raw;(void)len;(void)image;
#else
    if (!raw || len<=0 || !image)return;
    unsigned long key_limit=g_layout_passive?SVG_REFLOW_PASSIVE_KEYS:SVG_REFLOW_KEYS;
    unsigned long bytes=(unsigned long)image->w*(unsigned long)image->h*4;
    if ((unsigned long)len>key_limit || !svg_reflow_room(bytes))return;
    while (g_svg_reflow_n==SVG_REFLOW_MAX || g_svg_reflow_keys>key_limit-(unsigned long)len)
        if (!svg_reflow_evict())return;
    char *key=kmalloc((unsigned long)len);
    if (!key)return;
    memcpy(key,raw,(unsigned long)len);
    struct svg_reflow_entry *e=&g_svg_reflow[g_svg_reflow_n++];
    *e=(struct svg_reflow_entry){key,len,svg_reflow_hash(raw,len),
        g_svg_reflow_generation,++g_svg_reflow_used,image,bytes};
    g_svg_reflow_bytes+=bytes;g_svg_reflow_keys+=(unsigned long)len;
#endif
}
