#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "prot.h"      /* PTE_NX + cpu_prot_nx(): W^X is decided here */
#include "rng.h"       /* kernel_random_bytes(): AT_RANDOM's 16 bytes */
#include "kprintf.h"
#ifndef LOGIT_HOSTTEST
#include "vma.h"       /* VMA_READ/VMA_EXEC + the file-mapping entry point */
#else
/* tests/unit/exechost has no c/kernel/mm on its include path, and no VMAs to
 * reserve -- the weak vma_reserve_file_fixed below is absent there and the
 * loader takes the eager path. These two exist only so the prot expression
 * compiles; if they ever disagree with vma.h the host build cannot notice,
 * which is why the kernel build includes the real header instead of copying
 * the values. */
#define VMA_READ 0x1
#define VMA_EXEC 0x4
#endif

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

/* struct elf64_phdr moved to elf.h: elf_file_runs() is part of the interface
 * now, and a host test that enumerates the predicate needs the type. */

/* e_ident indices + the values this machine accepts. */
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ELFOSABI_SYSV 0
#define ELFOSABI_GNU  3

#define ET_EXEC  2
#define ET_DYN   3
#define EM_X86_64 62

#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6
#define PT_TLS      7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552
#define PT_GNU_PROPERTY 0x6474e553

#define ELF_MAX_PHNUM 64

/* The architectural address field of a page-table entry is bits 12..51. The
 * mask this file used to use -- ~0xFFF -- clears the flag bits and KEEPS bit
 * 63, so an NX page read back through it is a physical address eight exabytes
 * up. That mask is still wrong in ~63 places under c/kernel/mm (see prot.h);
 * it is right HERE, which is why NX being inert is a report and not a bug in
 * this file. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* ---------------------------------------------------------------------------
 * W^X
 *
 * Every PT_LOAD used to be mapped VMM_WRITABLE | VMM_USER, whatever p_flags
 * said. With no NX bit either, that made every page of every process both
 * writable and executable: a program could overwrite its own code, and any
 * buffer it could overflow was somewhere it could then jump. Both halves of
 * "overwrite a buffer, jump into it" were open, and the ELF had been carrying
 * the information needed to close them the whole time.
 *
 * The mapping is now what p_flags asks for. Three details make that harder than
 * setting a flag in the existing loop:
 *
 * 1. THE COPY NEEDS WRITE. The kernel memcpy()s file bytes to the user virtual
 *    address, so the page has to be writable while that happens -- a read-only
 *    text page cannot be written even from ring 0 once CR0.WP is on, and this
 *    kernel takes a ring-0 fault as fatal. So: map writable, copy, then a
 *    SECOND pass applies the real protection. The window is closed before the
 *    program's first instruction runs and never overlaps ring 3.
 *
 * 2. SEGMENTS CAN SHARE A PAGE. p_vaddr is page-aligned only up to p_align, and
 *    .rodata's last page can be .data's first. A page's permission must be the
 *    UNION of every segment that lands on it (page_prot below), because giving
 *    it the last segment's permissions would take write away from real data or
 *    hand execute to real data depending on link order. That also means a
 *    shared page must be allocated ONCE: the old loop allocated per segment, so
 *    two segments sharing a page would have mapped a second frame over the
 *    first and silently discarded the bytes already copied into it. That bug
 *    was latent -- lld happens to page-separate these segments -- and is fixed
 *    here rather than left to be found by the first link that does not.
 *
 * 3. NX IS NOT AVAILABLE YET, AND THAT IS NOT ABOUT THE CPU. Bit 63 is
 *    reserved unless EFER.NXE is set -- it is set, on every core (prot.c) --
 *    but c/kernel/mm converts page-table entries back to physical frames with
 *    `e & ~(uint64_t)0xFFF`, a mask that clears the flag bits and KEEPS bit 63.
 *    An NX page therefore reaches pmm_ref()/pmm_free()/pmm_refcount() as the
 *    physical address 0x8000000000nnnnnn. Measured, not predicted: with NX set
 *    this machine boots the desktop and dies on a kernel #GP in memcpy the
 *    first time /bin/sh fork+execs. So cpu_prot_nx_usable() returns 0 and this
 *    loader leaves bit 63 clear; see prot.h for the mm-side change that lifts
 *    it. The read-only half of W^X needs no high PTE bits and is ON regardless,
 *    which is why map_flags() still runs and text is still unwritable.
 *
 * What this does NOT do: relocate, or support a program that wants writable
 * executable memory. A PT_LOAD carrying both PF_W and PF_X is REFUSED rather
 * than honoured. Linux would map it RWX and let the binary opt out of W^X;
 * refusing is the stronger rule and costs nothing here, because every program
 * this system builds links R / R+X / R+W and not one of them is RWX (checked
 * across all 57 built ELFs). The day a JIT needs RWX it will say so by failing
 * to load, with a message naming the segment, which is a better conversation
 * than a silently re-opened boundary.
 * ------------------------------------------------------------------------ */

/* One refusal, one line, one code. Every `return ELF_E_*` in this file goes
 * through here so a refusal can never be silent -- the loader's whole job on
 * bad input is to say which field was bad. */
static int reject(int code, const char *what, uint64_t a, uint64_t b)
{
    kprintf("[elf] refused (%d): %s (%p, %p)\n", code, what, (void *)a, (void *)b);
    return code;
}

/* The permission bits for one page, unioned across every PT_LOAD that covers
 * it. Returns a p_flags-shaped value (PF_R/PF_W/PF_X), or 0 if no segment
 * covers the page. O(phnum) per page with phnum <= 64. */
static uint32_t page_prot(const struct elf64_phdr *ph, int phnum, uint64_t page)
{
    uint32_t prot = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (page >= start && page < end) prot |= ph[i].p_flags;
    }
    return prot;
}

