/* Host test for SHARED ANONYMOUS MEMORY (c/kernel/mm/shm.c + the MAP_SHARED
 * paths in vma.c / vmm.c / fault.c), compiled -DMM_HOSTTEST alongside the rest
 * of c/kernel/mm.
 *
 * WHY THIS FILE EXISTS, said plainly: EVERY WAY THIS FEATURE BREAKS IS SILENT.
 *
 * A shared mapping has exactly one observable property -- a write on one side
 * is seen on the other -- and every plausible bug preserves everything else
 * while destroying that one thing. The process does not crash. The bytes are
 * not corrupted. The permissions are right, the refcounts balance, rmap_audit
 * stays clean, and both address spaces look perfectly well formed to anything
 * that inspects them. What happens is that the two processes quietly stop
 * talking to each other, each one reading back only its own writes. There is no
 * error to report and nothing to attribute it to.
 *
 * There are three such bugs and this file is built around them:
 *
 *   1. FORK PRIVATISES IT. vmm_clone_user's job is to make every writable page
 *      copy-on-write, which is right for all memory except this. Add shared
 *      memory, do not touch fork, and the child gets a COW copy: correct bytes
 *      at the moment of the fork, divergent from the first write onward. This
 *      is `-DSHM_FORK_COPY`, the negative control.
 *
 *   2. RECLAIM TAKES IT. A shared frame mapped by N processes has N PTEs, N
 *      rmap entries and -- if nothing else held it -- N references, so it would
 *      BALANCE reclaim's eligibility test and be eligible. Tier 1 then drops it
 *      (a segment before its first write is all zeroes, which is exactly the
 *      condition the drop tier looks for) and each side re-derives a PRIVATE
 *      zero page. shm.c's answer is the segment's own extra reference; this
 *      file asserts the arithmetic that makes it work, and then actually runs
 *      reclaim at the frame and requires it to still be there.
 *
 *   3. THE INDEX IS WRONG. Two mappings of one segment at different offsets, or
 *      at different addresses, that resolve to the wrong page look exactly like
 *      working shared memory until the two sides disagree about which page they
 *      are on. The pattern below is a function of BOTH the page index and the
 *      byte offset, for the reason mm_reclaim_test.c argues at length: zeroes
 *      pass a wrong-page read and a per-page constant passes a wrong-offset one.
 *
 * NOTE ON WHAT "VISIBLE IN THE OTHER PROCESS" MEANS HERE. There is no MMU in a
 * host test, so a write cannot be issued through a virtual address. The
 * assertion is therefore made one level down and is STRONGER, not weaker: both
 * address spaces' page tables are resolved to physical frames and those frames
 * are required to be THE SAME FRAME, after which a write through one side's
 * resolved pointer is read back through the other side's. A test that could
 * only compare bytes would pass against two identical copies; requiring frame
 * identity is what distinguishes sharing from copying, which is the entire
 * subject of this file.
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
#include "shm.h"
#include "reclaim.h"
#include "mmhost.h"      /* mm_host_cr3: the simulated active address space */

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

#define PF_P 0x01
#define PF_W 0x02
#define PF_U 0x04

#define SEGPAGES 4

static void phase(const char *s) { printf("-- %s\n", s); }

/* A function of the page index AND the byte offset -- see the header. */
static uint8_t pat(int page, int off)
{
    return (uint8_t)((page * 149 + off * 23 + (off >> 6) * 11 + 0x71) & 0xFF);
}

static void fill_pattern(void *p, int page)
{
    uint8_t *b = p;
    for (int i = 0; i < 4096; i++) b[i] = pat(page, i);
}

static int check_pattern(const void *p, int page)
{
    const uint8_t *b = p;
    for (int i = 0; i < 4096; i++)
        if (b[i] != pat(page, i)) return i;
    return -1;
}

static uint64_t pte_at(uint64_t cr3, uint64_t va)
{
    uint64_t *e = vmm_pte(cr3, va);
    return e ? *e : 0;
}

static uint64_t frame_at(uint64_t cr3, uint64_t va)
{
    uint64_t e = pte_at(cr3, va);
    return (e & PRESENT) ? (e & MM_PTE_ADDR) : 0;
}

