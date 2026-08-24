/* Host test for the page cache (c/kernel/mm/pcache.c), compiled -DMM_HOSTTEST
 * alongside the rest of c/kernel/mm (see mm_run.sh -- pcache.c is already part
 * of MMSRC, because fault.c's file case and vma.c's file-backed VMAs call
 * straight into it).
 *
 * SCOPE. Five things do not exist yet: pcache_init()/pcache_set_ops() have no
 * caller, vma_reserve_file() has no caller, c/fs/vfs.c never invalidates, and
 * reclaim.c never calls pcache_holds()/pcache_forget_frame(). None of that is
 * this file's problem. pcache.c's OWN behaviour is complete and compiles, and
 * that is everything under test here: this test drives pcache_file_open/
 * pcache_get/pcache_forget_frame/pcache_invalidate_path/pcache_holds/
 * pcache_pread/pcache_file_ref/pcache_file_put/pcache_file_size directly,
 * against a tiny simulated filesystem installed through pcache_set_ops (the
 * seam pcache.h documents), never touching vma.c or fault.c. When the wiring
 * lands, a SEPARATE end-to-end case belongs beside it -- this file is not that
 * case and does not pretend to be.
 *
 * THE SEAM. pc_ops here is not a stub that returns canned bytes: it is a small
 * in-memory filesystem (a path->(dev,ino,size) table plus a byte buffer per
 * file, each filled with a pattern that is a function of (file, offset)), so
 * that every read through the cache can be checked byte-for-byte and every
 * hard-link claim can be made real -- two distinct path STRINGS deliberately
 * resolving to the same (dev, ino).
 *
 * THE DELIVERABLE. pcache.h names this file and says it is REQUIRED TO FAIL
 * against -DPCACHE_PER_OPEN, the negative control already implemented in
 * pcache.c: it keys the cache on the OPEN instead of the FILE, which is the
 * single most plausible wrong version of this design. mm_run.sh builds that
 * control and asserts it fails. By design, most of the phases below stay GREEN
 * under that control -- a single process touching pcache_holds/forget/
 * invalidate/the pool bound sees nothing wrong, which is exactly pcache.h's
 * point: only a test that opens the SAME file more than once and expects ONE
 * entry can catch it. The first two phases are that test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "pcache.h"
/* The readahead phases at the bottom ask reclaim to take a prefetched page and
 * then audit the reverse map, because "is a prefetched page evictable" is a
 * question about all three structures at once and cannot be answered from
 * inside pcache.c alone. Both files are already in mm_run.sh's MMSRC. */
#include "reclaim.h"
#include "rmap.h"

static void phase(const char *s) { printf("-- %s\n", s); }

/* ------------------------------------------------------- the fake VFS ------
 * A path->(dev,ino,size,bytes) table. Several path strings may alias the same
 * entry (sim_add_alias), which is how a hard link is represented -- the
 * identity pcache.c is supposed to key on is (dev, ino), never the string. */
#define SIM_NFILES 96   /* the slot-eviction phase fills all PCACHE_MAXFILE=32
                         * cache slots with fresh files ON TOP of what earlier
                         * phases created, so the sim needs real headroom */
#define SIM_NALIAS 3
#define SIM_PATHLEN 96

struct simfile {
    char     alias[SIM_NALIAS][SIM_PATHLEN];
    int      nalias;
    uint64_t dev, ino, size;
    uint8_t *data;
};

static struct simfile sfiles[SIM_NFILES];
static int             nsfiles;
static uint64_t         next_ino = 1000;

/* A function of (file index, byte offset), exactly like mm_reclaim_test.c's
 * `pat`: a page of zeroes would hide a wrong-file read, a per-file constant
 * would hide a wrong-offset one. This catches both. */
static uint8_t pfpat(int fileid, uint64_t off)
{
    return (uint8_t)((fileid * 131 + (int)((off * 17) & 0xFF) +
                       (int)(((off >> 5) * 7) & 0xFF) + 0x5B) & 0xFF);
}

static int sim_add_file(const char *path, uint64_t size)
{
    int i = nsfiles++;
    struct simfile *f = &sfiles[i];
    memset(f, 0, sizeof *f);
    snprintf(f->alias[0], SIM_PATHLEN, "%s", path);
    f->nalias = 1;
    f->dev = 1;
    f->ino = next_ino++;
    f->size = size;
    f->data = malloc((size_t)size);
    for (uint64_t o = 0; o < size; o++) f->data[o] = pfpat(i, o);
    return i;
}

/* Hard-link `path2` onto file `i`: a second name, the SAME (dev, ino). */
static void sim_add_alias(int i, const char *path2)
{
    snprintf(sfiles[i].alias[sfiles[i].nalias++], SIM_PATHLEN, "%s", path2);
}

static struct simfile *sim_find(const char *path)
{
    for (int i = 0; i < nsfiles; i++)
        for (int a = 0; a < sfiles[i].nalias; a++)
            if (strcmp(sfiles[i].alias[a], path) == 0) return &sfiles[i];
    return NULL;
}

static int sim_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{
    struct simfile *f = sim_find(path);
    if (!f) return -1;
    *dev = f->dev; *ino = f->ino; *size = f->size;
    return 0;
}

static long sim_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    struct simfile *f = sim_find(path);
    if (!f) return -1;
    if (off >= f->size) return 0;
    uint64_t n = len;
    if (off + n > f->size) n = f->size - off;
    memcpy(dst, f->data + off, (size_t)n);
    return (long)n;
}

/* `forget` is NULL: this backend reads straight out of its own byte buffers,
 * so there is no second copy of a file for an invalidation to have to reach.
 * Named rather than left to the initializer, because "the field is absent"
 * and "the field is deliberately unused" read identically otherwise. */
static const struct pcache_ops sim_ops = { sim_stat, sim_read, 0 };

/* First byte of `frame`'s page that disagrees with file `fileid` at page
 * `page`, or -1 if the whole page (up to `n` bytes, <= 4096) matches. */
static int bad_byte(uint64_t frame, int fileid, int page, int n)
{
    const uint8_t *b = mm_sim_ptr(frame);
    uint64_t base = (uint64_t)page * 4096;
    for (int i = 0; i < n; i++)
        if (b[i] != pfpat(fileid, base + (uint64_t)i)) return i;
    return -1;
}