/* p_flags -> the flags vmm_map_page() wants. USER always (this is ring-3
 * memory); WRITABLE only if the segment asks; NX unless the segment is
 * executable AND the CPU can express it. */
static uint64_t map_flags(uint32_t prot, int nx_on)
{
#ifdef ELF_NEGCTL_NOWX
    /* The other NEGATIVE CONTROL (tests/exec.mk): map every page of every
     * segment writable and executable, whatever p_flags says -- the loader
     * exactly as it was before 37a0849. test-exec-negctl requires the unit
     * test to FAIL, and to fail on the per-segment permission checks rather
     * than on anything else, because a test that measures W^X has to be able
     * to tell a loader that honours p_flags from one that does not. */
    (void)prot; (void)nx_on;
    return VMM_USER | VMM_WRITABLE;
#else
    uint64_t f = VMM_USER;
    if (prot & PF_W) f |= VMM_WRITABLE;
    if (!(prot & PF_X) && nx_on) f |= PTE_NX;
    return f;
#endif
}

static uint64_t read_cr3(void)
{
    /* elf_load runs with the TARGET space active, which is not always the
     * scheduler's current thread's space (proc_spawn switches CR3 by hand
     * around the load), so the register is the only correct source. */
#ifdef LOGIT_HOSTTEST
    /* tests/unit/exechost: the host harness has ONE address space and cannot
     * read CR3 from the ring it is in. This is the ONLY privileged instruction
     * in the file and therefore the only thing the host test stubs --
     * everything else it runs is the kernel's code, byte for byte. */
    return 0;
#else
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v & PTE_ADDR_MASK;
#endif
}

static int is_pow2(uint64_t v) { return v && (v & (v - 1)) == 0; }

/* ------------------------------------------------------- FILE-BACKED TEXT --
 * The predicate elf.h states in full. Pure by construction: it reads program
 * headers and returns runs, and the two things that could make it lie -- the
 * page table and the allocator -- are not reachable from here.
 *
 * The trims are written as loops over the two ENDS rather than a per-page
 * filter because PT_LOADs are ascending and non-overlapping (PASS 0 refuses
 * anything else), so a segment's eligible pages are ONE contiguous run and
 * condition (a) can only remove pages at its edges. A per-page filter would
 * produce the same answer for every real link and a set of disjoint runs for a
 * malformed one -- more VMAs than there are slots, from disk-controlled input. */
static int seg_count_at(const struct elf64_phdr *ph, int phnum, uint64_t page)
{
    int n = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz > (uint64_t)-1 - ph[i].p_vaddr) continue;   /* PASS 0 refuses it */
        uint64_t s = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t v = ph[i].p_vaddr + ph[i].p_memsz;
        if (v > (uint64_t)-1 - 0xFFF) continue;
        uint64_t e = (v + 0xFFF) & ~(uint64_t)0xFFF;
        if (page >= s && page < e) n++;
    }
    return n;
}

int elf_file_runs(const struct elf64_phdr *ph, int phnum, uint64_t hdr_off,
                  uint64_t file_pages, struct elf_run *out, int max)
{
    int n = 0;
    if (!ph || phnum <= 0 || !out || max <= 0) return 0;
    if (hdr_off & 0xFFF) return 0;                     /* (d), the cheap half */

    for (int i = 0; i < phnum && n < max; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_flags & PF_W) continue;            /* (b) */
        if (ph[i].p_filesz > ph[i].p_memsz) continue;  /* PASS 0 refuses it */
        if (ph[i].p_filesz < 0x1000) continue;         /* no whole page to share */
        if (ph[i].p_filesz > (uint64_t)-1 - ph[i].p_vaddr) continue;

        /* (c): whole pages strictly inside [p_vaddr, p_vaddr + p_filesz). */
        uint64_t lo = (ph[i].p_vaddr + 0xFFF) & ~(uint64_t)0xFFF;
#ifdef ELF_NEGCTL_FILETAIL
        /* THE NEGATIVE CONTROL (tests/exec.mk), and it is the PLAUSIBLE wrong
         * version rather than the feature switched off: round the end of the
         * run UP instead of down, so the run covers the whole segment "as far
         * as the file goes". Every page it maps really is in the file and
         * really does hold the right bytes, so the program still starts, its
         * text is still correct, and the permissions are still right -- what
         * changes is the p_filesz < p_memsz BOUNDARY page, which is part file
         * and part .bss. Mapped from the cache it arrives holding the file's
         * next bytes where zeroes belong, shared with every other process
         * running the same binary. That is R3, and it is a silent wrong
         * answer: the failure is a global that was never zero, days later,
         * somewhere else. exec_test's elf_file_runs cases must FAIL against
         * this build. */
        uint64_t hi = (ph[i].p_vaddr + ph[i].p_filesz + 0xFFF) & ~(uint64_t)0xFFF;
#else
        uint64_t hi = (ph[i].p_vaddr + ph[i].p_filesz) & ~(uint64_t)0xFFF;
#endif
        if (lo >= hi) continue;

        /* (a). */
        while (lo < hi && seg_count_at(ph, phnum, lo) > 1) lo += 0x1000;
        while (hi > lo && seg_count_at(ph, phnum, hi - 0x1000) > 1) hi -= 0x1000;
        if (lo >= hi) continue;

        /* The private user region, because a run outside it is not a run: PASS
         * 1 SKIPS the low read-only headers segment some lld versions emit at
         * 0x200000 (mapping it with USER would open a window into the shared
         * kernel page tables), and vma_reserve_file_fixed refuses the address
         * anyway. Returning it would be a run that is always dropped, which
         * costs a "could not be reserved" line on every load of every binary
         * that has one -- a warning that means nothing, printed forever. */
        if (lo < USER_VA_BASE || hi > USER_VA_END) continue;

        /* (d) in full. Redundant with the hdr_off test above for any image
         * PASS 0 accepted (p_offset == p_vaddr mod 4096) and kept because the
         * predicate is called on unvalidated headers by the host test, where
         * "the congruence check upstream guarantees it" is not true. */
        uint64_t foff = hdr_off + ph[i].p_offset + (lo - ph[i].p_vaddr);
        if (foff & 0xFFF) continue;
        if (foff < hdr_off) continue;                  /* wrapped */

        uint64_t pages = (hi - lo) >> 12;
        uint64_t first = foff >> 12;
        /* (e). TRIMMED, not refused: a run that runs off the end of the file
         * is still a legitimate run for the pages that ARE in the file, and
         * losing the whole segment to a short tail would silently un-share a
         * binary for a reason nobody could see from outside. */
        if (first >= file_pages) continue;
        if (file_pages - first < pages) pages = file_pages - first;
        if (!pages) continue;

        out[n].va = lo;
        out[n].foff = foff;
        out[n].pages = pages;
        out[n].prot = ph[i].p_flags & (uint32_t)(PF_R | PF_X);
        n++;
    }
    return n;
}