/* ===================================================================== */
/* 0. THE SEGMENT TABLE ITSELF, including every refusal.
 *
 * The refusals are here rather than left implicit because each one is a way a
 * caller could otherwise be handed something other than what it asked for, and
 * three of them (the size on a second open, the name with a separator, the
 * permission) would be indistinguishable from success at the call site. */
static int t_table(void)
{
    phase("the segment table: what it creates, and what it refuses");

    mm_eqi(shm_open("frames", 0, 0600, SHM_CREAT | SHM_WRITE, 1000), SHM_E_INVAL,
           "a zero-page segment is refused");
    mm_eqi(shm_open("frames", SHM_PAGEMAX + 1, 0600, SHM_CREAT, 1000), SHM_E_INVAL,
           "a segment larger than SHM_PAGEMAX is refused, not clamped");
    mm_eqi(shm_open("a/b", 4, 0600, SHM_CREAT, 1000), SHM_E_INVAL,
           "a name with a path separator is refused -- this table is not a filesystem");
    mm_eqi(shm_open("", 4, 0600, SHM_CREAT, 1000), SHM_E_INVAL,
           "an empty name is refused; an unnamed segment is shm_create_anon's job");
    mm_eqi(shm_open("nothere", 4, 0600, 0, 1000), SHM_E_NOENT,
           "opening a segment that does not exist WITHOUT SHM_CREAT is an error, "
           "not a silent create");

    int sh = shm_open("frames", SEGPAGES, 0600, SHM_CREAT | SHM_WRITE, 1000);
    mm_ok(sh >= 0, "a named segment is created");
    mm_eqi((long long)shm_pages(sh), SEGPAGES, "with the size it asked for");

    mm_eqi(shm_open("frames", SEGPAGES, 0600, SHM_CREAT | SHM_EXCL | SHM_WRITE, 1000),
           SHM_E_EXIST, "SHM_EXCL refuses an existing segment");

    /* The size on a SECOND open is ignored, and that is the point: a resize
     * would move every other mapping's pages underneath it. */
    int again = shm_open("frames", SEGPAGES * 2, 0600, SHM_CREAT | SHM_WRITE, 1000);
    mm_ok(again == sh, "a second open finds the SAME segment");
    mm_eqi((long long)shm_pages(sh), SEGPAGES,
           "and its size is unchanged -- the second caller's size is ignored, never applied");
    shm_put(again);

    /* Permissions. 0600 means owner-only, and "other" gets nothing. */
    mm_eqi(shm_open("frames", 0, 0, SHM_WRITE, 1001), SHM_E_ACCES,
           "a different uid is refused by mode 0600");
    mm_eqi(shm_open("frames", 0, 0, 0, 1001), SHM_E_ACCES,
           "...for reading too, not only for writing");
    int as_root = shm_open("frames", 0, 0, SHM_WRITE, 0);
    mm_ok(as_root == sh, "root bypasses the mode, as it does in c/fs");
    shm_put(as_root);
    mm_eqi(shm_unlink("frames", 1001), SHM_E_ACCES,
           "and a uid that may not write may not unlink either");

    return sh;
}

/* ===================================================================== */
/* 1. ONE SEGMENT, TWO ADDRESS SPACES, AT DIFFERENT ADDRESSES.
 *
 * Two UNRELATED processes -- no fork anywhere in this case -- find the same
 * segment by name and map it. The addresses are deliberately different, because
 * a mapping that only works when both sides land on the same virtual address is
 * not shared memory, it is a coincidence. */
