#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "prot.h"      /* PTE_NX + cpu_prot_nx(): W^X is decided here */
#include "kprintf.h"

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

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define PT_LOAD 1

/* p_flags. The ELF already says which segments are code and which are data;
 * this loader used to throw that away. */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* The private user region (PML4[0]/PDPT[1]) is exactly 1 GiB. A segment or entry
 * point outside it would map/write into the SHARED kernel page tables (next_table
 * propagates USER into them), so the whole load is rejected instead. */
#define USER_VA_BASE 0x40000000ull
#define USER_VA_END  0x80000000ull

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
 * across all 41 built ELFs). The day a JIT needs RWX it will say so by failing
 * to load, with a message naming the segment, which is a better conversation
 * than a silently re-opened boundary.
 * ------------------------------------------------------------------------ */

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
    uint64_t f = VMM_USER;
    if (prot & PF_W) f |= VMM_WRITABLE;
    if (!(prot & PF_X) && nx_on) f |= PTE_NX;
    return f;
}

static uint64_t read_cr3(void)
{
    /* elf_load runs with the TARGET space active, which is not always the
     * scheduler's current thread's space (proc_spawn switches CR3 by hand
     * around the load), so the register is the only correct source. */
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v & ~(uint64_t)0xFFF;
}

uint64_t elf_load(void *image, uint64_t image_size, uint64_t *out_top)
{
    struct elf64_ehdr *eh = image;
    uint64_t top = 0;
    if (out_top) *out_top = 0;

    if (image_size < sizeof *eh) return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 2)            /* ELFCLASS64 */
        return 0;

    /* The program-header table is disk-controlled: bound it to the image before
     * dereferencing (subtraction form avoids overflow in the add). */
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) return 0;
    if (eh->e_phnum > 64) return 0;                      /* sane cap */
    if (eh->e_phoff > image_size ||
        (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > image_size - eh->e_phoff)
        return 0;

    /* The entry point must land in the user region too, with 64 MiB of headroom
     * above it: setup_cli_stack() maps the CLI stack at (entry & ~0xFFFFF) +
     * 0x4000000. (Subtraction form: no overflow for a huge e_entry.) */
    if (eh->e_entry < USER_VA_BASE || eh->e_entry > USER_VA_END - 0x4000000)
        return 0;

    struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)image + eh->e_phoff);

    /* W^X, checked before anything is mapped so a refusal leaves no half-built
     * address space behind. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if ((ph[i].p_flags & PF_W) && (ph[i].p_flags & PF_X)) {
            kprintf("[elf] refusing segment %d: writable AND executable "
                    "(vaddr=%p memsz=%p) -- W^X\n", i,
                    (void *)ph[i].p_vaddr, (void *)ph[i].p_memsz);
            return 0;
        }
    }

    int nx_on = cpu_prot_nx_usable();
    uint64_t cr3 = read_cr3();

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        /* Validate the segment: file bytes must fit the image, file<=mem, and the
         * target VA range must lie wholly inside the private user region. */
        if (ph[i].p_filesz > ph[i].p_memsz) return 0;
        if (ph[i].p_offset > image_size || ph[i].p_filesz > image_size - ph[i].p_offset)
            return 0;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end < start) return 0;                  /* p_vaddr + p_memsz overflow */
        /* Some lld versions emit a read-only headers PT_LOAD at a low fixed VA
         * (0x200000). It is unused at runtime, and mapping it into the SHARED
         * low region with USER would hand ring-3 a window over kernel memory --
         * skip it instead of mapping. Anything straddling or above the user
         * region boundary is still rejected outright. */
        if (end <= USER_VA_BASE) continue;
        if (start < USER_VA_BASE || end > USER_VA_END) return 0;  /* outside the user region */
        if (end > top) top = end;

        for (uint64_t a = start; a < end; a += 0x1000) {
            /* Skip a page an earlier segment already placed: two PT_LOADs may
             * share one. Allocating again would map a second frame over the
             * first and throw away the bytes already copied into it. */
            uint64_t *e = vmm_pte(cr3, a);
            if (e && (*e & 1)) continue;               /* PRESENT */
            uint64_t frame = pmm_alloc();
            if (!frame) return 0;                      /* OOM: caller aborts + frees the space */
            memset((void *)frame, 0, 0x1000);          /* zero (covers .bss) */
            /* Writable for now, whatever the segment says -- the memcpy below
             * needs it. Pass 2 takes it away again. */
            vmm_map_page(a, frame, VMM_WRITABLE | VMM_USER);
        }

        /* Copy file-backed bytes to the now-mapped user virtual address. */
        memcpy((void *)ph[i].p_vaddr, (uint8_t *)image + ph[i].p_offset,
               ph[i].p_filesz);
    }

    /* Pass 2: the real protections, now that every byte is in place. Re-mapping
     * the same frame through vmm_map_page() rather than editing the PTE by hand
     * keeps this on the public interface and gets the invlpg for free.
     *
     * Driven off the phdrs again (not a page list) because a program's .bss can
     * be tens of megabytes -- /bin/as reserves 25 MiB -- and a per-page record
     * of a load that size is a bigger allocation than the load. page_prot()
     * unions the segments, so a page reached from two of them gets the same
     * answer either time and the order of this loop does not matter. */
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end <= USER_VA_BASE) continue;             /* the skipped headers segment */
        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t *e = vmm_pte(cr3, a);
            if (!e || !(*e & 1)) continue;             /* not present: nothing to protect */
            uint64_t frame = *e & ~(uint64_t)0xFFF & ~PTE_NX;
            vmm_map_page(a, frame, map_flags(page_prot(ph, eh->e_phnum, a), nx_on));
        }
    }

    if (out_top) *out_top = top;
    return eh->e_entry;
}