/* Is `va` inside one of the runs? The loop is over at most ELF_MAX_RUNS
 * entries and runs once per page of the image, which is 870 x 2 comparisons
 * for the biggest binary here -- cheaper than building a per-page bitmap that
 * would have to be allocated somewhere. */
static int in_runs(const struct elf_run *runs, int nrun, uint64_t va)
{
    for (int i = 0; i < nrun; i++)
        if (va >= runs[i].va && va < runs[i].va + (runs[i].pages << 12)) return 1;
    return 0;
}

/* c/kernel/mm's fixed-address file mapping. WEAK on purpose, exactly as vmm.c
 * declares tlb_flush_all: tests/unit/exechost links elf.c against a simulated
 * MMU with no VMAs and no page cache, so the symbol is absent there, the call
 * is skipped, and the host tests go on measuring the eager loader byte for
 * byte. A hard reference would make them a link error instead. */
int vma_reserve_file_fixed(uint64_t cr3, uint64_t start, uint64_t len,
                           uint32_t prot, int fh, uint64_t foff) __attribute__((weak));

/* Allocate + map one zeroed user page at `va`. Returns 0 on OOM, 1 if a page
 * was already there, 2 if this call allocated one. Skips the allocation if
 * something is already mapped there (two PT_LOADs sharing a page: allocating
 * again would map a second frame over the first and throw away the bytes
 * already copied into it).
 *
 * The 1-vs-2 distinction is new and is not decoration: it is what makes
 * out->copied_pages a count of FRAMES rather than of loop iterations, and a
 * page reached from two segments would otherwise be counted twice in the
 * number a gate reads to decide whether file-backing did anything. */
static int place_page(uint64_t cr3, uint64_t va, uint64_t flags)
{
    uint64_t *e = vmm_pte(cr3, va);
    if (e && (*e & 1)) return 1;                  /* PRESENT: already ours */
    uint64_t frame = pmm_alloc();
    if (!frame) return 0;
    memset((void *)frame, 0, 0x1000);
    vmm_map_page(va, frame, flags);
    return 2;
}

/* ---------------------------------------------------------------------------
 * THE DELIBERATE NOs.
 *
 * Each of these is a program header this loader understands well enough to
 * REFUSE, which is a different and more useful state than not knowing about it.
 * Written down here so the next person does not have to re-derive whether the
 * absence is a decision:
 *
 *   PT_INTERP   -- refused. It names a dynamic loader (/lib/ld-linux...). There
 *                  is none on this machine and no plan for one; mapping the
 *                  program and jumping to its entry anyway would run a binary
 *                  whose PLT has never been filled in, and the first library
 *                  call would jump to zero. A refusal names the interpreter.
 *   PT_DYNAMIC  -- refused. It is the relocation/symbol table, and this loader
 *                  applies no relocations. A static ET_EXEC never has one; a
 *                  binary that does has expectations we would silently break.
 *   ET_DYN      -- refused, same reason, at the file level: a PIE is *defined*
 *                  by needing R_X86_64_RELATIVE applied at its load base.
 *                  Refusing is what makes the fixed link bases honest rather
 *                  than accidental.
 *   PT_SHLIB    -- refused. The ELF spec reserves it and says its semantics are
 *                  unspecified; nothing should ever emit one.
 *   PT_GNU_PROPERTY / NT_GNU_PROPERTY_TYPE_0 -- IGNORED, on purpose. It carries
 *                  the x86 CET feature bits (IBT/SHSTK). This machine sets
 *                  neither CR4.CET nor the MSRs, so honouring the property
 *                  would be a lie and refusing it would reject every binary a
 *                  modern toolchain produces. Recorded in seen[] so a future
 *                  CET line can find every image that asked.
 *   PT_GNU_EH_FRAME -- IGNORED by the kernel, which is correct: it is a pointer
 *                  to .eh_frame_hdr that only a userland unwinder reads, and it
 *                  reads it through the program-header table, which this loader
 *                  now actually puts in memory (AT_PHDR). Bounds-checked, since
 *                  an unwinder will dereference it.
 *   PT_NOTE     -- IGNORED (informational), bounds-checked.
 *   Section headers -- NOT read. Loading an ELF is defined entirely by the
 *                  program headers; sections are the linker's view. They are
 *                  bounds-checked so a malformed table is refused rather than
 *                  ignored, and then not consulted. This is the one "absent"
 *                  entry in the survey that was already correct.
 *   Relocations -- none, of any kind. See ET_DYN.
 * ------------------------------------------------------------------------ */

int elf_load_image(void *image, uint64_t image_size, struct elf_image *out)
{
    return elf_load_image_ex(image, image_size, out, 0);
}