static void t_two_spaces(int sh, uint64_t *out_a, uint64_t *out_b,
                         uint64_t *out_va, uint64_t *out_vb)
{
    phase("two unrelated address spaces map one segment, at different addresses");

    uint64_t a = vmm_new_space(), b = vmm_new_space();
    mm_ok(a && b, "two address spaces");

    mm_host_cr3 = a;
    uint64_t va = vma_reserve_shm(a, 0, SEGPAGES * 4096, VMA_READ | VMA_WRITE, sh, 0);
    mm_ok(va != 0, "space A reserves the whole segment");

    /* A hint one page further up, so B's addresses cannot coincide with A's. */
    mm_host_cr3 = b;
    uint64_t vb = vma_reserve_shm(b, va + 0x200000, SEGPAGES * 4096,
                                  VMA_READ | VMA_WRITE, sh, 0);
    mm_ok(vb != 0, "space B reserves it too");
    mm_ok(vb != va, "at a DIFFERENT virtual address -- sharing is by frame, not by address");

    /* Fault every page into both spaces. */
    for (int i = 0; i < SEGPAGES; i++) {
        mm_host_cr3 = a;
        mm_eqf(mm_fault_in(a, va + i * 4096, PF_W | PF_U), 1, "A faults page %d", i);
        mm_host_cr3 = b;
        mm_eqf(mm_fault_in(b, vb + i * 4096, PF_W | PF_U), 1, "B faults page %d", i);
    }

    /* THE PTE. A shared page is the only writable page in this kernel that is
     * not copy-on-write, and it must claim neither of the two re-derivable
     * origins -- see vmm.h's VMM_PTE_SHM. */
    uint64_t pa = pte_at(a, va);
    mm_ok(!!(pa & PRESENT),      "A's PTE is present");
    mm_ok(!!(pa & USER),         "and user-accessible");
    mm_ok(!!(pa & WRITABLE),     "and WRITABLE -- a segment page is written, that is the point");
    mm_ok(!!(pa & VMM_PTE_SHM),  "and marked VMM_PTE_SHM");
    mm_ok(!(pa & VMM_PTE_COW),   "and NOT copy-on-write");
    mm_ok(!(pa & VMM_PTE_ANON),  "and NOT anonymous -- do_anon would re-derive it PRIVATE");
    mm_ok(!(pa & VMM_PTE_FILE),  "and NOT file-backed -- there is no file to re-read");

    /* THE SHARING, stated as frame identity and then as a write-through. */
    int same = 1;
    for (int i = 0; i < SEGPAGES; i++) {
        uint64_t fa = frame_at(a, va + i * 4096), fb = frame_at(b, vb + i * 4096);
        mm_eqf((long long)fa, (long long)shm_frame(sh, i),
               "A's page %d is the segment's frame %d", i, i);
        if (fa != fb) same = 0;
        mm_eqf((long long)fa, (long long)fb,
               "A and B resolve page %d to the SAME frame", i);
    }
    mm_ok(same, "every page of the mapping is one frame, not two copies");

    /* Write through A, read through B. */
    for (int i = 0; i < SEGPAGES; i++)
        fill_pattern(mm_sim_ptr(frame_at(a, va + i * 4096)), i);
    for (int i = 0; i < SEGPAGES; i++)
        mm_eqf(check_pattern(mm_sim_ptr(frame_at(b, vb + i * 4096)), i), -1,
               "A's write to page %d is visible in B, byte for byte", i);

    *out_a = a; *out_b = b; *out_va = va; *out_vb = vb;
}

/* ===================================================================== */
/* 2. A PARTIAL MAPPING AT AN OFFSET.
 *
 * The index arithmetic, on its own. A third space maps only the UPPER HALF of
 * the segment, so its page 0 must be the segment's page SEGPAGES/2 -- and the
 * pattern written above says which page it actually got. An off-by-one here is
 * a program reading somebody else's page and finding perfectly valid data in
 * it. */
static void t_offset(int sh)
{
    phase("a mapping at an offset resolves to the right pages, not the first ones");

    uint64_t c = vmm_new_space();
    mm_ok(c != 0, "a third address space");
    mm_host_cr3 = c;

    const uint64_t half = SEGPAGES / 2;
    uint64_t vc = vma_reserve_shm(c, 0, half * 4096, VMA_READ | VMA_WRITE,
                                  sh, half * 4096);
    mm_ok(vc != 0, "it maps only the upper half of the segment");

    for (uint64_t i = 0; i < half; i++)
        mm_eqf(mm_fault_in(c, vc + i * 4096, PF_W | PF_U), 1,
               "C faults its page %d", (int)i);

    for (uint64_t i = 0; i < half; i++)
        mm_eqf(check_pattern(mm_sim_ptr(frame_at(c, vc + i * 4096)), (int)(i + half)), -1,
               "C's page %d holds SEGMENT page %d", (int)i, (int)(i + half));

    /* A range that runs off the end of the segment is refused at reservation
     * time, not left to fault forever against pages that do not exist. */
    mm_eqi((long long)vma_reserve_shm(c, 0, (SEGPAGES + 1) * 4096,
                                      VMA_READ | VMA_WRITE, sh, 0), 0,
           "a mapping longer than the segment is refused");
    mm_eqi((long long)vma_reserve_shm(c, 0, 4096, VMA_READ | VMA_WRITE,
                                      sh, SEGPAGES * 4096), 0,
           "and so is one starting past its end");
    mm_eqi((long long)vma_reserve_shm(c, 0, 4096, VMA_READ | VMA_WRITE, sh, 0x800), 0,
           "an unaligned offset is refused -- the fault path divides it by 4096");

    vmm_free_space(c);
}