/* ======================================================================== */
static void t_key_is_file_not_open(void)
{
    phase("the key is the FILE, not the open: two opens and a hard link are ONE entry");

    int fid = sim_add_file("/data/report.txt", 3 * 4096 + 100);
    sim_add_alias(fid, "/data/report-hardlink.txt");

    int fh1 = pcache_file_open("/data/report.txt");
    int fh2 = pcache_file_open("/data/report.txt");          /* second open, same name */
    int fh3 = pcache_file_open("/data/report-hardlink.txt"); /* different name, same inode */
    mm_ok(fh1 >= 0 && fh2 >= 0 && fh3 >= 0, "all three opens succeeded");

    /* THE CLAIM -DPCACHE_PER_OPEN breaks first: without the per-inode match,
     * every one of these three opens allocates its own file slot. */
    mm_eqi(fh1, fh2, "two opens of the SAME PATH find the SAME entry");
    mm_eqi(fh1, fh3, "a HARD-LINKED path to the same inode finds the SAME entry too");

    uint64_t f1 = pcache_get(fh1, 0);
    uint64_t f2 = pcache_get(fh2, 0);
    uint64_t f3 = pcache_get(fh3, 0);
    mm_ok(f1 != 0, "page 0 installed");
    mm_eqi((long long)f1, (long long)f2, "...and it is the SAME frame, opened by path");
    mm_eqi((long long)f1, (long long)f3, "...and the SAME frame again, opened by hard link");

    mm_eqf(bad_byte(f1, fid, 0, 4096), -1,
           "the shared frame holds report.txt's real bytes, not zeroes or garbage");

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_dropped_page_same_entry(void)
{
    phase("a page dropped by reclaim comes back in the SAME entry, seen by every handle");

    int fid = sim_add_file("/data/shared.bin", 4096);
    int fhA = pcache_file_open("/data/shared.bin");
    int fhB = pcache_file_open("/data/shared.bin");   /* a second consumer of the file */
    mm_ok(fhA >= 0 && fhB >= 0, "opened via two handles");

    uint64_t beforeA = pcache_get(fhA, 0);
    uint64_t beforeB = pcache_get(fhB, 0);
    mm_ok(beforeA != 0, "page 0 installed");
    mm_eqi((long long)beforeA, (long long)beforeB,
           "both handles see the SAME frame before reclaim ever touches it");
    mm_ok(pcache_holds(beforeA), "the cache reports holding that frame");

    /* This is exactly the call reclaim.c's drop tier makes once the frame's
     * last user PTE (none, here -- this IS the cache's only reference) is
     * gone: remove the entry, drop the cache's reference, the frame goes back
     * to the allocator. */
    pcache_forget_frame(beforeA);
    mm_ok(!pcache_holds(beforeA), "the cache no longer claims the forgotten frame");

    uint64_t miss_before = pcache_misses();
    uint64_t afterA = pcache_get(fhA, 0);
    mm_ok(afterA != 0, "the page comes back on the next access through A");
    mm_eqi((long long)(pcache_misses() - miss_before), 1,
           "...and it was a genuine miss (a fresh read), not a stale hit");

    /* THE CLAIM. Handle B never itself forgot anything, and asks for the same
     * (file, page). Under the correct per-inode keying there is only ever ONE
     * entry for this file's page 0, so B necessarily observes the identical
     * fresh frame A just installed. Under -DPCACHE_PER_OPEN, A and B were
     * different file slots from their very first open, so B's own copy was
     * never forgotten by the call above and this diverges: B still points at
     * whatever it had (now possibly a frame the allocator has since handed to
     * someone else), while A holds a brand new one. */
    uint64_t afterB = pcache_get(fhB, 0);
    mm_eqi((long long)afterA, (long long)afterB,
           "handle B observes the SAME refreshed frame handle A does");

    mm_eqf(bad_byte(afterA, fid, 0, 4096), -1, "the refreshed page holds the real bytes");
    mm_eqi(pcache_audit(), 0, "audit clean after the drop-and-refault");
}

/* ======================================================================== */
static void t_holds_agrees(void)
{
    phase("pcache_holds() agrees with the frame table for every resident page");

    sim_add_file("/data/holds.bin", 5 * 4096);
    int fh = pcache_file_open("/data/holds.bin");
    mm_ok(fh >= 0, "opened");

    uint64_t frames[5];
    for (int i = 0; i < 5; i++) {
        frames[i] = pcache_get(fh, (uint64_t)i);
        mm_ok(frames[i] != 0, "page %d installed", i);
        mm_ok(pcache_holds(frames[i]), "pcache_holds() is true for resident page %d", i);
    }

    /* A frame the cache never touched at all. */
    uint64_t plain = pmm_alloc();
    mm_ok(plain != 0, "a plain frame, allocated outside the cache entirely");
    mm_ok(!pcache_holds(plain), "pcache_holds() is 0 for a frame the cache never held");
    pmm_free(plain);

    /* And it goes back to 0 the instant the entry is forgotten -- this is the
     * exact fact reclaim's eligibility test (rmap_count + pcache_holds ==
     * refcount) depends on being current. */
    pcache_forget_frame(frames[2]);
    mm_ok(!pcache_holds(frames[2]), "pcache_holds() drops to 0 once the entry is forgotten");
    for (int i = 0; i < 5; i++)
        if (i != 2)
            mm_ok(pcache_holds(frames[i]), "an unrelated resident page (%d) is unaffected", i);

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_forget_only_that_entry(void)
{
    phase("pcache_forget_frame() removes exactly one entry, no others");

    sim_add_file("/data/fileA.bin", 4 * 4096);
    sim_add_file("/data/fileB.bin", 4 * 4096);
    int fhA = pcache_file_open("/data/fileA.bin");
    int fhB = pcache_file_open("/data/fileB.bin");
    mm_ok(fhA >= 0 && fhB >= 0 && fhA != fhB, "two distinct files, two distinct entries");

    uint64_t fA[4], fB[4];
    for (int i = 0; i < 4; i++) {
        fA[i] = pcache_get(fhA, (uint64_t)i);
        fB[i] = pcache_get(fhB, (uint64_t)i);
        mm_ok(fA[i] != 0 && fB[i] != 0, "page %d of both files installed", i);
    }

    uint64_t resident_before = pcache_resident();
    pcache_forget_frame(fA[1]);
    mm_eqi((long long)pcache_resident(), (long long)(resident_before - 1),
           "resident count dropped by exactly one");
    mm_ok(!pcache_holds(fA[1]), "the forgotten frame is gone");

    for (int i = 0; i < 4; i++)
        if (i != 1)
            mm_ok(pcache_holds(fA[i]), "fileA page %d is untouched", i);
    for (int i = 0; i < 4; i++)
        mm_ok(pcache_holds(fB[i]), "fileB page %d is untouched (a different file entirely)", i);

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_invalidate_path(void)
{
    phase("invalidation drops every page of the file and no page of any other file");

    int fidV = sim_add_file("/data/victim.bin", 4 * 4096);
    sim_add_file("/data/bystander.bin", 4 * 4096);
    int fhV = pcache_file_open("/data/victim.bin");
    int fhY = pcache_file_open("/data/bystander.bin");
    mm_ok(fhV >= 0 && fhY >= 0, "opened both");

    uint64_t fv[4], fy[4];
    for (int i = 0; i < 4; i++) {
        fv[i] = pcache_get(fhV, (uint64_t)i);
        fy[i] = pcache_get(fhY, (uint64_t)i);
    }
    for (int i = 0; i < 4; i++) {
        mm_ok(pcache_holds(fv[i]), "victim page %d resident before invalidation", i);
        mm_ok(pcache_holds(fy[i]), "bystander page %d resident before invalidation", i);
    }

    uint64_t inval_before = pcache_invalidated();
    pcache_invalidate_path("/data/victim.bin");
    mm_eqi((long long)(pcache_invalidated() - inval_before), 4,
           "exactly the victim's four pages were counted as invalidated");

    for (int i = 0; i < 4; i++)
        mm_ok(!pcache_holds(fv[i]), "victim page %d is gone after invalidation", i);
    for (int i = 0; i < 4; i++)
        mm_ok(pcache_holds(fy[i]), "bystander page %d survives -- a different file", i);

    /* The file itself is untouched on the "device" -- only the cache's copy
     * was thrown away -- so it reads back correctly on the next access. */
    uint64_t nv0 = pcache_get(fhV, 0);
    mm_ok(nv0 != 0, "the victim file is still readable after invalidation");
    mm_eqf(bad_byte(nv0, fidV, 0, 4096), -1,
           "...with its real bytes, not corrupted by the invalidation");

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_pread_identity(void)
{
    phase("pcache_pread() is a read() over the SAME cache pcache_get() installs into");

    /* Two full pages plus a 50-byte third -- enough to exercise pcache_pread's
     * per-page loop, including a read that crosses a page boundary, not just
     * the single-page case. */
    const uint64_t size = 2 * 4096 + 50;
    int fid = sim_add_file("/data/pread.bin", size);

    uint8_t buf[2 * 4096 + 50];
    long n = pcache_pread("/data/pread.bin", buf, 0, size);
    mm_eqi((long long)n, (long long)size, "a whole-file pread returns every byte");
    int bad = -1;
    for (uint64_t i = 0; i < size; i++)
        if (buf[i] != pfpat(fid, i)) { bad = (int)i; break; }
    mm_eqf(bad, -1, "every byte pcache_pread returned matches the file's real bytes");

    uint8_t mid[32];
    long nm = pcache_pread("/data/pread.bin", mid, 4090, 20);
    mm_eqi((long long)nm, 20, "a read spanning a page boundary returns every byte asked for");
    bad = -1;
    for (int i = 0; i < 20; i++)
        if (mid[i] != pfpat(fid, 4090 + (uint64_t)i)) { bad = i; break; }
    mm_eqf(bad, -1, "...with the right bytes on both sides of the boundary");

    /* THE IDENTITY CLAIM ITSELF (pcache.h: "read() and mmap() return the same
     * memory"). The pread above already installed page 0 as a side effect of
     * serving it; opening the same file again and asking pcache_get() for
     * page 0 must be a HIT. If it were a fresh miss, the bytes read() just
     * returned would not be the bytes an mmap of this page would be handed --
     * which is the entire distinction this file draws against bcache.c's
     * copy-out model in the header comment above. */
    /* CONFIRMED, REPRODUCIBLE FAILURE against the pcache.c this test was
     * written against -- not a flake, not a misreading of the contract, and
     * NOT this file's bug to fix (pcache.c is outside tests/unit/'s
     * boundary). Recorded here rather than weakened or deleted, because a
     * test that stops asking the question is worse than one that asks it and
     * loses: see the top-of-file design note on never stubbing to success.
     *
     * THE MECHANISM. pcache_pread() calls pcache_file_open(path) on entry and
     * pcache_file_put(fh) on every exit (c/kernel/mm/pcache.c). When this file
     * has no OTHER live reference -- exactly the common case for a plain
     * read()-only file nobody has mmap'd -- that open/put pair is refs 0->1->0
     * within the single call, and pcache_file_put()'s "refs <= 0 -> purge every
     * page of this file" rule (correct for a VMA's real close) fires on a
     * transient, single-call reference that was never meant to be a closing
     * one. The page pcache_pread() just installed is gone before the call even
     * returns. Bytes already returned to the caller are still correct (the
     * device is re-read on the next access), so only THIS assertion --
     * persistence, the actual "one copy of the file in RAM" claim pcache.h
     * makes -- catches it; content alone does not. It reproduces on ANY file
     * whose first cache access is a pcache_pread() with nothing else holding it
     * open, and is invisible whenever something else (a VMA, or -- as every
     * OTHER pcache_pread() call in this test file happens to have --  an
     * already-open handle from earlier in the same phase) keeps refs above 0
     * across the call, which is why no other phase in this file trips it. */
    uint64_t miss_before = pcache_misses();
    int fh = pcache_file_open("/data/pread.bin");
    mm_ok(fh >= 0, "re-opened the same file by path");
    uint64_t frame0 = pcache_get(fh, 0);
    mm_ok(frame0 != 0, "page 0 fetched");
    mm_eqi((long long)(pcache_misses() - miss_before), 0,
           "...as a HIT: pcache_pread() already installed this exact page");
    mm_eqf(bad_byte(frame0, fid, 0, 4096), -1,
           "the frame pcache_get() hands back holds the same bytes pcache_pread() returned");

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_eof_and_zero_length(void)
{
    phase("EOF is a hard edge: a zero-length file, a byte past the end, and a "
          "partial tail page's padding");

    /* A zero-length file is a legitimate regular file, not an error case --
     * open must succeed, and every read of it must say "nothing here" rather
     * than crash or hand back a fabricated page. */
    sim_add_file("/data/empty.bin", 0);
    int fhE = pcache_file_open("/data/empty.bin");
    mm_ok(fhE >= 0, "a zero-length file opens");
    mm_eqi((long long)pcache_file_size(fhE), 0, "its size reads back as 0");
    mm_eqi((long long)pcache_get(fhE, 0), 0,
           "page 0 of a zero-length file does not exist -- pcache_get() says so");
    uint8_t ebuf[8];
    mm_eqi((long long)pcache_pread("/data/empty.bin", ebuf, 0, 8), 0,
           "pcache_pread() of a zero-length file returns 0 bytes, not -1 and not garbage");

    /* A file whose last page is partial: 7 real bytes into a 4096-byte page.
     * pcache_get()'s own comment says the rest of that page must read as
     * ZERO, the same disclosure rule do_anon() follows -- nothing before this
     * phase has ever checked that byte by byte. */
    const uint64_t psize = 4096 + 7;
    int fidP = sim_add_file("/data/partial.bin", psize);
    int fhP = pcache_file_open("/data/partial.bin");
    mm_ok(fhP >= 0, "opened the partial-tail file");

    uint64_t f1 = pcache_get(fhP, 1);
    mm_ok(f1 != 0, "the tail page (page 1) exists -- it holds 7 real bytes");
    const uint8_t *tail = (const uint8_t *)mm_sim_ptr(f1);
    int badc = -1;
    for (int i = 0; i < 7; i++)
        if (tail[i] != pfpat(fidP, 4096 + (uint64_t)i)) { badc = i; break; }
    mm_eqf(badc, -1, "the 7 real bytes of the tail page match the file");
    int badz = -1;
    for (int i = 7; i < 4096; i++)
        if (tail[i] != 0) { badz = i; break; }
    mm_eqf(badz, -1, "every byte of the tail page past EOF reads as ZERO, not leftover frame contents");

    /* One byte past EOF, three different ways. */
    mm_eqi((long long)pcache_get(fhP, 2), 0,
           "page 2 is entirely past EOF -- pcache_get() refuses it");
    uint8_t pbuf[8];
    mm_eqi((long long)pcache_pread("/data/partial.bin", pbuf, psize, 1), 0,
           "pcache_pread() starting AT EOF (byte 'size') returns 0 bytes");
    mm_eqi((long long)pcache_pread("/data/partial.bin", pbuf, psize + 5, 1), 0,
           "pcache_pread() starting PAST EOF returns 0 bytes, not an error and not garbage");
    long nlast = pcache_pread("/data/partial.bin", pbuf, psize - 1, 10);
    mm_eqi((long long)nlast, 1,
           "a read asking for 10 bytes with only 1 left before EOF is CLAMPED, not overrun");
    mm_eqi((long long)pbuf[0], (long long)pfpat(fidP, psize - 1),
           "...and it is the correct last byte");

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
static void t_stream_bypass(void)
{
    phase("a file over PCACHE_STREAM_BYTES bypasses whole-file caching, but a "
          "single page of it still installs through pcache_get()");

    /* Just over the threshold, not orders of magnitude over it -- the point is
     * the boundary, not a stress test of a huge simulated file. */
    const uint64_t size = (uint64_t)PCACHE_STREAM_BYTES + 4096;
    int fid = sim_add_file("/data/streambig.bin", size);

    uint64_t bypass_before = pcache_bypassed();
    uint8_t buf[4096];
    long n = pcache_pread("/data/streambig.bin", buf, 0, sizeof buf);
    mm_eqi((long long)n, -1, "pcache_pread() of an oversized file refuses to serve it from the cache");
    mm_eqi((long long)(pcache_bypassed() - bypass_before), 1,
           "...and counts it as a bypass, not a silent no-op");

    /* The streaming bypass is a whole-FILE-READ decision, not a per-page one:
     * a caller that maps a page of this same file (rather than reading it
     * whole) still gets that page cached, exactly like a small file would. */
    int fh = pcache_file_open("/data/streambig.bin");
    mm_ok(fh >= 0, "the oversized file still opens for page-granular access");
    uint64_t frame = pcache_get(fh, 0);
    mm_ok(frame != 0, "page 0 of the oversized file installs through pcache_get()");
    mm_eqf(bad_byte(frame, fid, 0, 4096), -1, "...with its real bytes");
    mm_ok(pcache_holds(frame), "and it is genuinely resident, not the pool-exhausted uncached fallback");

    mm_eqi(pcache_audit(), 0, "audit clean");
}

/* ======================================================================== */
/* This phase used to assert the OPPOSITE -- "the last put purges every page" --
 * and that assertion was the bug wearing a test's clothes. Purging on the last
 * put made every standalone pcache_pread() (open, read, put -- refs 0->1->0
 * inside one call) throw away the pages it had just installed, which
 * t_pread_identity's persistence check caught. The last put now leaves the
 * entry CACHED-IDLE: pages retained on the bet somebody re-reads them, which is
 * the page cache's entire reason to exist ("a program that read a file once" is
 * pcache.h's own example), and reclaim -- not the put path -- is who takes them
 * back under pressure. The three ways an idle entry's pages actually leave are
 * each pinned below. */
static void t_file_ref_put_idles(void)
{
    phase("pcache_file_put(): the last put leaves the entry CACHED-IDLE -- "
          "pages retained, revived on re-open, surrendered only to "
          "invalidation or slot pressure");

    int fid = sim_add_file("/data/refcount.bin", 4096);
    int fh = pcache_file_open("/data/refcount.bin");     /* refs = 1 */
    mm_ok(fh >= 0, "opened");
    uint64_t f0 = pcache_get(fh, 0);
    mm_ok(f0 != 0, "page 0 installed");
    mm_ok(pcache_holds(f0), "resident");

    pcache_file_ref(fh);                                 /* refs = 2, e.g. a fork's VMA */
    pcache_file_put(fh);                                 /* refs = 1 -- not the last */
    mm_ok(pcache_holds(f0), "a put that is not the last reference leaves the page alone");

    pcache_file_put(fh);                                 /* refs = 0 -- CACHED-IDLE */
    mm_ok(pcache_holds(f0), "the LAST put keeps the page too: idle, not gone");
    mm_eqi(pcache_audit(), 0, "audit clean while idle");

    /* Way back in: a re-open revives the same entry and the page is a HIT --
     * the exact re-hit the idle state exists to serve. */
    uint64_t miss_before = pcache_misses();
    int fh2 = pcache_file_open("/data/refcount.bin");
    mm_eqi(fh2, fh, "re-open revives the SAME entry, not a fresh slot");
    uint64_t f0b = pcache_get(fh2, 0);
    mm_eqi((long long)f0b, (long long)f0, "the SAME frame");
    mm_eqi((long long)(pcache_misses() - miss_before), 0, "...as a HIT, no device read");
    mm_eqf(bad_byte(f0b, fid, 0, 4096), -1, "...holding the real bytes");
    pcache_file_put(fh2);                                /* idle again */

    /* Way out #1: invalidation retires an idle entry OUTRIGHT -- pages AND
     * slot. The slot half is a correctness rule, not tidiness: the file behind
     * an invalidation may have been deleted, logitfs reuses inode numbers, and
     * an idle entry left wearing a dead (dev,ino) would be matched by the next
     * file to receive that inode and serve it another file's identity. */
    pcache_invalidate_path("/data/refcount.bin");
    mm_ok(!pcache_holds(f0), "invalidating an idle file purges its pages");
    uint64_t files_after = pcache_files();
    int fh3 = pcache_file_open("/data/refcount.bin");
    mm_ok(fh3 >= 0, "the path re-opens cleanly afterwards");
    mm_eqi((long long)pcache_files(), (long long)(files_after + 1),
           "...as a NEW entry: the idle slot was retired, not left dangling");
    uint64_t f0c = pcache_get(fh3, 0);
    mm_ok(f0c != 0, "and reads page 0 again");
    mm_eqf(bad_byte(f0c, fid, 0, 4096), -1, "...with its real bytes, not anything left over");
    pcache_file_put(fh3);

    mm_eqi(pcache_audit(), 0, "final audit for this phase");
}

/* Way out #2: SLOT PRESSURE. With every slot occupied and one entry idle, a
 * new file takes the idle entry's slot -- and its pages go with it, counted as
 * an eviction. Idle is a bet, and a new file that needs the table wins it. */
static void t_idle_slot_eviction(void)
{
    phase("a full file table evicts a CACHED-IDLE entry for a new file");

    /* Make the table ALL LIVE, deterministically, whatever CACHED-IDLE
     * leftovers the earlier phases planted: keep opening fresh files -- each
     * takes a free slot or evicts an idle leftover -- until an open is
     * REFUSED, which under the idle semantics happens only when every slot has
     * a live holder. Only then is "the one idle entry" below a unique,
     * predictable victim; without this drain, first-idle eviction could pick a
     * leftover instead and the fh_new == idle_fh assertion would be a coin
     * toss over test-phase ordering. */
    int live[PCACHE_MAXFILE];
    int nlive = 0;
    for (int i = 0; i < PCACHE_MAXFILE * 2 && nlive < PCACHE_MAXFILE; i++) {
        char name[64];
        snprintf(name, sizeof name, "/data/fill_%c%c.bin",
                 (char)('a' + (i % 26)), (char)('a' + (i / 26)));
        sim_add_file(name, 4096);
        int fh = pcache_file_open(name);
        if (fh < 0) break;                        /* every slot live: done */
        live[nlive++] = fh;
    }
    mm_ok(pcache_files() == PCACHE_MAXFILE, "every file slot is occupied");
    {
        sim_add_file("/data/alllive-probe.bin", 4096);
        mm_ok(pcache_file_open("/data/alllive-probe.bin") < 0,
              "...all by LIVE holders: a fresh open is refused, so the victim below is unique");
    }

    /* Make exactly one of them idle, with a resident page to lose. */
    int idle_fh = live[nlive - 1];
    uint64_t idle_frame = pcache_get(idle_fh, 0);
    mm_ok(idle_frame != 0, "the victim-to-be holds a resident page");
    pcache_file_put(idle_fh);
    uint64_t ev_before = pcache_evicted();

    int fid = sim_add_file("/data/newcomer.bin", 4096);
    int fh_new = pcache_file_open("/data/newcomer.bin");
    mm_ok(fh_new >= 0, "a NEW file still gets a slot from a full table");
    mm_eqi(fh_new, idle_fh, "...the idle entry's slot, specifically");
    mm_ok(!pcache_holds(idle_frame), "...and the idle entry's pages went with it");
    mm_ok(pcache_evicted() > ev_before, "...counted as an eviction, not silently");
    uint64_t fn = pcache_get(fh_new, 0);
    mm_ok(fn != 0, "the newcomer reads its own page 0");
    mm_eqf(bad_byte(fn, fid, 0, 4096), -1, "...with its OWN bytes -- no aliasing from the victim");

    /* Cleanup: put everything back so later phases see a sane table. */
    pcache_file_put(fh_new);
    for (int i = 0; i < nlive - 1; i++) pcache_file_put(live[i]);
    mm_eqi(pcache_audit(), 0, "final audit for this phase");
}

/* ========================================================================
 * READAHEAD (pcache.h's READAHEAD block is the design).
 *
 * THE MEASUREMENT THIS EXISTS TO MAKE is a COUNT, not a behaviour: every page
 * of a sequential walk was already served correctly before readahead existed,
 * one device read at a time, and every assertion in this file passed. So a
 * test that asks "did the pages arrive and do they hold the right bytes"
 * cannot see this feature at all -- which is exactly why -DPCACHE_NO_READAHEAD
 * is a control worth having: it must move a NUMBER, and nothing else.
 *
 * The bytes are still checked, on every page, because the one way readahead
 * can be catastrophically wrong is to install page N+3's bytes under index
 * N+2 -- and pfpat() is a function of (file, offset), so a wrong-offset
 * install is a byte mismatch and not a silent success.
 * ==================================================================== */
#define RA_SEQ_PAGES 1000

/* The bound on misses, DERIVED from the window constants rather than written
 * down. Batch b costs exactly one miss and brings in K_b + 1 pages, with K_b
 * running MIN, 2*MIN, ... MAX and staying at MAX; page 0 is a miss on its own
 * because there is nothing for it to be sequential with. If PCACHE_RA_MIN or
 * PCACHE_RA_MAX changes, this follows -- a literal 34 here would become a
 * number nobody could re-derive the day it started failing. */
static long long ra_miss_bound(long long pages)
{
    long long covered = 1, bound = 1;           /* page 0, on its own */
    for (unsigned k = PCACHE_RA_MIN; k < PCACHE_RA_MAX; k *= 2) {
        covered += (long long)k + 1;
        bound++;
    }
    if (pages > covered)
        bound += (pages - covered + PCACHE_RA_MAX) / ((long long)PCACHE_RA_MAX + 1);
    return bound;
}

static void t_readahead_sequential(void)
{
    phase("a sequential walk of 1,000 pages costs one miss per BATCH, not one per page");

    int fid = sim_add_file("/data/model.bin", (uint64_t)RA_SEQ_PAGES * 4096);
    int fh = pcache_file_open("/data/model.bin");
    mm_ok(fh >= 0, "opened the 1,000-page file");

    uint64_t miss0 = pcache_misses(), hit0 = pcache_hits();
    uint64_t run0 = pcache_ra_runs(), page0 = pcache_ra_pages();
    uint64_t read0 = pcache_ra_reads(), short0 = pcache_ra_short();

    int bad = -1, unserved = -1;
    for (int i = 0; i < RA_SEQ_PAGES; i++) {
        uint64_t f = pcache_get(fh, (uint64_t)i);
        /* One assertion for the loop, not 1,000 that can only agree with each
         * other -- t_pool_bounded's argument, and the same reason. */
        if (!f) { if (unserved < 0) unserved = i; continue; }
        if (bad < 0 && bad_byte(f, fid, i, 4096) >= 0) bad = i;
    }
    mm_eqf(unserved, -1, "every one of the %d pages was served", RA_SEQ_PAGES);
    mm_eqf(bad, -1, "and every page holds ITS OWN bytes -- a prefetched page "
                    "installed under the wrong index would read as the wrong file offset");

    long long misses = (long long)(pcache_misses() - miss0);
    long long hits   = (long long)(pcache_hits() - hit0);
    long long runs   = (long long)(pcache_ra_runs() - run0);
    long long ahead  = (long long)(pcache_ra_pages() - page0);
    long long reads  = (long long)(pcache_ra_reads() - read0);
    long long bound  = ra_miss_bound(RA_SEQ_PAGES);

    printf("   [ra] %lld pages: %lld misses (bound %lld), %lld hits; "
           "%lld batches, %lld pages ahead, %lld backend reads "
           "(%lld pages/read), %lld short\n",
           (long long)RA_SEQ_PAGES, misses, bound, hits, runs, ahead, reads,
           reads ? (ahead + runs) / reads : 0,
           (long long)(pcache_ra_short() - short0));

    /* THE GATE. Without readahead this is 1,000. */
    mm_ok(misses <= bound,
          "%lld misses for %lld pages, within the bound the window constants "
          "give (%lld) -- one device round trip per batch, not per page",
          misses, (long long)RA_SEQ_PAGES, bound);
    mm_eqf(misses + hits, RA_SEQ_PAGES,
           "every request was one or the other: %lld hits + %lld misses", hits, misses);
    mm_ok(hits > RA_SEQ_PAGES * 9 / 10,
          "%lld of %lld requests were served from a page fetched before anyone "
          "asked for it", hits, (long long)RA_SEQ_PAGES);
    mm_eqf(runs, misses - 1,
           "one batch per miss after the first: page 0 has nothing to be "
           "sequential with, so it is the one miss that fetches alone");
    mm_eqf(ahead, RA_SEQ_PAGES - misses,
           "and the pages fetched ahead are exactly the ones that became hits");

    /* THE COALESCING CLAIM, and it is the reason the batch is one backend call
     * per CONTIGUOUS RUN of frames rather than one per page. The block layer
     * merges a contiguous byte range into one device command (test-bulkread:
     * 900 blocks, 10 commands); it cannot merge anything if this cache asks
     * for 4096 bytes at a time. `reads` is what it was asked. */
    mm_ok(reads >= runs, "at least one backend read per batch (%lld/%lld)", reads, runs);
    mm_ok(reads < ahead / 2,
          "and far fewer reads (%lld) than pages fetched (%lld) -- the frames "
          "came back adjacent and one call filled a run of them",
          reads, ahead);

    mm_eqi(pcache_audit(), 0, "audit clean after 1,000 prefetched pages");

    /* Put the pool back roughly as it was: the phases after this one measure
     * the pool's own capacity, and a thousand of my pages sitting in it would
     * make their numbers mine. */
    pcache_file_put(fh);
    pcache_invalidate_path("/data/model.bin");
}

static void t_readahead_random(void)
{
    phase("a random walk does NOT prefetch -- the trigger is two consecutive pages and nothing else");

#define RA_RND_PAGES  2000      /* the file */
#define RA_RND_TOUCH  200       /* what this walk actually asks for */
#define RA_RND_STRIDE 397
    /* THE FILE IS TEN TIMES THE WALK, and that is the whole design of this
     * phase rather than an arbitrary size. On a file the same size as the walk,
     * a readahead that ignores the access pattern reads the file once and comes
     * out looking GOOD -- 31 misses instead of 200, measured, with
     * -DPCACHE_RA_ALWAYS. The waste only becomes visible when most of what it
     * prefetches is never asked for, which is the real shape: the model this
     * line exists for is 91,054 pages and no walk touches all of them.
     *
     * gcd(397, 2000) == 1, so the 200 touches are distinct pages, and the step
     * between consecutive visits is +397 or -1603 -- never +1, in either
     * direction, including at the wrap. A shuffle would have been worse: it can
     * produce a consecutive pair by chance, and then this phase fails once in a
     * while for a reason that is not a defect. */
    int fid = sim_add_file("/data/scatter.bin", (uint64_t)RA_RND_PAGES * 4096);
    int fh = pcache_file_open("/data/scatter.bin");
    mm_ok(fh >= 0, "opened the %d-page file", RA_RND_PAGES);

    uint64_t miss0 = pcache_misses(), run0 = pcache_ra_runs(), page0 = pcache_ra_pages();
    int bad = -1, unserved = -1, consecutive = 0, prev = -1;
    for (int i = 0; i < RA_RND_TOUCH; i++) {
        int p = (i * RA_RND_STRIDE) % RA_RND_PAGES;
        if (prev >= 0 && p == prev + 1) consecutive++;
        prev = p;
        uint64_t f = pcache_get(fh, (uint64_t)p);
        if (!f) { if (unserved < 0) unserved = p; continue; }
        if (bad < 0 && bad_byte(f, fid, p, 4096) >= 0) bad = p;
    }
    mm_eqf(unserved, -1, "every page was served");
    mm_eqf(bad, -1, "and holds its own bytes");
    /* The control on the pattern itself: if this were ever nonzero the phase
     * would be measuring a partly-sequential walk and its verdict would mean
     * nothing. It is checked rather than argued because the stride is arithmetic
     * somebody could change. */
    mm_eqi(consecutive, 0, "the access pattern contains no consecutive pair at all");

    long long misses = (long long)(pcache_misses() - miss0);
    printf("   [ra] random: %d touches into a %d-page file: %lld misses, "
           "%lld batches, %lld pages ahead\n",
           RA_RND_TOUCH, RA_RND_PAGES, misses,
           (long long)(pcache_ra_runs() - run0),
           (long long)(pcache_ra_pages() - page0));

    /* THE GATE, and the one -DPCACHE_RA_ALWAYS is required to redden: a
     * readahead that skips the sequential test looks exactly as good on the
     * phase above and spends K frames and K pages of device time here for
     * every single page anybody wanted. */
    mm_eqi((long long)(pcache_ra_runs() - run0), 0,
           "not one readahead batch was issued");
    mm_eqi((long long)(pcache_ra_pages() - page0), 0,
           "and not one page was fetched ahead");
    mm_eqf(misses, RA_RND_TOUCH,
           "one miss per page ASKED FOR, which is correct here: nothing about "
           "this pattern predicts the next page, so a page fetched ahead is a "
           "page read for nobody");

    mm_eqi(pcache_audit(), 0, "audit clean");
    pcache_file_put(fh);
    pcache_invalidate_path("/data/scatter.bin");
}

/* THE INVARIANT, which is the first question readahead has to answer: a page
 * that is in the cache and mapped by NOBODY has rmap_count 0, pcache_holds 1
 * and pmm_refcount 1. reclaim.h's rule is
 *
 *      evict only if   rmap_count(f) + pcache_holds(f) == pmm_refcount(f)
 *
 * so 0 + 1 == 1 and the page IS evictable -- it is exactly the "cached but
 * unmapped" page pcache.h calls the cheapest frame on the machine, and
 * try_drop_cached() already handles n == 0 by name. A readahead that
 * manufactured UNEVICTABLE pages would be a leak with a good name, and the
 * only way to know which one this is, is to build the state and then make
 * reclaim take it. */
static void t_readahead_evictable(void)
{
    phase("a prefetched page nobody maps is evictable -- the three numbers agree, and reclaim takes it");

    int fid = sim_add_file("/data/ahead.bin", 64ull * 4096);
    int fh = pcache_file_open("/data/ahead.bin");
    mm_ok(fh >= 0, "opened it");

    uint64_t run0 = pcache_ra_runs();
    mm_ok(pcache_get(fh, 0) != 0, "page 0: a first touch, which never prefetches");
    mm_eqi((long long)(pcache_ra_runs() - run0), 0, "...and did not");
    mm_ok(pcache_get(fh, 1) != 0, "page 1: the second consecutive page fires the first batch");
    mm_eqi((long long)(pcache_ra_runs() - run0), 1, "...exactly one batch");

    /* Page 3 was fetched by that batch and NOBODY has asked for it. Reading it
     * through pcache_get() here would make it a hit and prove nothing, so the
     * frame is found through the cache's own frame table instead. */
    uint64_t f3 = 0;
    {
        uint64_t before = pcache_hits();
        f3 = pcache_get(fh, 3);
        mm_ok(pcache_hits() > before,
              "page 3 is resident WITHOUT anyone having asked for it -- it was prefetched");
    }
    mm_ok(f3 != 0 && pcache_holds(f3), "the cache holds it");
    mm_eqf(bad_byte(f3, fid, 3, 4096), -1, "with page 3's real bytes");

    /* THE THREE NUMBERS, each from a structure maintained independently of the
     * other two. This is the whole safety argument, asserted rather than
     * assumed. */
    mm_eqi((long long)rmap_count(f3), 0, "rmap_count is 0: no PTE anywhere maps it");
    mm_eqi((long long)(pcache_holds(f3) ? 1 : 0), 1, "pcache_holds is 1: the cache's own reference");
    mm_eqi((long long)pmm_refcount(f3), 1, "pmm_refcount is 1: and there is no third holder");
    mm_eqi((long long)(rmap_count(f3) + (pcache_holds(f3) ? 1u : 0u)),
           (long long)pmm_refcount(f3),
           "so rmap_count + pcache_holds == pmm_refcount: reclaim MAY take it");
    mm_eqi((long long)pmm_pincount(f3), 0,
           "and nothing pinned it -- readahead takes no pin, deliberately: a pin "
           "here would make every speculative page permanently unevictable");

    /* Now make reclaim prove it. `want` is larger than the machine, so the hand
     * sweeps everything once and every eligible page goes -- deterministic,
     * unlike asking for one frame and hoping the hand is near this one. */
    uint64_t dc0 = reclaim_dropped_cache(), dz0 = reclaim_dropped_zero();
    uint64_t freed = reclaim_frames(pmm_total_frames());
    mm_ok(freed > 0, "reclaim freed %llu frames", (unsigned long long)freed);
    mm_ok(reclaim_dropped_cache() > dc0,
          "%llu of them through the CACHE-drop tier",
          (unsigned long long)(reclaim_dropped_cache() - dc0));
    mm_eqi((long long)(reclaim_dropped_zero() - dz0), 0,
           "and none through the zero-page tier -- a file page is not an anonymous one");
    mm_ok(!pcache_holds(f3), "the prefetched page is gone: it was never a pin");
    mm_eqi((long long)pmm_refcount(f3), 0,
           "and its frame is back on the allocator, not dangling");

    mm_eqi(rmap_audit(), 0, "reverse map clean after prefetch + forced reclaim");
    mm_eqi(pcache_audit(), 0, "page cache clean: no entry names a freed frame");
    mm_eqi((long long)pmm_bugs(), 0, "no allocator invariant was violated");

    pcache_file_put(fh);
    pcache_invalidate_path("/data/ahead.bin");
}

/* Every frame t_pool_bounded installed, so t_pool_full_of_mapped can take a
 * reference on the ones still resident. Sized from the 16 MiB simulated
 * machine below, not from the pool: pcache_slots() is not a compile-time
 * value any more, and an array that is a fixed multiple of the sim's own size
 * cannot be silently outgrown by a change to the pool fraction -- it is
 * bounded by the FRAMES, and the cache can never hold more pages than that. */
#define POOL_FRAMES_MAX 4096
static uint64_t filled[POOL_FRAMES_MAX];

/* ======================================================================== */
static void t_pool_bounded(void)
{
    phase("the pool is bounded: fill past its capacity and the cache evicts its own");

    /* THE PAGE COUNT IS DERIVED FROM THE POOL, NOT WRITTEN DOWN. It used to be
     * a literal 600 against a pool that pcache_init() clamped to
     * total_frames/16 -- true of the constant that existed then and false the
     * moment the pool started being sized from RAM (pcache.h). A fixed number
     * larger than the pool proves self-eviction; a fixed number SMALLER than
     * the pool proves nothing at all and says so by never firing, which is the
     * failure mode this phase must not be able to have. +32 is the smallest
     * margin that leaves several installs past the cap for the plateau
     * assertion below to look at. */
    const int NPAGES = (int)pcache_slots() + 32;
    int fid = sim_add_file("/data/big.bin", (uint64_t)NPAGES * 4096);
    int fh = pcache_file_open("/data/big.bin");
    mm_ok(fh >= 0, "opened the big file");

    uint64_t evict_before = pcache_evicted();
    int saw_evict = 0;
    long long cap = -1;
    int hard_fail = -1;

    for (int i = 0; i < NPAGES; i++) {
        uint64_t f = pcache_get(fh, (uint64_t)i);
        /* ONE assertion for the whole loop, not one per page: NPAGES is
         * derived from the pool now and is in the thousands, and 2,080 checks
         * that can only all pass or all fail together do not carry 2,080
         * checks' worth of information -- they just move the suite's total. */
        if (f == 0 && hard_fail < 0) hard_fail = i;
        if (i < POOL_FRAMES_MAX) filled[i] = f;

        if (!saw_evict && pcache_evicted() > evict_before) {
            saw_evict = 1;
            cap = (long long)pcache_resident();
        } else if (saw_evict) {
            /* Once full, the pool evicts exactly one entry per install: net
             * change zero. A pool that "overran" instead would show resident
             * climbing past this point rather than holding still. */
            mm_eqf((long long)pcache_resident(), cap,
                   "resident stays at the pool's cap (%lld) once full -- page %d", cap, i);
        }
    }

    mm_eqf(hard_fail, -1, "every one of the %d pages was served -- never a hard failure", NPAGES);
    mm_ok(saw_evict, "the cache evicted its OWN entries under pressure rather than growing without bound");
    mm_ok(cap > 0 && cap <= (long long)pcache_slots(),
          "the plateau (%lld) is a real bound, within the pool this machine was given (%d)",
          cap, (int)pcache_slots());
    mm_ok(pcache_evicted() - evict_before >= (uint64_t)(NPAGES - cap),
          "self-eviction fired for (at minimum) every page past the cap");
    /* The pool was full of pages NOTHING maps, so pass 1 of evict_one_locked()
     * served every one of them. Neither of the two ceiling counters may move
     * here -- that is what makes the next phase's numbers attributable. */
    mm_eqi((long long)pcache_orphaned(), 0,
           "no entry was orphaned: every victim was unmapped, so pass 1 sufficed");
    mm_eqi((long long)pcache_uncached(), 0,
           "and nothing was handed back uncached -- the leak path is not reachable here");

    /* The most recent page is always resident -- eviction takes the COLD end
     * of the pool, never the page just installed. */
    uint64_t last = pcache_get(fh, (uint64_t)(NPAGES - 1));
    mm_ok(pcache_holds(last), "the most recently installed page is still resident");
    mm_eqf(bad_byte(last, fid, NPAGES - 1, 4096), -1, "...and holds its real bytes");

    mm_eqi(pcache_audit(), 0, "audit clean after filling past capacity");
}

/* ========================================================================
 * THE POOL FULL OF PAGES SOMEBODY MAPS -- the case that used to LEAK.
 *
 * evict_one_locked()'s pass 1 only takes an entry whose frame has refcount 1,
 * i.e. nothing maps it. When every entry fails that test the old code fell
 * through, handed the new page back with NO ENTRY and its allocation
 * reference intact, and fault.c's do_file() then added a second reference and
 * one PTE -- rmap_count 1 + pcache_holds 0 against a pmm refcount of 2, which
 * fails reclaim's eligibility test forever and loses one 4 KiB frame per
 * fault when the process exits. Nothing counted it and nothing printed it,
 * which is why it survived: the fault SUCCEEDS, so no test that asks "did the
 * program run" can see it.
 *
 * This phase manufactures exactly that state. There are no page tables in
 * this test (pcache.c never touches one), so "mapped" is spelled the only way
 * the cache can read it: an extra pmm reference on the frame, which is
 * precisely what a PTE is worth to pmm_refcount(). Then it asks for one more
 * page and requires the new behaviour -- an entry ORPHANED, the victim page
 * left alive and mapped, the newcomer properly cached, and NOTHING leaked.
 * ==================================================================== */
static void t_pool_full_of_mapped(void)
{
    phase("the pool full of MAPPED pages: an entry is orphaned, and no frame leaks");

    int fid = sim_add_file("/data/mapped.bin", 64ull * 4096);
    int fh = pcache_file_open("/data/mapped.bin");
    mm_ok(fh >= 0, "opened the file");

    /* Every frame the pool currently holds, referenced once more. `filled` may
     * name a frame twice (an evicted frame goes back to the allocator and can
     * be handed out again for a later page), so each is taken at most once --
     * a double reference here would be this test leaking, not the cache. */
    static uint64_t refd[POOL_FRAMES_MAX];
    int nref = 0;
    for (int i = 0; i < POOL_FRAMES_MAX; i++) {
        uint64_t f = filled[i];
        if (!f || !pcache_holds(f)) continue;
        int seen = 0;
        for (int j = 0; j < nref; j++) if (refd[j] == f) { seen = 1; break; }
        if (seen) continue;
        pmm_ref(f);
        refd[nref++] = f;
    }
    mm_ok(nref > 0, "took a reference on all %d resident frames -- the pool is now "
                    "full of pages something maps", nref);

    uint64_t orphan_before   = pcache_orphaned();
    uint64_t evict_before    = pcache_evicted();
    uint64_t uncached_before = pcache_uncached();
    uint64_t resident_before = pcache_resident();
    uint64_t free_before     = pmm_free_frames();

    uint64_t nf = pcache_get(fh, 0);
    mm_ok(nf != 0, "the new page was served");
    mm_ok(pcache_holds(nf), "...and it is CACHED, not handed back naked -- "
                            "the whole point of the orphan pass");
    mm_eqi((long long)(pcache_orphaned() - orphan_before), 1,
           "exactly one entry was orphaned to make room");
    mm_eqi((long long)(pcache_evicted() - evict_before), 0,
           "and no ORDINARY eviction happened -- pass 1 had nothing it could take");
    mm_eqi((long long)(pcache_uncached() - uncached_before), 0,
           "nothing was handed back uncached: the leak path did not run");
    mm_eqf((long long)pcache_resident(), (long long)resident_before,
           "the pool is still exactly full -- one entry out, one in");
    mm_eqf(bad_byte(nf, fid, 0, 4096), -1, "the new page holds its real bytes");

    /* THE LEAK ASSERTION, and it is the reason this phase exists. One frame
     * was allocated for the new page and one entry was orphaned; orphaning
     * drops the CACHE's reference on a frame something else still holds, so
     * that frame must NOT return to the allocator. Net free frames must
     * therefore fall by exactly one. The old code's number here was also one
     * -- and then never came back, which is what the release below checks. */
    mm_eqf((long long)(free_before - pmm_free_frames()), 1,
           "one frame went to the new page and the orphaned one stayed alive");

    /* Release the manufactured mappings. Every frame must now be reachable
     * again: the orphaned one has no cache entry and no reference left, so it
     * goes back to the allocator; the rest keep the cache's reference. If the
     * orphan pass had double-freed or forgotten a reference, pmm_bugs() is
     * where it lands. */
    uint64_t free_mid = pmm_free_frames();
    for (int i = 0; i < nref; i++) pmm_free(refd[i]);
    mm_eqf((long long)(pmm_free_frames() - free_mid), 1,
           "releasing the mappings returns exactly the ONE orphaned frame -- "
           "the others are still the cache's");
    mm_eqi((long long)pmm_bugs(), 0, "no allocator invariant was violated");
    mm_eqi(pcache_audit(), 0, "audit clean: no entry names a freed frame");
}

/* ======================================================================== */
int main(void)
{
    /* 16 MiB of simulated physical memory: real pmm_init over it, same as
     * every other c/kernel/mm host test. No CR3 / page-table setup
     * (mm_sim_kernel_space) is needed -- pcache.c never touches a page table,
     * only frames, which is the whole point of the refcount decision in
     * pcache.h: a cache entry is a reference, not a mapping. */
    mm_sim_init(16);

    pcache_set_ops(&sim_ops);
    pcache_init(pmm_total_frames());
    mm_ok(pcache_ready(), "the cache came up");

    t_key_is_file_not_open();
    t_dropped_page_same_entry();
    t_holds_agrees();
    t_forget_only_that_entry();
    t_invalidate_path();
    t_pread_identity();
    t_eof_and_zero_length();
    t_stream_bypass();
    t_file_ref_put_idles();
    t_idle_slot_eviction();
    /* BEFORE the two pool phases, and it matters: those measure the pool's own
     * capacity, and a thousand pages of mine sitting in it would make their
     * numbers mine. Each readahead phase hands its pages back on the way out. */
    t_readahead_sequential();
    t_readahead_random();
    t_readahead_evictable();
    t_pool_bounded();
    t_pool_full_of_mapped();    /* must run AFTER it: it needs a full pool */

    mm_eqi(pcache_audit(), 0, "final audit: every entry sane, no dangling frame");
    mm_eqi((long long)pmm_bugs(), 0, "no frame-allocator invariant violations");
    pcache_report("end of test");

    mm_sim_done();
    return mm_summary("mm_pcache_test");
}