int elf_load_image_ex(void *image, uint64_t image_size, struct elf_image *out,
                      const struct elf_src *src)
{
    struct elf_image blank;
    if (!out) out = &blank;
    memset(out, 0, sizeof *out);
    out->stack_flags = PF_R | PF_W;      /* the default when no PT_GNU_STACK */

    struct elf64_ehdr *eh = image;

    /* --- the identification header ------------------------------------- */
    if (image_size < sizeof *eh)
        return reject(ELF_E_SHORT, "image shorter than an ELF64 header", image_size, sizeof *eh);
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return reject(ELF_E_MAGIC, "bad ELF magic", eh->e_ident[0], eh->e_ident[1]);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)
        return reject(ELF_E_CLASS, "not ELFCLASS64", eh->e_ident[EI_CLASS], 0);
    /* Endianness. Reading a big-endian ELF with little-endian loads does not
     * fail anywhere -- it silently produces enormous offsets and sizes, all of
     * which the bounds checks below would reject one at a time with a confusing
     * message. One check at the top says the real thing. */
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return reject(ELF_E_DATA, "not ELFDATA2LSB (big-endian ELF)", eh->e_ident[EI_DATA], 0);
    if (eh->e_ident[EI_VERSION] != EV_CURRENT)
        return reject(ELF_E_IDENT, "EI_VERSION != 1", eh->e_ident[EI_VERSION], 0);
    if (eh->e_ident[EI_OSABI] != ELFOSABI_SYSV && eh->e_ident[EI_OSABI] != ELFOSABI_GNU)
        return reject(ELF_E_IDENT, "EI_OSABI is neither SysV nor GNU", eh->e_ident[EI_OSABI], 0);
    if (eh->e_ident[EI_ABIVERSION] != 0)
        return reject(ELF_E_IDENT, "EI_ABIVERSION != 0", eh->e_ident[EI_ABIVERSION], 0);

    if (eh->e_type == ET_DYN)
        return reject(ELF_E_TYPE, "ET_DYN: a PIE needs relocation, and this "
                                  "loader applies none", eh->e_type, 0);
    if (eh->e_type != ET_EXEC)
        return reject(ELF_E_TYPE, "not ET_EXEC", eh->e_type, 0);
    if (eh->e_machine != EM_X86_64)
        return reject(ELF_E_MACHINE, "not EM_X86_64", eh->e_machine, 0);
    if (eh->e_version != EV_CURRENT)
        return reject(ELF_E_VERSION, "e_version != EV_CURRENT", eh->e_version, 0);
    if (eh->e_ehsize != sizeof *eh)
        return reject(ELF_E_EHSIZE, "e_ehsize is not 64", eh->e_ehsize, sizeof *eh);
    /* e_flags: the x86-64 psABI defines no processor-specific flags, so the
     * only correct value is 0. A nonzero one means the file was built for a
     * different processor supplement than the one we implement. */
    if (eh->e_flags != 0)
        return reject(ELF_E_FLAGS, "e_flags nonzero (no x86-64 flags exist)", eh->e_flags, 0);

    /* --- the program-header table --------------------------------------- */
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return reject(ELF_E_EHSIZE, "e_phentsize is not 56", eh->e_phentsize, 56);
    if (eh->e_phnum == 0)
        return reject(ELF_E_PHNUM, "e_phnum is 0: nothing to load", 0, 0);
    if (eh->e_phnum > ELF_MAX_PHNUM)                     /* also catches PN_XNUM */
        return reject(ELF_E_PHNUM, "e_phnum past the cap", eh->e_phnum, ELF_MAX_PHNUM);
    /* Disk-controlled: bound it to the image before dereferencing (subtraction
     * form avoids overflow in the add). */
    if (eh->e_phoff > image_size ||
        (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > image_size - eh->e_phoff)
        return reject(ELF_E_PHTAB, "program-header table outside the image",
                      eh->e_phoff, image_size);

    /* --- the section-header table: bounded, then not read ---------------- */
    if (eh->e_shnum) {
        if (eh->e_shentsize != 64)
            return reject(ELF_E_EHSIZE, "e_shentsize is not 64", eh->e_shentsize, 64);
        if (eh->e_shoff > image_size ||
            (uint64_t)eh->e_shnum * 64ull > image_size - eh->e_shoff)
            return reject(ELF_E_SHTAB, "section-header table outside the image",
                          eh->e_shoff, image_size);
        if (eh->e_shstrndx != 0 && eh->e_shstrndx >= eh->e_shnum)
            return reject(ELF_E_SHTAB, "e_shstrndx past e_shnum", eh->e_shstrndx, eh->e_shnum);
    } else if (eh->e_shoff || eh->e_shentsize || eh->e_shstrndx) {
        return reject(ELF_E_SHTAB, "e_shnum is 0 but the other section fields are not",
                      eh->e_shoff, eh->e_shentsize);
    }

    /* The entry point must land in the user region too, with 64 MiB of headroom
     * above it: setup_cli_stack() maps the CLI stack at (entry & ~0xFFFFF) +
     * 0x4000000. (Subtraction form: no overflow for a huge e_entry.) */
    if (eh->e_entry < USER_VA_BASE || eh->e_entry > USER_VA_END - 0x4000000)
        return reject(ELF_E_ENTRY, "entry point outside the user region",
                      eh->e_entry, USER_VA_BASE);

    struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)image + eh->e_phoff);
    int phnum = eh->e_phnum;

    /* ================= PASS 0: validate every program header ==============
     * Nothing is mapped until this pass has passed, so a refusal leaves no
     * half-built address space behind -- the property the W^X check already
     * had, extended to everything else. */
    const struct elf64_phdr *tls = 0, *relro = 0, *phdrseg = 0;
    uint64_t prev_end = 0;
    int seen_load = 0;
    uint64_t total_bytes = 0;

    for (int i = 0; i < phnum; i++) {
        uint32_t t = ph[i].p_type;

        /* Every header that names file bytes must name bytes IN the file --
         * checked once, here, for all of them, rather than per-type. */
        if (t != PT_NULL && t != PT_GNU_STACK) {
            if (ph[i].p_offset > image_size ||
                ph[i].p_filesz > image_size - ph[i].p_offset)
                return reject(ELF_E_SEGRANGE, "segment file range outside the image",
                              ph[i].p_offset, ph[i].p_filesz);
        }
        if (ph[i].p_align > 1 && !is_pow2(ph[i].p_align))
            return reject(ELF_E_SEGALIGN, "p_align is not a power of two", i, ph[i].p_align);

        switch (t) {
        case PT_NULL:
            break;

        case PT_LOAD: {
            if (ph[i].p_filesz > ph[i].p_memsz)
                return reject(ELF_E_SEGRANGE, "p_filesz > p_memsz", ph[i].p_filesz, ph[i].p_memsz);
            /* ELF requires p_offset == p_vaddr modulo the page size. Our
             * memcpy-to-vaddr load does not depend on it, which is exactly why
             * it is worth checking: an image that violates it was produced by
             * something that is not a linker, and everything else it says is
             * then suspect. */
            if (ph[i].p_align >= 0x1000 &&
                (ph[i].p_offset & 0xFFF) != (ph[i].p_vaddr & 0xFFF))
                return reject(ELF_E_SEGALIGN, "p_offset not congruent to p_vaddr mod page",
                              ph[i].p_offset, ph[i].p_vaddr);
            /* THE OVERFLOW, and why the obvious check is not enough.
             *
             * This was `end = (vaddr + memsz + 0xFFF) & ~0xFFF; if (end < start)
             * reject`, which is right for almost every wrapping value and wrong
             * for the one the fuzzer found in a few thousand iterations:
             * p_vaddr = 0x50009048, p_memsz = 0xFFFFFFFFFFFFF000. That is
             * vaddr - 0x1000 modulo 2^64, so end rounds to 0x50009000, which is
             * EXACTLY start -- not less than it. The range check then passes
             * (both ends are inside the user region), the mapping loop
             * `for (a = start; a < end; ...)` runs zero times, and the memcpy
             * below writes p_filesz bytes to an address nothing mapped. In the
             * kernel that is a ring-0 write through a user pointer with no page
             * behind it, from a file on disk.
             *
             * So the overflow is tested BEFORE the arithmetic that would wrap,
             * in subtraction form -- the same shape the program-header bound
             * above was already written in, for the same reason. */
#ifdef ELF_NEGCTL_OVERFLOW
            /* The NEGATIVE CONTROL (tests/exec.mk): the check exactly as it was
             * written before the fuzzer got to it. test-exec-negctl requires
             * the unit test to FAIL on the named case above, and requires the
             * FUZZER to crash -- if either still passes, neither of them is
             * measuring this fix. */
            uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
            uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
            if (end < start)
                return reject(ELF_E_SEGVA, "p_vaddr + p_memsz overflows",
                              ph[i].p_vaddr, ph[i].p_memsz);
#else
            if (ph[i].p_memsz > (uint64_t)-1 - ph[i].p_vaddr)
                return reject(ELF_E_SEGVA, "p_vaddr + p_memsz overflows", ph[i].p_vaddr, ph[i].p_memsz);
            uint64_t vend = ph[i].p_vaddr + ph[i].p_memsz;
            if (vend > (uint64_t)-1 - 0xFFF)
                return reject(ELF_E_SEGVA, "p_vaddr + p_memsz overflows when page-rounded",
                              ph[i].p_vaddr, ph[i].p_memsz);
            uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
            uint64_t end   = (vend + 0xFFF) & ~(uint64_t)0xFFF;
#endif
            /* ELF requires PT_LOADs in ascending p_vaddr with no overlap. The
             * loader tolerated any order before, and two segments describing
             * the same bytes would have produced whichever copy ran last. */
            if (seen_load && ph[i].p_vaddr < prev_end)
                return reject(ELF_E_SEGORDER, "PT_LOADs overlap or are not ascending",
                              ph[i].p_vaddr, prev_end);
            prev_end = ph[i].p_vaddr + ph[i].p_memsz;
            seen_load = 1;

            /* Some lld versions emit a read-only headers PT_LOAD at a low fixed
             * VA (0x200000). It is unused at runtime, and mapping it into the
             * SHARED low region with USER would hand ring-3 a window over
             * kernel memory -- skip it instead of mapping. Anything straddling
             * or above the user region boundary is still rejected outright. */
            if (end <= USER_VA_BASE) break;
            if (start < USER_VA_BASE || end > USER_VA_END)
                return reject(ELF_E_SEGVA, "segment outside the user region", start, end);

#ifndef ELF_NEGCTL_NOWX
            if ((ph[i].p_flags & PF_W) && (ph[i].p_flags & PF_X))
                return reject(ELF_E_WX, "segment is writable AND executable (W^X)",
                              ph[i].p_vaddr, ph[i].p_memsz);
#endif

            total_bytes += end - start;
            if (total_bytes > ELF_MAX_IMAGE_BYTES)
                return reject(ELF_E_TOOBIG, "image asks for more memory than the cap",
                              total_bytes, ELF_MAX_IMAGE_BYTES);
            out->nload++;
            if (!out->load_base || start < out->load_base) out->load_base = start;
            if (end > out->top) out->top = end;
            break;
        }

        case PT_PHDR:
            /* PT_PHDR describes the program-header table's own location in the
             * memory image. It is only meaningful if a PT_LOAD covers it; a
             * PT_PHDR pointing at an unmapped address is what our own links
             * produce (0x200040, in the skipped low segment) and is exactly why
             * AT_PHDR needs the copy this loader makes. */
            out->seen |= ELF_SEEN_PHDR;
            phdrseg = &ph[i];
            break;

        case PT_INTERP:
            out->seen |= ELF_SEEN_INTERP;
            return reject(ELF_E_INTERP, "PT_INTERP: this system has no dynamic loader",
                          ph[i].p_offset, ph[i].p_filesz);

        case PT_DYNAMIC:
            out->seen |= ELF_SEEN_DYNAMIC;
            return reject(ELF_E_DYNAMIC, "PT_DYNAMIC: this loader applies no relocations",
                          ph[i].p_vaddr, ph[i].p_memsz);

        case PT_SHLIB:
            return reject(ELF_E_DYNAMIC, "PT_SHLIB: reserved by the ELF spec, "
                                         "semantics unspecified", i, 0);

        case PT_NOTE:
            out->seen |= ELF_SEEN_NOTE;
            break;

        case PT_TLS:
            out->seen |= ELF_SEEN_TLS;
            if (tls)
                return reject(ELF_E_TLS, "more than one PT_TLS", i, 0);
            if (ph[i].p_filesz > ph[i].p_memsz)
                return reject(ELF_E_TLS, "PT_TLS p_filesz > p_memsz", ph[i].p_filesz, ph[i].p_memsz);
            if (ph[i].p_memsz > 0x100000)
                return reject(ELF_E_TLS, "PT_TLS block larger than 1 MiB", ph[i].p_memsz, 0x100000);
            if (ph[i].p_align > 0x1000)
                return reject(ELF_E_TLS, "PT_TLS alignment larger than a page", ph[i].p_align, 0x1000);
            tls = &ph[i];
            break;

        case PT_GNU_STACK:
            out->seen |= ELF_SEEN_GNU_STACK;
            /* THE POINT of implementing this header: until now exec.c and wm.c
             * each decided stack executability on their own, and the file's
             * answer was never consulted. An executable stack is refused rather
             * than honoured, for the same reason a PF_W|PF_X PT_LOAD is: it is
             * the exact shape W^X exists to close, and nothing this tree builds
             * asks for it (all 57 carry PF_R|PF_W here). */
            if (ph[i].p_flags & PF_X)
                return reject(ELF_E_STACKX, "PT_GNU_STACK asks for an executable stack",
                              ph[i].p_flags, 0);
            out->stack_flags = ph[i].p_flags & (PF_R | PF_W | PF_X);
            if (!out->stack_flags) out->stack_flags = PF_R | PF_W;
            break;

        case PT_GNU_RELRO:
            out->seen |= ELF_SEEN_GNU_RELRO;
            if (relro)
                return reject(ELF_E_SEGRANGE, "more than one PT_GNU_RELRO", i, 0);
            if (ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr)
                return reject(ELF_E_SEGRANGE, "PT_GNU_RELRO range overflows",
                              ph[i].p_vaddr, ph[i].p_memsz);
            relro = &ph[i];
            break;

        case PT_GNU_EH_FRAME:
            out->seen |= ELF_SEEN_GNU_EH;
            break;

        case PT_GNU_PROPERTY:
            out->seen |= ELF_SEEN_GNU_PROP;
            break;

        default:
            /* The ELF rule for an unrecognised p_type is to ignore it. Recorded
             * so "the loader saw something it did not understand" is visible to
             * a test instead of being indistinguishable from a clean file. */
            out->seen |= ELF_SEEN_UNKNOWN;
            break;
        }
    }

    if (!out->nload)
        return reject(ELF_E_NOLOAD, "no PT_LOAD lands in the user region", 0, 0);
    /* The entry point has to be inside something that was actually mapped.
     * Without this an image can pass every range check and still hand ring 3 a
     * jump to an unmapped page, which arrives as an application fault with no
     * explanation. */
    {
        int covered = 0;
        for (int i = 0; i < phnum && !covered; i++) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (!(ph[i].p_flags & PF_X)) continue;
            if (eh->e_entry >= ph[i].p_vaddr && eh->e_entry < ph[i].p_vaddr + ph[i].p_memsz)
                covered = 1;
        }
        if (!covered)
            return reject(ELF_E_ENTRY, "entry point is in no executable PT_LOAD",
                          eh->e_entry, 0);
    }
    /* Room above the image for the loader's own two mappings (the info page and
     * any TLS block), which the caller then places its stack above. */
    if (out->top > USER_VA_END - 0x200000)
        return reject(ELF_E_SEGVA, "no room above the image for the info page",
                      out->top, USER_VA_END);

    int nx_on = cpu_prot_nx_usable();
    uint64_t cr3 = read_cr3();

    /* ================= PASS 0.5: the file-backed runs =====================
     * Before a single frame is allocated, because the whole point is not to
     * allocate them. Each run becomes a VMA over the page cache; PASS 1 then
     * skips those pages and PASS 2 finds them absent and leaves them alone.
     * They arrive on first touch through fault.c's do_file(), read-only, with
     * NX from the VMA's prot -- the same protection map_flags() would have
     * given them, arrived at by the other route.
     *
     * A run that cannot be reserved (no VMA slot, the range already claimed)
     * is DROPPED, not fatal: PASS 1 then maps and copies it exactly as before.
     * That is the only failure mode this feature is allowed to have, because
     * the eager path is still right -- it is merely more expensive. */
    struct elf_run runs[ELF_MAX_RUNS];
    int nrun = 0;
    if (src && src->fh >= 0 && vma_reserve_file_fixed) {
        int want = elf_file_runs(ph, phnum, src->base_off, src->file_pages,
                                 runs, ELF_MAX_RUNS);
        int kept = 0;
        for (int i = 0; i < want; i++) {
            uint32_t vprot = VMA_READ | ((runs[i].prot & PF_X) ? VMA_EXEC : 0);
            if (vma_reserve_file_fixed(cr3, runs[i].va, runs[i].pages << 12,
                                       vprot, src->fh, runs[i].foff) == 0) {
                runs[kept++] = runs[i];
                out->file_pages += (uint32_t)runs[i].pages;
            }
        }
        nrun = kept;
        out->file_runs = (uint32_t)nrun;
        if (kept < want)
            kprintf("[elf] %d of %d file-backed runs could not be reserved; "
                    "those pages are being copied\n", want - kept, want);
    }

    /* ================= PASS 1: map + copy ================================ */
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end <= USER_VA_BASE) continue;          /* the skipped headers segment */

        for (uint64_t a = start; a < end; a += 0x1000) {
            if (in_runs(runs, nrun, a)) continue;   /* the page cache owns it */
            /* Writable for now, whatever the segment says -- the memcpy below
             * needs it. Pass 2 takes it away again. */
            int pr = place_page(cr3, a, VMM_WRITABLE | VMM_USER);
            if (!pr)
                return reject(ELF_E_OOM, "out of physical frames mapping a segment", a, i);
            if (pr == 2) out->copied_pages++;
        }

        /* The bytes. With no run this is the one memcpy it always was; with a
         * run it is at most two, and the run's own pages are deliberately NOT
         * written -- they are the file, already, and writing them would fault
         * (they are absent) or, worse, would dirty a shared page. */
        uint64_t fs_start = ph[i].p_vaddr, fs_end = ph[i].p_vaddr + ph[i].p_filesz;
        const struct elf_run *r = 0;
        for (int k = 0; k < nrun; k++)
            if (runs[k].va >= start && runs[k].va < end) { r = &runs[k]; break; }
        if (!r) {
            memcpy((void *)fs_start, (uint8_t *)image + ph[i].p_offset, ph[i].p_filesz);
        } else {
            uint64_t rlo = r->va, rhi = r->va + (r->pages << 12);
            if (rlo > fs_start)
                memcpy((void *)fs_start, (uint8_t *)image + ph[i].p_offset,
                       rlo - fs_start);
            if (fs_end > rhi)
                memcpy((void *)rhi,
                       (uint8_t *)image + ph[i].p_offset + (rhi - fs_start),
                       fs_end - rhi);
        }
    }

    /* ================= PASS 2: the real protections ======================
     * Now that every byte is in place. Re-mapping the same frame through
     * vmm_map_page() rather than editing the PTE by hand keeps this on the
     * public interface and gets the invlpg for free.
     *
     * Driven off the phdrs again (not a page list) because a program's .bss can
     * be tens of megabytes -- /bin/as reserves 25 MiB -- and a per-page record
     * of a load that size is a bigger allocation than the load. page_prot()
     * unions the segments, so a page reached from two of them gets the same
     * answer either time and the order of this loop does not matter. */
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end <= USER_VA_BASE) continue;
        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t *e = vmm_pte(cr3, a);
            if (!e || !(*e & 1)) continue;             /* not present: nothing to protect */
            vmm_map_page(a, *e & PTE_ADDR_MASK,
                         map_flags(page_prot(ph, phnum, a), nx_on));
        }
    }

    /* ================= PASS 3: PT_GNU_RELRO ==============================
     * The segment names the part of the writable image that is only written
     * while the program is being STARTED -- .data.rel.ro, .init_array, the GOT.
     * With relocation there would be a relocate-then-protect ordering to get
     * right; with none, everything in it was written by the memcpy above and is
     * finished, so this is simply "take write away again".
     *
     * Rounding: the region's start is not page-aligned (lld emits
     * 0x4529c4f8/0x2b08 for the browser), and a page is either writable or not.
     * Rounding the start DOWN is what a dynamic loader does, and it is right
     * here too -- but only after checking that the page it swallows holds no
     * byte of some OTHER writable segment. Round up instead of taking write
     * away from real .data. */
    if (relro) {
        uint64_t rs = relro->p_vaddr, re = relro->p_vaddr + relro->p_memsz;
        uint64_t first = rs & ~(uint64_t)0xFFF, last = re & ~(uint64_t)0xFFF;
        for (uint64_t a = first; a < last; a += 0x1000) {
            int conflict = 0;
            for (int i = 0; i < phnum && !conflict; i++) {
                if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_W)) continue;
                uint64_t ls = ph[i].p_vaddr, le = ph[i].p_vaddr + ph[i].p_memsz;
                uint64_t is = ls > a ? ls : a, ie = le < a + 0x1000 ? le : a + 0x1000;
                if (is >= ie) continue;               /* this segment misses the page */
                if (is < rs || ie > re) conflict = 1; /* ... and pokes outside relro */
            }
            if (conflict) continue;
            uint64_t *e = vmm_pte(cr3, a);
            if (!e || !(*e & 1)) continue;
            vmm_map_page(a, *e & PTE_ADDR_MASK,
                         map_flags(page_prot(ph, phnum, a) & ~(uint32_t)PF_W, nx_on));
            out->relro_pages++;
        }
    }

    /* ================= PASS 4: the image-info page =======================
     * One read-only user page directly above the image, holding a copy of the
     * ELF header, the program-header table, and 16 bytes of entropy.
     *
     * WHY A COPY. AT_PHDR is supposed to be the program's own phdrs, in its own
     * memory -- that is how a static glibc finds PT_TLS, how dl_iterate_phdr
     * works, and how an unwinder finds PT_GNU_EH_FRAME. On Linux the first
     * PT_LOAD starts at file offset 0 and the headers come along for free. Our
     * links do not do that: lld puts the headers at 0x200000, BELOW the user
     * region, and this loader skips that segment because mapping it would open
     * a USER window into shared kernel page tables. So there is no address the
     * program can be told. Rather than emit an AT_PHDR that lies or omit it and
     * break anything ported, the loader puts the table in the image itself.
     *
     * The entropy lives here too, and not on the stack where Linux puts it,
     * because on this page it is read-only: AT_RANDOM is the seed for a stack
     * canary, and a canary seed the program can rewrite is not one. */
    {
        uint64_t page = out->top;
        if (!place_page(cr3, page, VMM_WRITABLE | VMM_USER))
            return reject(ELF_E_OOM, "out of physical frames for the info page", page, 0);
        uint8_t *p = (uint8_t *)page;
        memset(p, 0, 0x1000);
        uint64_t phbytes = (uint64_t)phnum * sizeof(struct elf64_phdr);
        memcpy(p, eh, sizeof *eh);
        memcpy(p + sizeof *eh, ph, phbytes);
        uint64_t roff = (sizeof *eh + phbytes + 15) & ~(uint64_t)15;
        kernel_random_bytes(p + roff, 16);

        out->phdr_va   = page + sizeof *eh;
        out->phentsize = eh->e_phentsize;
        out->phnum     = eh->e_phnum;
        out->random_va = page + roff;
        out->info_page = page;
        /* If the link DID map its own phdrs (a normal Linux-shaped layout),
         * prefer the real address: dl_iterate_phdr's callers compare it against
         * segment addresses, and the copy is not one of the program's segments. */
        if (phdrseg && phdrseg->p_vaddr >= USER_VA_BASE) {
            uint64_t *e = vmm_pte(cr3, phdrseg->p_vaddr & ~(uint64_t)0xFFF);
            if (e && (*e & 1)) out->phdr_va = phdrseg->p_vaddr;
        }
        vmm_map_page(page, *vmm_pte(cr3, page) & PTE_ADDR_MASK,
                     map_flags(PF_R, nx_on));
        out->top = page + 0x1000;
    }

    /* ================= PASS 5: PT_TLS ====================================
     * The x86-64 variant-II layout, which is the one the psABI specifies and
     * the one every compiler on this target emits code for:
     *
     *      tp - tlsoffset   ... tp        the static TLS block
     *      %fs:0            = tp          the TCB self-pointer
     *      %fs:8            = dtv         (0: nothing here is dynamic)
     *
     * with tlsoffset = round_up(p_memsz, p_align), so a `__thread` variable is
     * reached as %fs:-N and needs no indirection at all. THE TCB IS AT THE TOP
     * of the block and the data grows DOWNWARD from it -- get that backwards and
     * every thread-local read returns the self-pointer.
     *
     * WHO SETS %fs, AND WHY IT IS NOT THIS FILE.
     *
     * The thread pointer can only be installed from ring 0 -- IA32_FS_BASE
     * needs WRMSR, and CR4.FSGSBASE is not enabled here -- so the first draft
     * of this did the WRMSR itself. That was wrong, and the threads line's work
     * is why: c/kernel/sched/sched.c now carries a per-thread `fsbase` with a
     * stated invariant -- on every core, the hardware IA32_FS_BASE equals
     * this_cpu()->current->fsbase -- and schedule() decides whether to reload
     * the MSR by COMPARING the two threads' fields. A bare WRMSR from here
     * changes the hardware without changing the field, so the very next switch
     * back would compare equal, skip the reload, and run the thread with
     * somebody else's thread pointer. One privileged instruction in the wrong
     * file would have turned a working feature into an intermittent one.
     *
     * So the loader does the part that is the loader's: it lays the block out
     * from p_filesz/p_memsz/p_align -- which nothing else can, since only the
     * loader has the image -- initialises it, builds the TCB, and reports
     * `tls_tp`. Installing it is the caller's, through sched_set_fsbase(),
     * which writes the field and the MSR together. exec.c does exactly that
     * (and installs 0 when there is no PT_TLS, so an execve can never inherit
     * the previous program's thread pointer).
     *
     * For a program whose thread does not exist yet -- proc_spawn, wm_launch --
     * the new thread starts with fsbase 0, which is the contract mini-libc
     * already documents ("the main thread arrives from crt0 with %fs pointing
     * at 0") and installs for itself with SYS_SET_TLS. What that libc could NOT
     * do until now is find its own PT_TLS: c/apps/libc/logit_tls.ld exists
     * precisely because "there is no auxv here (no AT_PHDR)" and __ehdr_start
     * points at 0x200000, outside the mapped region. AT_PHDR is real now, and
     * the block below is already laid out and initialised, so that linker
     * script is a route that can be retired rather than a requirement. */
    if (tls) {
        uint64_t align = tls->p_align < 8 ? 8 : tls->p_align;
        uint64_t tlsoff = (tls->p_memsz + align - 1) & ~(align - 1);
        uint64_t tcb    = 0x100;                 /* room for a real TCB, zeroed */
        uint64_t base   = out->top;              /* just above the info page    */
        uint64_t tp     = (base + tlsoff + align - 1) & ~(align - 1);
        uint64_t end    = (tp + tcb + 0xFFF) & ~(uint64_t)0xFFF;
        if (end > USER_VA_END - 0x100000)
            return reject(ELF_E_TLS, "no room above the image for the TLS block", end, 0);
        for (uint64_t a = base; a < end; a += 0x1000)
            if (!place_page(cr3, a, VMM_WRITABLE | VMM_USER |
                                    (nx_on ? PTE_NX : 0)))
                return reject(ELF_E_OOM, "out of physical frames for the TLS block", a, 0);
        memset((void *)(tp - tlsoff), 0, tlsoff + tcb);
        memcpy((void *)(tp - tlsoff), (uint8_t *)image + tls->p_offset, tls->p_filesz);
        *(uint64_t *)tp       = tp;              /* %fs:0  = self-pointer */
        *(uint64_t *)(tp + 8) = 0;               /* %fs:8  = dtv          */

        out->tls_tp = tp;
        out->tls_va = tp - tlsoff;
        out->tls_filesz = tls->p_filesz;
        out->tls_memsz = tls->p_memsz;
        out->tls_align = align;
        out->top = end;
    }

    out->entry = eh->e_entry;
    out->bytes_mapped = total_bytes;
    return ELF_OK;
}

uint64_t elf_load(void *image, uint64_t image_size, uint64_t *out_top)
{
    struct elf_image img;
    if (out_top) *out_top = 0;
    if (elf_load_image(image, image_size, &img) != ELF_OK)
        return 0;
    if (out_top) *out_top = img.top;
    return img.entry;
}