/* ===================================================================== */
/* 3. THE RECLAIM INVARIANT. The dangerous one.
 *
 * Two assertions, and the second is the one that matters. The first is the
 * arithmetic shm.h rests on. The second actually RUNS reclaim at the frame and
 * requires it to still be there afterwards -- because an argument about
 * refcounts is a claim about code in another file, and the only way to know
 * reclaim agrees is to ask it. */
static void t_reclaim_refuses(uint64_t a, uint64_t b, uint64_t va, uint64_t vb)
{
    phase("a shared frame is structurally unevictable, and reclaim agrees");

    uint64_t f = frame_at(a, va);
    mm_ok(f != 0, "the frame is there to begin with");

    /* THE THREE NUMBERS. Two PTEs point at this frame, so the reverse map knows
     * two; the allocator counts those two PLUS the segment's own reference. It
     * is that ONE extra reference -- and nothing else, no pin, no exception
     * list, no change to reclaim.c -- that makes the equality fail and the
     * frame ineligible. */
    mm_eqi(rmap_count(f), 2, "the reverse map knows both mappings");
    mm_eqi((long long)pmm_refcount(f), 3,
           "and the allocator counts three: two PTEs plus the SEGMENT's own reference");
    mm_ok(pmm_refcount(f) != rmap_count(f),
          "so rmap_count != refcount -- the eligibility test fails, which is the "
          "whole mechanism");
    mm_eqi(pmm_pincount(f), 0,
           "and it is NOT pinned: the exclusion is structural, not a pin "
           "(rmap.h keeps those two mechanisms apart on purpose)");

    mm_eqi(rmap_audit(), 0, "the reverse map and the allocator agree about every frame");

    /* NOW MAKE IT MAXIMALLY TEMPTING AND RUN RECLAIM AT IT.
     *
     * Zero the page first. An all-zero page is precisely what the cheap drop
     * tier looks for, and a freshly created segment is all zeroes -- so this is
     * not a contrived state, it is the state a segment is in for the whole
     * window between creation and first write. If VMM_PTE_SHM were ever set
     * alongside VMM_PTE_ANON, or if the segment's reference went missing, this
     * is the case that would take the page away. */
    memset(mm_sim_ptr(f), 0, 4096);

    uint64_t got = reclaim_frames(64);
    printf("   reclaim was asked for 64 frames and returned %llu\n",
           (unsigned long long)got);

    mm_eqi((long long)frame_at(a, va), (long long)f,
           "after a forced reclaim pass A still maps the SAME frame");
    mm_eqi((long long)frame_at(b, vb), (long long)f,
           "and so does B -- the sharing survived");
    mm_ok(!!(pte_at(a, va) & PRESENT) && !!(pte_at(b, vb) & PRESENT),
          "neither PTE was turned into a swap entry or cleared");
    mm_eqi((long long)pmm_refcount(f), 3, "and the reference count is untouched");
    mm_eqi(rmap_audit(), 0, "the reverse map is still consistent afterwards");

    /* Put the pattern back for the cases that follow. */
    fill_pattern(mm_sim_ptr(f), 0);
}

/* ===================================================================== */
/* 4. FORK. The case the negative control exists for.
 *
 * A MAP_SHARED region survives fork as THE SAME MEMORY. The child must get the
 * parent's entry verbatim -- same frame, still writable, still SHM, NOT
 * copy-on-write -- because the moment it becomes copy-on-write the two
 * processes are one write away from never hearing from each other again.
 *
 * The private mapping alongside it is not decoration. Without it, "make fork
 * share everything" would satisfy every assertion below, and that is a much
 * worse kernel than the one this test is protecting against. The two halves of
 * this case pull in opposite directions on purpose. */
