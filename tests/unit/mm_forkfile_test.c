/* Host test for a FILE-BACKED PAGE ACROSS A FORK, and for the fixed-address
 * file reservation the ELF loader creates (c/kernel/mm/vma.c
 * vma_reserve_file_fixed + c/kernel/mm/fault.c do_file + c/kernel/mm/vmm.c
 * vmm_clone_user), compiled -DMM_HOSTTEST alongside the rest of c/kernel/mm.
 *
 * WHY THIS FILE EXISTS, said plainly: a file-backed PTE survives fork BY
 * ACCIDENT.
 *
 * VMM_PTE_FILE appears in exactly two files, fault.c and reclaim.c, and
 * vmm_clone_user has no case for it at all. It survives a fork only because a
 * file PTE is not writable and not copy-on-write, so it falls into the
 * "already read-only keeps its flags" branch and is installed WHOLE by
 * vmm_map_raw_in(). That branch was written for NX, years before anything was
 * file-backed. It is correct. Nothing named it, nothing tested it, and the one
 * line it rests on is the kind that gets "cleaned up" into
 * vmm_map_page_in(dst, va, e & MM_PTE_ADDR, e & 0xFFF) by somebody who has no
 * reason to know better -- which loses every bit above 51 and hands a forked
 * child an executable copy of its parent's no-execute page. That is the
 * negative control below.
 *
 * The second thing being pinned is the REFCOUNT IDENTITY. rmap.h's whole
 * safety argument is `evict only if rmap_count(f) == pmm_refcount(f)`, and a
 * page-cache page breaks that arithmetic on purpose: the cache holds a
 * reference with no PTE behind it, so the rule becomes
 * rmap_count + pcache_holds == pmm_refcount. A fork adds one PTE and one rmap
 * entry and must add exactly one pmm reference -- one too few and the frame is
 * freed underneath a live mapping, one too many and the page is never
 * reclaimable again. Both are silent. So the identity is asserted directly,
 * with all three numbers printed, rather than inferred from "reclaim still
 * worked".
 *
 * Run: sh tests/unit/mm_run.sh   (Makefile: test-mm) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "mm.h"
#include "rmap.h"
#include "reclaim.h"
#include "pcache.h"
#include "mmhost.h"      /* mm_host_cr3: the simulated active address space */

/* The page-fault error-code bits, as fault.c defines them privately. */
#define PF_P 0x01
#define PF_W 0x02
#define PF_U 0x04
#define PF_I 0x10

/* Two areas, deliberately with DIFFERENT prot, because the property under test
 * lives in a bit that only one of them sets. A program's text is executable
 * and its rodata is not; do_file() puts MM_PTE_NX on the second and not the
 * first, and MM_PTE_NX is bit 63 -- the only bit in a file PTE that a
 * reassembling fork can actually lose. A test that mapped only text would pass
 * against the broken clone and prove nothing. */
#define TEXT_VA  0x45000000ull
#define RO_VA    0x45010000ull
#define NPG      4
#define TEXT_FOFF 0x1000ull       /* the .aex header is one page, as mkaex.py now writes it */
#define RO_FOFF   0x9000ull

/* A simulated file: 16 pages, each filled with a pattern that is a function of
 * BOTH the page index and the offset within the page. mm_reclaim_test.c argues
 * this at length and the argument is the same here -- zeroes would pass a
 * wrong-page read and a per-page constant would pass a wrong-offset one. */
#define SIMPAGES 16
static uint8_t simfile[SIMPAGES][4096];

static uint8_t simpat(int page, int off)
{
    return (uint8_t)((page * 61 + off * 7 + (off >> 6) * 29 + 0x33) & 0xFF);
}

static int sim_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{
    if (strcmp(path, "/t/prog") != 0) return -1;
    *dev = 7; *ino = 42; *size = (uint64_t)SIMPAGES * 4096;
    return 0;
}

static long sim_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    if (strcmp(path, "/t/prog") != 0) return -1;
    uint64_t page = off / 4096;
    if (page >= SIMPAGES) return 0;
    if (len > 4096) len = 4096;
    memcpy(dst, simfile[page], (size_t)len);
    return (long)len;
}

/* forget: NULL. This backend serves out of its own arrays, so there is no
 * second copy of the file for an invalidation to have to reach. */
static const struct pcache_ops sim_ops = { sim_stat, sim_read, 0 };

static void phase(const char *s) { printf("-- %s\n", s); }

static uint64_t pte_at(uint64_t cr3, uint64_t va)
{
    uint64_t *e = vmm_pte(cr3, va);
    return e ? *e : 0;
}

