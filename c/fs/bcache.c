#include <stdint.h>
#include <stddef.h>
#include "bcache.h"
#include "blkdev.h"
#include "kheap.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

#define SPB   (BC_BS / BLK_SECTOR)      /* 512-byte sectors per cache block */
#define NIL   (-1)
#define NBUCK 256                       /* power of two; blk & (NBUCK-1) */

struct bufhdr {
    uint32_t blk;
    int      valid;
    int      dirty;
    int      lru_prev, lru_next;        /* MRU head .. LRU tail */
    int      hnext;                     /* hash chain */
};

static struct bufhdr *hdr;
static uint8_t      (*data)[BC_BS];
static int           nbuf;
static int           bucket[NBUCK];
static int           lru_head = NIL, lru_tail = NIL;
static uint32_t      dev_blocks;
static struct bcache_stats st;

/* --- intrusive LRU list ---------------------------------------------------- */

static void lru_unlink(int i)
{
    if (hdr[i].lru_prev != NIL) hdr[hdr[i].lru_prev].lru_next = hdr[i].lru_next;
    else                        lru_head = hdr[i].lru_next;
    if (hdr[i].lru_next != NIL) hdr[hdr[i].lru_next].lru_prev = hdr[i].lru_prev;
    else                        lru_tail = hdr[i].lru_prev;
    hdr[i].lru_prev = hdr[i].lru_next = NIL;
}

static void lru_push_front(int i)
{
    hdr[i].lru_prev = NIL;
    hdr[i].lru_next = lru_head;
    if (lru_head != NIL) hdr[lru_head].lru_prev = i;
    lru_head = i;
    if (lru_tail == NIL) lru_tail = i;
}

static void touch(int i) { lru_unlink(i); lru_push_front(i); }

/* --- hash ------------------------------------------------------------------ */

static void hash_insert(int i)
{
    int b = (int)(hdr[i].blk & (NBUCK - 1));
    hdr[i].hnext = bucket[b];
    bucket[b] = i;
}

static void hash_remove(int i)
{
    int b = (int)(hdr[i].blk & (NBUCK - 1));
    int *p = &bucket[b];
    while (*p != NIL) {
        if (*p == i) { *p = hdr[i].hnext; hdr[i].hnext = NIL; return; }
        p = &hdr[*p].hnext;
    }
}

static int lookup(uint32_t blk)
{
    for (int i = bucket[blk & (NBUCK - 1)]; i != NIL; i = hdr[i].hnext)
        if (hdr[i].valid && hdr[i].blk == blk) return i;
    return NIL;
}

/* --- writeback ------------------------------------------------------------- */

/* Push one buffer at the device. No barrier: see the header comment -- a
 * writeback may land at any time, and the ordering is the barriers' job. */
static int flush_one(int i)
{
    if (!hdr[i].valid || !hdr[i].dirty) return 0;
    if (blk_write(hdr[i].blk * SPB, SPB, data[i])) return -1;
    hdr[i].dirty = 0;
    st.writebacks++;
    return 0;
}

/* Detach a buffer from the index and park it at the LRU tail as a free slot. */
static void release(int i)
{
    if (hdr[i].valid) hash_remove(i);
    hdr[i].valid = 0;
    hdr[i].dirty = 0;
    lru_unlink(i);
    hdr[i].lru_prev = lru_tail;
    hdr[i].lru_next = NIL;
    if (lru_tail != NIL) hdr[lru_tail].lru_next = i;
    lru_tail = i;
    if (lru_head == NIL) lru_head = i;
}

/* Take a buffer for `blk`: the LRU tail, written back first if it is dirty.
 * Invalid slots sit at the tail end, so they are reused before anything live.
 * Returns NIL only if the victim was dirty and could not be written -- losing
 * it silently would be data loss, so the caller falls back to the device. */
static int claim(uint32_t blk)
{
    int i = lru_tail;
    if (i == NIL) return NIL;
    if (hdr[i].valid) {
        if (hdr[i].dirty) {
            if (flush_one(i)) return NIL;
            st.dirty_evictions++;
        }
        hash_remove(i);
        st.evictions++;
    }
    hdr[i].blk   = blk;
    hdr[i].valid = 1;
    hdr[i].dirty = 0;
    hash_insert(i);
    touch(i);
    return i;
}

/* --- public ---------------------------------------------------------------- */

int bcache_init(uint32_t total_blocks, int n)
{
    if (hdr || data) return -1;                 /* already up */
    if (n <= 0) n = BC_NBUF;
    hdr  = kmalloc((size_t)n * sizeof *hdr);
    data = kmalloc((size_t)n * BC_BS);
    if (!hdr || !data) { kfree(hdr); kfree(data); hdr = 0; data = 0; return -1; }
    nbuf       = n;
    dev_blocks = total_blocks;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < NBUCK; i++) bucket[i] = NIL;
    lru_head = lru_tail = NIL;
    for (int i = 0; i < n; i++) {
        hdr[i].blk = 0; hdr[i].valid = 0; hdr[i].dirty = 0; hdr[i].hnext = NIL;
        hdr[i].lru_prev = hdr[i].lru_next = NIL;
        lru_push_front(i);                       /* all free, all at the tail end */
    }
    return 0;
}