static void t_fork(int sh)
{
    phase("fork: the shared region stays shared, and the private one does not");

    uint64_t p = vmm_new_space();
    mm_ok(p != 0, "a parent address space");
    mm_host_cr3 = p;

    uint64_t vs = vma_reserve_shm(p, 0, SEGPAGES * 4096, VMA_READ | VMA_WRITE, sh, 0);
    mm_ok(vs != 0, "the parent maps the segment");

    /* A PRIVATE anonymous page in the same space, at a fixed address well away
     * from the mmap window, so both kinds go through the one clone. */
    const uint64_t vp = MM_USER_BASE + 0x100000;
    mm_eqi(vma_reserve_fixed(p, vp, 4096, VMA_READ | VMA_WRITE), 0,
           "and reserves a PRIVATE anonymous page too");

    for (int i = 0; i < SEGPAGES; i++)
        mm_eqf(mm_fault_in(p, vs + i * 4096, PF_W | PF_U), 1,
               "the parent faults shared page %d", i);
    mm_eqi(mm_fault_in(p, vp, PF_W | PF_U), 1, "and its private page");

    fill_pattern(mm_sim_ptr(frame_at(p, vs)), 0);
    fill_pattern(mm_sim_ptr(frame_at(p, vp)), 1);

    uint64_t shared_frame  = frame_at(p, vs);
    uint64_t private_frame = frame_at(p, vp);
    unsigned rc_before = pmm_refcount(shared_frame);

    /* THE FORK. */
    uint64_t ch = vmm_new_space();
    mm_ok(ch != 0, "a child address space");
    mm_eqi(vmm_clone_user(ch, p), 0, "the clone succeeded");

    /* --- the shared half: the property under test ------------------------ */
    uint64_t pc = pte_at(ch, vs);
    mm_eqi((long long)frame_at(ch, vs), (long long)shared_frame,
           "the child's shared page is THE SAME FRAME as the parent's");
    mm_ok(!!(pc & WRITABLE),
          "and it is STILL WRITABLE in the child -- not downgraded for copy-on-write");
    mm_ok(!(pc & VMM_PTE_COW),
          "and NOT marked copy-on-write, which is what would privatise it on the "
          "first write");
    mm_ok(!!(pc & VMM_PTE_SHM), "and still marked VMM_PTE_SHM");
    mm_ok(!!(pte_at(p, vs) & WRITABLE),
          "and the PARENT's entry was not downgraded either");
    mm_ok(!(pte_at(p, vs) & VMM_PTE_COW), "nor marked copy-on-write");
    mm_eqi((long long)pmm_refcount(shared_frame), (long long)rc_before + 1,
           "the frame gained exactly one reference for the child's PTE");

    /* A write on either side is seen on the other. Written through the child's
     * resolved frame and read through the parent's, so the direction that a
     * copy-on-write bug breaks FIRST is the one asserted. */
    fill_pattern(mm_sim_ptr(frame_at(ch, vs)), 2);
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(p, vs)), 2), -1,
           "the CHILD's write is visible in the PARENT, byte for byte");

    /* THE BEHAVIOURAL FORM OF THE SAME PROPERTY, and it is the one that matters.
     *
     * The two assertions above compare bytes through resolved frames, and on a
     * host there is no MMU, so they cannot make a write TAKE A FAULT -- which
     * means they still pass against a kernel whose fork marked the page
     * copy-on-write, because the copy has not happened yet. The divergence in a
     * real process begins at the first write, so the test has to reach the
     * thing that would perform it.
     *
     * mm_fault_in() is that. On a correct kernel a write to this page is not a
     * fault at all: the PTE is present and writable, the classifier finds
     * nothing to do and DECLINES, and the frame is untouched. Marked
     * copy-on-write, the very same call is served by copying -- and that copy is
     * the exact instant the two processes stop hearing from each other. */
    mm_host_cr3 = ch;
    mm_eqi(mm_fault_in(ch, vs, PF_P | PF_W | PF_U), 0,
           "a write fault on the child's shared page is DECLINED -- there is "
           "nothing to resolve, it is already writable");
    mm_eqi((long long)frame_at(ch, vs), (long long)shared_frame,
           "so the child was NOT given a private copy");
    fill_pattern(mm_sim_ptr(frame_at(ch, vs)), 6);
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(p, vs)), 6), -1,
           "and a write after that attempt still reaches the parent");

    /* --- the private half: the control against 'share everything' -------- */
    mm_eqi((long long)frame_at(ch, vp), (long long)private_frame,
           "the private page is shared for now -- that is copy-on-write, not sharing");
    mm_ok(!!(pte_at(ch, vp) & VMM_PTE_COW),
          "but it IS marked copy-on-write in the child");
    mm_ok(!!(pte_at(p, vp) & VMM_PTE_COW), "and in the parent");
    mm_ok(!(pte_at(ch, vp) & WRITABLE),
          "and NOT writable -- the next write takes a fault and privatises it");

    /* Drive that fault and confirm the two really do diverge. */
    mm_host_cr3 = ch;
    mm_eqi(mm_fault_in(ch, vp, PF_P | PF_W | PF_U), 1,
           "the child's write to the private page is served by copying");
    mm_ok(frame_at(ch, vp) != private_frame,
          "and the child now has its OWN frame for it");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(p, vp)), 1), -1,
           "the parent's private page still holds the parent's bytes");

    mm_eqi(rmap_audit(), 0, "the reverse map is consistent after the fork");

    /* The child exits. The parent must keep its shared mapping AND its bytes. */
    vmm_free_space(ch);
    mm_eqi((long long)frame_at(p, vs), (long long)shared_frame,
           "after the child exits the parent still maps the shared frame");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(p, vs)), 6), -1,
           "and the bytes the child wrote are still there");
    mm_eqi((long long)pmm_refcount(shared_frame), (long long)rc_before,
           "and the frame is back to the reference count it had before the fork");

    vmm_free_space(p);
}