static int page_matches(uint64_t phys, int filepage)
{
    const uint8_t *p = mm_sim_ptr(phys);
    for (int i = 0; i < 4096; i++)
        if (p[i] != simpat(filepage, i)) return i;
    return -1;
}

int main(void)
{
    mm_sim_init(64);
    mm_sim_kernel_space();

    for (int p = 0; p < SIMPAGES; p++)
        for (int i = 0; i < 4096; i++) simfile[p][i] = simpat(p, i);

    pcache_init(pmm_total_frames());
    mm_ok(pcache_ready(), "the page cache came up");
    pcache_set_ops(&sim_ops);

    /* ---------------------------------------------------------------------
     * 0. THE RESERVATION ITSELF, including what it refuses.
     *
     * vma_reserve_file_fixed exists because vma_reserve_file() places only
     * inside [MM_MMAP_BASE, MM_MMAP_TOP) and a program's text is at the
     * address it was LINKED at, far below that window. So the first thing to
     * prove is that a fixed address is honoured EXACTLY -- an area that lands
     * somewhere else is a file mapping of the right bytes at the wrong
     * address, which is worse than no mapping at all. */
    phase("the fixed file reservation: the address is the address, and the "
          "refusals are real");
    uint64_t parent = vmm_new_space();
    mm_ok(parent != 0, "a parent address space");
    mm_host_cr3 = parent;

    int fh = pcache_file_open("/t/prog");
    mm_ok(fh >= 0, "the file opened in the page cache");

    mm_eqi(vma_reserve_file_fixed(parent, TEXT_VA, NPG * 4096,
                                  VMA_READ | VMA_EXEC, fh, TEXT_FOFF), 0,
           "an executable file area at the link address");
    mm_eqi(vma_reserve_file_fixed(parent, RO_VA, NPG * 4096,
                                  VMA_READ, fh, RO_FOFF), 0,
           "a read-only file area above it");

    mm_ok(vma_reserve_file_fixed(parent, 0x45020000ull, 4096,
                                 VMA_READ | VMA_WRITE, fh, 0x1000) < 0,
          "a WRITABLE file area is refused -- there is no writeback in this "
          "line and no private-file COW fault case");
    mm_ok(vma_reserve_file_fixed(parent, 0x45020000ull, 4096,
                                 VMA_READ, fh, 0x800) < 0,
          "an unaligned file offset is refused -- the fault path divides it by "
          "4096 and would silently serve the wrong page");
    mm_ok(vma_reserve_file_fixed(parent, TEXT_VA, 4096, VMA_READ, fh, 0x1000) < 0,
          "an overlapping area is refused, never evicted");
    mm_ok(vma_reserve_file_fixed(parent, 0x10000000ull, 4096, VMA_READ, fh, 0x1000) < 0,
          "an address outside the private user region is refused");
    mm_ok(vma_reserve_file_fixed(parent, 0x45020000ull, 4096, VMA_READ, -1, 0) < 0,
          "and a mapping with no file handle is not a file mapping");

    /* The areas took their own references, so the loader's transient one goes
     * back now -- exactly the discipline mmsys.c states for SYS_MMAP_FILE. If
     * this put were the last reference the entry would go CACHED-IDLE and the
     * pages below could be purged out from under a live mapping. */
    pcache_file_put(fh);

    /* ---------------------------------------------------------------------
     * 1. THE FAULT: what a file page's PTE actually says. */
    phase("do_file(): present, user, read-only, FILE, and NX iff not executable");
    mm_eqi(mm_fault_in(parent, TEXT_VA, PF_U | PF_I), 1,
           "an instruction fetch on the text area faults in");
    mm_eqi(mm_fault_in(parent, RO_VA, PF_U), 1,
           "a read on the read-only area faults in");

    uint64_t tp = pte_at(parent, TEXT_VA), rp = pte_at(parent, RO_VA);
    mm_ok(!!(tp & 1), "the text PTE is present");
    mm_ok(!!(tp & VMM_PTE_FILE), "and marked VMM_PTE_FILE -- reclaim's drop tier keys on it");
    mm_ok(!(tp & 2), "and NOT writable");
    mm_ok(!!(tp & 4), "and user-accessible");
    mm_ok(!(tp & MM_PTE_NX), "and executable: the VMA said VMA_EXEC");
    mm_ok(!!(rp & VMM_PTE_FILE), "the read-only PTE is VMM_PTE_FILE too");
    /* !! on every one of these: mm_ok() takes an int and MM_PTE_NX is bit 63,
     * so a bare `pte & MM_PTE_NX` truncates to 0 and the assertion reports on
     * a bit it never looked at. Found by this test failing against the
     * CORRECT kernel while its own negative control passed the same line. */
    mm_ok(!!(rp & MM_PTE_NX), "and NO-EXECUTE: the VMA did not say VMA_EXEC");
    mm_ok(!(tp & VMM_PTE_ANON) && !(rp & VMM_PTE_ANON),
          "neither is ANON -- FILE and ANON are mutually exclusive by construction");

    uint64_t tf = tp & MM_PTE_ADDR, rf = rp & MM_PTE_ADDR;
    mm_eqi(page_matches(tf, (int)(TEXT_FOFF / 4096)), -1,
           "the text page holds file page 1, byte for byte");
    mm_eqi(page_matches(rf, (int)(RO_FOFF / 4096)), -1,
           "the read-only page holds file page 9, byte for byte");
    mm_ok(pcache_holds(tf) && pcache_holds(rf), "the cache holds both frames");

    /* The offset arithmetic, checked at a page that is NOT the first of its
     * area -- an area whose foff is applied only to its base would pass every
     * assertion above and serve the wrong page here. */
    mm_eqi(mm_fault_in(parent, RO_VA + 2 * 4096, PF_U), 1, "the third page of the area faults in");
    mm_eqi(page_matches(pte_at(parent, RO_VA + 2 * 4096) & MM_PTE_ADDR,
                        (int)(RO_FOFF / 4096) + 2), -1,
           "and holds file page 11 -- foff + (va - start), not foff alone");

    /* ---------------------------------------------------------------------
     * 2. THE FORK. Bit for bit, not "has FILE set": the defect this pins is a
     * branch that takes the entry apart and puts it back together, and what it
     * loses is a bit no coarser assertion can see. */
    phase("fork: the child's file PTE is the parent's, bit for bit");
    uint64_t child = vmm_new_space();
    mm_ok(child != 0, "a child address space");
    mm_eqi(vmm_clone_user(child, parent), 0, "fork");

    uint64_t ctp = pte_at(child, TEXT_VA), crp = pte_at(child, RO_VA);
    mm_ok(ctp == tp, "the text PTE survived whole (parent %llx, child %llx)",
          (unsigned long long)tp, (unsigned long long)ctp);
    mm_ok(crp == rp, "the NO-EXECUTE PTE survived whole (parent %llx, child %llx)",
          (unsigned long long)rp, (unsigned long long)crp);
    mm_ok(!!(crp & MM_PTE_NX),
          "and the child's copy is still no-execute -- this is the bit a "
          "reassembling clone drops");
    mm_ok(!(ctp & VMM_PTE_COW),
          "a file page is not copy-on-write: it is read-only for real, so a "
          "write to it must stay a fatal protection fault");

    /* ---------------------------------------------------------------------
     * 3. THE THREE-NUMBER IDENTITY, asserted rather than inferred. */
    phase("the refcount identity: rmap_count + pcache_holds == pmm_refcount");
    printf("   text frame %llx: rmap %d + cache %d == refcount %d\n",
           (unsigned long long)tf, rmap_count(tf), pcache_holds(tf),
           pmm_refcount(tf));
    mm_eqi(rmap_count(tf), 2, "two page tables map it");
    mm_eqi(pcache_holds(tf), 1, "the cache holds it once");
    mm_eqi(pmm_refcount(tf), 3, "three references: two PTEs and the cache");
    mm_eqi(rmap_count(tf) + pcache_holds(tf), pmm_refcount(tf),
           "and the identity reclaim's eligibility test rests on holds");

    /* ---------------------------------------------------------------------
     * 4. RECLAIM TAKES IT AND BOTH SIDES GET IT BACK. This is the mechanism
     * the whole page cache was built to feed and it had never had a producer:
     * before file-backed text, tier 1 could only drop an all-zero anonymous
     * page. */
    phase("reclaim's drop tier takes a shared file page, and both spaces "
          "fault it back onto the same frame");
    uint64_t dropped_before = reclaim_dropped_cache();
    /* Sweep enough of the machine to be sure the clock reaches these frames;
     * the assertion is about THESE frames' state, not about the count. */
    reclaim_frames(64);
    mm_ok(reclaim_dropped_cache() > dropped_before,
          "pages left through the CACHE-drop split (%llu)",
          (unsigned long long)(reclaim_dropped_cache() - dropped_before));

    int text_went = !(pte_at(parent, TEXT_VA) & 1);
    mm_ok(text_went, "the shared text page was unmapped from the parent");
    mm_ok(!(pte_at(child, TEXT_VA) & 1),
          "AND from the child -- one frame, every PTE, or the survivor points "
          "at memory the allocator has given away");
    mm_eqi(pmm_refcount(tf), 0, "the frame went back to the allocator");

    mm_host_cr3 = parent;
    mm_eqi(mm_fault_in(parent, TEXT_VA, PF_U | PF_I), 1, "the parent faults it back");
    uint64_t tf2 = pte_at(parent, TEXT_VA) & MM_PTE_ADDR;
    mm_eqi(page_matches(tf2, (int)(TEXT_FOFF / 4096)), -1,
           "with the right bytes, re-read from the file");
    mm_host_cr3 = child;
    mm_eqi(mm_fault_in(child, TEXT_VA, PF_U | PF_I), 1, "the child faults it back");
    mm_eqi((long long)(pte_at(child, TEXT_VA) & MM_PTE_ADDR), (long long)tf2,
           "onto the SAME frame -- which is the sharing, restored, and the "
           "reason the cache is keyed on the file and not on the mapping");
    mm_eqi(pte_at(child, TEXT_VA), pte_at(parent, TEXT_VA),
           "and with the same PTE again");

    /* ---------------------------------------------------------------------
     * 5. THE PARENT DIES AND THE CHILD KEEPS ITS PROGRAM. vma.h claims
     * vma_space_clone takes a reference per area and that this is what stops a
     * mapped file being purged when one holder exits; nothing had ever
     * exercised that across a real clone. */
    phase("the parent exits: the child's mapping and its frames survive");
    uint64_t survivor = pte_at(child, TEXT_VA) & MM_PTE_ADDR;
    mm_ok(pcache_files() >= 1, "the file entry is live before the parent goes");
    vmm_free_space(parent);
    mm_host_cr3 = child;

    mm_eqi((long long)(pte_at(child, TEXT_VA) & MM_PTE_ADDR), (long long)survivor,
           "the child still maps the same frame");
    mm_ok(pcache_holds(survivor),
          "and the cache still holds it -- the child's area is a reference of "
          "its own, taken by vma_space_clone");
    mm_eqi(page_matches(survivor, (int)(TEXT_FOFF / 4096)), -1,
           "with the bytes intact");
    mm_eqi(mm_fault_in(child, RO_VA + 3 * 4096, PF_U), 1,
           "and a page the parent never touched still faults in for the child");
    mm_eqi(page_matches(pte_at(child, RO_VA + 3 * 4096) & MM_PTE_ADDR,
                        (int)(RO_FOFF / 4096) + 3), -1,
           "from the right place in the file");

    /* And when the LAST holder goes, what is left is CACHED-IDLE -- pages, no
     * holders -- which is pcache.c's deliberate choice and not a leak: "an
     * entry with refs == 0 is CACHED-IDLE, not gone, and that distinction is
     * the whole point of a page cache". So the assertion is NOT that the entry
     * disappears (it must not); it is that every frame is now held by the
     * CACHE ALONE, which is the state that makes it the cheapest frame on the
     * machine -- no PTE to tear down, no device write. An area that forgot to
     * put its reference is visible here as a frame whose rmap count never
     * reached zero. */
    phase("the child exits too: the pages are cached-idle, held by nobody but "
          "the cache");
    /* Fault it in first: the reclaim phase above swept the whole machine, so
     * this area's pages are legitimately gone by now. Taking the frame without
     * this reads a cleared PTE and every number below is about address 0. */
    mm_eqi(mm_fault_in(child, RO_VA, PF_U), 1, "the child faults its page back");
    uint64_t last = pte_at(child, RO_VA) & MM_PTE_ADDR;
    mm_ok(last != 0 && pcache_holds(last), "a frame the child still maps");
    vmm_free_space(child);
    mm_eqi(rmap_count(last), 0, "nothing maps it any more");
    mm_eqi(pmm_refcount(last), 1, "and exactly one reference is left: the cache's");
    mm_eqi(rmap_count(last) + pcache_holds(last), pmm_refcount(last),
           "the identity still holds with no mappings at all");
    uint64_t dropped2 = reclaim_dropped_cache();
    reclaim_frames(64);
    mm_ok(reclaim_dropped_cache() > dropped2,
          "and reclaim can take it back with no PTE to unmap and no device "
          "write (%llu more)",
          (unsigned long long)(reclaim_dropped_cache() - dropped2));

    mm_eqi(rmap_bugs(), 0, "the reverse map reported no bugs");
    mm_eqi(reclaim_bugs(), 0, "reclaim never found the reverse map disagreeing");
    mm_eqi(pcache_audit(), 0, "every cache entry still names an allocated frame");

    pcache_report("end of test");
    mm_sim_done();
    return mm_summary("mm_forkfile_test");
}