void bcache_shutdown(void)
{
    if (hdr) { for (int i = 0; i < nbuf; i++) (void)flush_one(i); }
    kfree(hdr); kfree(data);
    hdr = 0; data = 0; nbuf = 0;
    lru_head = lru_tail = NIL;
    for (int i = 0; i < NBUCK; i++) bucket[i] = NIL;
}

int bcache_read(uint32_t blk, void *buf)
{
    if (!hdr || blk >= dev_blocks) return -1;
    int i = lookup(blk);
    if (i != NIL) { st.hits++; touch(i); memcpy(buf, data[i], BC_BS); return 0; }
    st.misses++;
    i = claim(blk);
    if (i == NIL) {                                       /* no buffer: bypass */
        st.dev_reads++; st.dev_blocks++;
        return blk_read(blk * SPB, SPB, buf);
    }
    st.dev_reads++; st.dev_blocks++;
    if (blk_read(blk * SPB, SPB, data[i])) {
        release(i);      /* never leave a buffer claiming a block it never read */
        return -1;
    }
    memcpy(buf, data[i], BC_BS);
    return 0;
}

int bcache_read_run(uint32_t blk, uint32_t n, void *buf)
{
    if (n == 0) return -1;
    if (!hdr) {                                  /* no cache up: straight through */
        st.dev_reads++; st.dev_blocks += n;
        return blk_read_n((uint64_t)blk * SPB, n * SPB, buf);
    }
    if (blk >= dev_blocks || n > dev_blocks - blk) return -1;

    /* A small request is a working-set read (a directory, a 12 KB app, an
     * indirect chain) and belongs in the pool; a big one is a stream and would
     * evict the working set to hold data nobody reads twice. nbuf/4 is the line:
     * a request that could take a quarter of the pool is already too big to be
     * a working set. */
    int install = (n <= (uint32_t)(nbuf / 4));
    uint8_t *out = (uint8_t *)buf;

    for (uint32_t k = 0; k < n; ) {
        int i = lookup(blk + k);
        if (i != NIL) {                          /* resident: the cache is authoritative */
            st.hits++; touch(i);
            memcpy(out + (size_t)k * BC_BS, data[i], BC_BS);
            k++;
            continue;
        }
        /* Grow the miss as far as it stays contiguous AND non-resident: a
         * resident block in the middle must be served from its buffer (it may
         * be dirty), so it ends the run rather than being read over. */
        uint32_t run = 1;
        while (k + run < n && lookup(blk + k + run) == NIL) run++;

        st.misses += run;
        st.dev_reads++;
        st.dev_blocks += run;
        if (!install) st.bypassed += run;
        if (blk_read_n((uint64_t)(blk + k) * SPB, run * SPB, out + (size_t)k * BC_BS))
            return -1;

        if (install)
            for (uint32_t j = 0; j < run; j++) {
                int c = claim(blk + k + j);
                if (c == NIL) break;             /* no buffer to spare; the data is
                                                  * already in the caller's hands */
                memcpy(data[c], out + (size_t)(k + j) * BC_BS, BC_BS);
            }
        k += run;
    }
    return 0;
}

int bcache_write(uint32_t blk, const void *buf)
{
    if (!hdr || blk >= dev_blocks) return -1;
    int i = lookup(blk);
    if (i != NIL) st.hits++;
    else {
        st.misses++;
        i = claim(blk);
        /* No buffer available (every one dirty and unwritable): fall back to a
         * direct device write rather than dropping the block. */
        if (i == NIL) return blk_write(blk * SPB, SPB, buf);
    }
    memcpy(data[i], buf, BC_BS);
    hdr[i].dirty = 1;
    touch(i);
    return 0;
}

int bcache_sync(void)
{
    if (!hdr) return blk_flush();
    int rc = 0;
    for (int i = 0; i < nbuf; i++)
        if (flush_one(i)) rc = -1;              /* stays dirty: retried next sync */
    st.syncs++;
    if (blk_flush()) rc = -1;
    return rc;
}

void bcache_drop(void)
{
    if (!hdr) return;
    for (int i = 0; i < NBUCK; i++) bucket[i] = NIL;
    lru_head = lru_tail = NIL;
    for (int i = nbuf - 1; i >= 0; i--) {
        hdr[i].valid = 0; hdr[i].dirty = 0; hdr[i].hnext = NIL;
        hdr[i].lru_prev = hdr[i].lru_next = NIL;
        lru_push_front(i);
    }
}

void bcache_getstats(struct bcache_stats *out)
{
    if (!out) return;
    unsigned long res = 0, dty = 0;
    for (int i = 0; hdr && i < nbuf; i++) {
        if (hdr[i].valid) res++;
        if (hdr[i].valid && hdr[i].dirty) dty++;
    }
    *out = st;
    out->resident = res;
    out->dirty    = dty;
}