/* ===================================================================== */
/* 5. LIFETIME. Unlink is not free; the last reference is.
 *
 * POSIX's rule, and the reason create-map-unlink is a safe idiom: the NAME goes
 * immediately so nobody new can join, and the OBJECT goes when the last mapping
 * does. Getting this backwards frees frames that are still in somebody's page
 * table, which is the one failure in this file that is not silent -- it is
 * memory corruption. */
static void t_lifetime(int sh, uint64_t a, uint64_t b, uint64_t va, uint64_t vb)
{
    phase("unlink removes the name; the last reference removes the memory");

    uint64_t f = frame_at(a, va);

    /* Re-establish a known pattern HERE rather than relying on what an earlier
     * case left behind. t_fork() maps this same segment and writes to it, so
     * this case reading a pattern set three cases ago was asserting on the
     * ordering of the whole file -- which is a test that breaks whenever a case
     * is added, moved or given a new pattern, and says nothing about unlink. */
    fill_pattern(mm_sim_ptr(f), 5);

    mm_eqi(shm_unlink("frames", 1000), 0, "the owner unlinks it");
    mm_eqi(shm_open("frames", 0, 0, 0, 1000), SHM_E_NOENT,
           "and it can no longer be found by name");
    mm_eqi((long long)frame_at(a, va), (long long)f,
           "but A's mapping still works -- unlink is not destruction");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(b, vb)), 5), -1,
           "and B still reads what A wrote");

    uint64_t free_before = pmm_free_frames();

    /* The handle t_table() opened is still held. Drop it: the mappings keep the
     * segment alive on their own references. */
    shm_put(sh);
    mm_ok(shm_pages(sh) != 0,
          "the segment is still alive: the two mappings hold references of their own");

    /* A takes its mapping down the way munmap does: release the reservation,
     * then unmap the pages. */
    mm_host_cr3 = a;
    mm_eqi(vma_release(a, va, SEGPAGES * 4096), 0, "A releases its reservation");
    vmm_unmap_range_in(a, va, SEGPAGES * 4096);
    mm_ok(shm_pages(sh) != 0, "the segment survives: B still has it");
    mm_eqi((long long)frame_at(b, vb), (long long)f, "and B's mapping is untouched");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(b, vb)), 5), -1,
           "with its bytes intact -- A's teardown did not free a live frame");
    mm_eqi(rmap_count(f), 1, "one mapping left, and the reverse map says so");
    mm_eqi((long long)pmm_refcount(f), 2, "one PTE plus the segment's reference");

    /* B goes away entirely, which is what exit does. */
    vmm_free_space(b);
    mm_eqi((long long)shm_pages(sh), 0,
           "the last reference is gone, so the segment is freed");
    mm_eqi(pmm_refcount(f), 0, "and its frames went back to the allocator");

    uint64_t recovered = pmm_free_frames() - free_before;
    printf("   %llu frames came back (the segment held %d)\n",
           (unsigned long long)recovered, SEGPAGES);
    mm_ok(recovered >= SEGPAGES,
          "at least the segment's own pages were recovered");

    mm_eqi(rmap_audit(), 0, "the reverse map is consistent at the end");
    mm_eqi((long long)shm_bugs(), 0, "shm reported no bugs");
}

/* ===================================================================== */
/* 6. THE UNNAMED SEGMENT -- what libc's MAP_SHARED|MAP_ANONYMOUS is built on.
 *
 * Same object, nothing to find it by. The assertion that matters is that it is
 * genuinely unreachable by name, because an unnamed segment that could be
 * guessed at would be a channel between processes that never agreed to have
 * one. */
static void t_anon_segment(void)
{
    phase("an unnamed segment: shared through fork, findable by nobody");

    int sh = shm_create_anon(2, 1000);
    mm_ok(sh >= 0, "an unnamed segment is created");
    mm_eqi((long long)shm_pages(sh), 2, "with the size asked for");
    mm_eqi(shm_open("", 0, 0, 0, 1000), SHM_E_INVAL,
           "and the empty name does not find it");

    uint64_t p = vmm_new_space();
    mm_host_cr3 = p;
    uint64_t v = vma_reserve_shm(p, 0, 2 * 4096, VMA_READ | VMA_WRITE, sh, 0);
    mm_ok(v != 0, "it maps");
    mm_eqi(mm_fault_in(p, v, PF_W | PF_U), 1, "and faults in");
    fill_pattern(mm_sim_ptr(frame_at(p, v)), 3);

    uint64_t ch = vmm_new_space();
    mm_eqi(vmm_clone_user(ch, p), 0, "fork");
    mm_eqi((long long)frame_at(ch, v), (long long)frame_at(p, v),
           "the child inherits the SAME frame");
    mm_ok(!(pte_at(ch, v) & VMM_PTE_COW), "and not as a copy-on-write copy");
    fill_pattern(mm_sim_ptr(frame_at(ch, v)), 4);
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(p, v)), 4), -1,
           "so the child's write reaches the parent");

    shm_put(sh);
    vmm_free_space(ch);
    vmm_free_space(p);
    mm_eqi((long long)shm_pages(sh), 0,
           "and it is freed when the last mapping goes -- nothing else could free it");
}

/* ===================================================================== */
int main(void)
{
    mm_sim_init(64);
    mm_sim_kernel_space();
    pmm_watch_step(1u << 20);
    shm_init();

    mm_ok(rmap_ready(), "the reverse map is up");
    reclaim_init();          /* no swap device: the drop tier only, which is
                              * exactly the tier that would take a zeroed
                              * shared page */

    int sh = t_table();
    if (sh < 0) { printf("FAIL: no segment; the rest cannot run\n"); return 1; }

    uint64_t a = 0, b = 0, va = 0, vb = 0;
    t_two_spaces(sh, &a, &b, &va, &vb);
    t_offset(sh);
    t_reclaim_refuses(a, b, va, vb);
    t_fork(sh);
    t_lifetime(sh, a, b, va, vb);
    t_anon_segment();

    mm_eqi((long long)rmap_bugs(), 0, "the reverse map reported no bugs");
    mm_eqi((long long)pmm_bugs(), 0, "the allocator reported no bugs");
    mm_eqi((long long)shm_bugs(), 0, "shm reported no bugs");

    shm_report("end of test");
    mm_sim_done();
    return mm_summary("mm_shm_test");
}
