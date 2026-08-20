#ifndef LOGIT_ELF_H
#define LOGIT_ELF_H

#include <stdint.h>

/* The ELF64 loader.
 *
 * WHAT THIS FILE IS FOR, said once: an ELF is a DESCRIPTION of a program, and
 * for a long time this loader read three fields of it (the magic, the class,
 * and PT_LOAD) and invented the rest. Everything else the file said -- which
 * machine it was for, which byte order, whether it wanted a dynamic loader,
 * whether its stack should be executable, where its thread-local storage went
 * -- was neither honoured nor refused. Ignoring a field is not the same as not
 * supporting it: the first says nothing happens, the second says something is
 * wrong. This loader now does one or the other for every field, and the list of
 * deliberate refusals is in elf.c above elf_load_image().
 */

/* ------------------------------------------------------------- ERRORS --
 * Every refusal has a code AND a kprintf line naming the field. The code is
 * what a test asserts on; the line is what a human reads. They are produced by
 * the same call so they cannot disagree. */
#define ELF_OK             0
#define ELF_E_SHORT       -1   /* image smaller than the header it claims       */
#define ELF_E_MAGIC       -2   /* not \x7fELF                                    */
#define ELF_E_CLASS       -3   /* not ELFCLASS64                                 */
#define ELF_E_DATA        -4   /* not ELFDATA2LSB                                */
#define ELF_E_IDENT       -5   /* EI_VERSION / EI_OSABI / EI_ABIVERSION          */
#define ELF_E_TYPE        -6   /* not ET_EXEC (ET_DYN needs relocation)          */
#define ELF_E_MACHINE     -7   /* not EM_X86_64                                  */
#define ELF_E_VERSION     -8   /* e_version != EV_CURRENT                        */
#define ELF_E_EHSIZE      -9   /* e_ehsize / e_phentsize / e_shentsize wrong     */
#define ELF_E_PHTAB      -10   /* the program-header table is out of the image   */
#define ELF_E_SHTAB      -11   /* the section-header table is out of the image   */
#define ELF_E_ENTRY      -12   /* entry point outside the user region            */
#define ELF_E_SEGRANGE   -13   /* a segment's file bytes are outside the image   */
#define ELF_E_SEGVA      -14   /* a segment's VA range is outside the user region*/
#define ELF_E_SEGALIGN   -15   /* p_align not a power of two / bad congruence    */
#define ELF_E_SEGORDER   -16   /* PT_LOADs not ascending, or they overlap        */
#define ELF_E_WX         -17   /* a PT_LOAD asks for write AND execute           */
#define ELF_E_INTERP     -18   /* PT_INTERP: no dynamic loader on this system    */
#define ELF_E_DYNAMIC    -19   /* PT_DYNAMIC: no relocation on this system       */
#define ELF_E_STACKX     -20   /* PT_GNU_STACK asks for an executable stack      */
#define ELF_E_TLS        -21   /* PT_TLS malformed                               */
#define ELF_E_TOOBIG     -22   /* the image asks for more memory than the cap    */
#define ELF_E_NOLOAD     -23   /* no PT_LOAD lands in the user region            */
#define ELF_E_OOM        -24   /* out of physical frames part-way through        */
#define ELF_E_PHNUM      -25   /* e_phnum 0, or past the cap                     */
#define ELF_E_FLAGS      -26   /* e_flags nonzero: no x86-64 flags are defined    */

/* Which program headers the image actually carried. Diagnostics, and the thing
 * a test asserts on when it wants to prove a header was SEEN and not merely
 * absent -- "PT_TLS was handled" and "there was no PT_TLS" look identical from
 * outside otherwise. */
#define ELF_SEEN_PHDR        0x0001
#define ELF_SEEN_INTERP      0x0002
#define ELF_SEEN_DYNAMIC     0x0004
#define ELF_SEEN_NOTE        0x0008
#define ELF_SEEN_TLS         0x0010
#define ELF_SEEN_GNU_STACK   0x0020
#define ELF_SEEN_GNU_RELRO   0x0040
#define ELF_SEEN_GNU_EH      0x0080
#define ELF_SEEN_GNU_PROP    0x0100
#define ELF_SEEN_UNKNOWN     0x0200   /* an ignored p_type, per the ELF rule */

/* p_flags, exported because callers reason about PT_GNU_STACK with them. */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* The program header, exported for elf_file_runs() below. It is here rather
 * than in elf.c so the predicate can be called -- and enumerated -- by a host
 * test that never maps anything, which is the whole reason it is a separate
 * function. (c/boot/efi/loader.c carries its own copy and includes none of
 * this; it is a different program, linked into the EFI stub.) */
struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

/* ------------------------------------------------------- FILE-BACKED TEXT --
 *
 * A run of whole pages inside one PT_LOAD whose bytes are in the file, are
 * never written, and line up page-for-page with the file -- so the loader can
 * hand the range to the page cache as a VMA instead of allocating a frame and
 * copying into it. Two processes running the same binary then map the SAME
 * frames, and a page nothing ever executes is never read off the disk at all.
 *
 * elf_file_runs() is PURE: no page tables, no allocator, no globals. That is
 * the same split fault.c makes for mm_fault_classify ("pure ... so the whole
 * table can be enumerated in a host test"), and for the same reason -- the
 * five conditions below are where this feature is either correct or silently
 * wrong, and a wrong answer is not a crash, it is a program running with
 * somebody else's bytes in it.
 *
 * A page is eligible iff ALL FIVE hold. Stated per PAGE and not per SEGMENT,
 * because elf.c already argues that two PT_LOADs can share a page and that a
 * page's permission is the UNION of them; a per-segment rule re-opens exactly
 * that bug.
 *
 *   (a) exactly ONE PT_LOAD covers the page. Kills the text/rodata and
 *       rodata/data boundary pages, which hold two segments' bytes and need
 *       the unioned permission.
 *   (b) that segment is not PF_W. There is no private-file COW fault case, and
 *       mmsys.c refuses a writable file mapping OUT LOUD rather than quietly
 *       handing back a private copy; .data and .bss keep the eager copy.
 *   (c) the page is entirely inside [p_vaddr, p_vaddr + p_filesz). TWO bounds,
 *       not one. The upper excludes the p_filesz < p_memsz boundary page, which
 *       is part file and part zero -- mapping it shared leaks file bytes into
 *       .bss, a silent wrong answer rather than a crash. The lower excludes a
 *       leading partial page, whose bytes below p_vaddr belong to the previous
 *       segment; (a) usually catches that and does not always -- when the
 *       previous segment's memsz ends exactly on a page boundary and this one
 *       starts mid-page (the browser's rodata at 0x452A6900) the page is
 *       covered by one segment and only (c) saves you.
 *   (d) the file offset of the page is page-aligned:
 *       (hdr_off + p_offset + (page - p_vaddr)) % 4096 == 0. Since elf.c
 *       already enforces p_offset == p_vaddr (mod 4096), this reduces to
 *       hdr_off % 4096 -- but it is written in the full form on purpose,
 *       because that one line is what makes a v1 .aex (hdr_off = 64) take the
 *       eager path with NO version test anywhere in the loader.
 *   (e) the page is inside the file. pcache_get() returns 0 past EOF and the
 *       fault then kills the process, so a run is TRIMMED to the file rather
 *       than built and discovered at first touch.
 *
 * Everything else is copied eagerly, byte for byte as before.
 *
 * `hdr_off` is where the ELF image starts inside the FILE (a .aex's hdr_size;
 * 0 for a bare ELF). `file_pages` is the file's size rounded up to pages.
 * Returns the number of runs written to `out` (at most `max`), which may be 0.
 * The phdrs must already have passed elf.c's PASS 0; on unvalidated input this
 * refuses rather than trusting the arithmetic, but it does not diagnose. */
struct elf_run {
    uint64_t va;      /* first virtual address, page aligned */
    uint64_t foff;    /* byte offset of `va` in the FILE, page aligned */
    uint64_t pages;   /* whole 4 KiB pages */
    uint32_t prot;    /* PF_R | PF_X. Never PF_W -- see (b). */
};

#define ELF_MAX_RUNS 8      /* one per non-writable PT_LOAD; every binary this
                             * tree builds has two (text + rodata), and
                             * ELF_MAX_PHNUM is 64, so this is the shape of a
                             * real link and not a guess. A ninth run is
                             * dropped to the eager path, never mis-mapped. */

int elf_file_runs(const struct elf64_phdr *ph, int phnum, uint64_t hdr_off,
                  uint64_t file_pages, struct elf_run *out, int max);

/* ------------------------------------------------------------- AUXV --
 * The SysV auxiliary vector tags, at their standard numbers. They live here
 * rather than in include/abi/logit_abi.h on purpose: these are not LogitOS's
 * ABI to choose. They are the x86-64 SysV numbers, and a program that was not
 * written for this system -- which is the whole reason to emit an auxv at all
 * -- will read them at these values or not at all.
 *
 * WHAT IS EMITTED and why, since the point of a survey finding is not to fix
 * only the fields somebody happened to list:
 *   AT_PHDR/AT_PHENT/AT_PHNUM  a program's only route to its own headers, and
 *                              therefore to its own PT_TLS and unwind tables.
 *   AT_ENTRY                   where it started.
 *   AT_PAGESZ                  4096; a libc rounds with this before it can do
 *                              anything else, and guessing is how you get a
 *                              malloc that works on one machine.
 *   AT_RANDOM                  16 bytes, from the kernel DRBG, on a read-only
 *                              page. It is a stack-canary seed; on Linux it is
 *                              on the stack, which means the program can
 *                              rewrite the seed of its own canary.
 *   AT_BASE                    0: there is no interpreter to have a base.
 *   AT_UID/EUID/GID/AT_SECURE  0. One user, never setuid -- but emitted rather
 *                              than omitted, because a libc that cannot find
 *                              AT_SECURE assumes it IS running privileged.
 *   AT_EXECFN                  the path, so argv[0] is not the only answer.
 * Deliberately NOT emitted: AT_HWCAP/AT_HWCAP2 (this kernel does not publish a
 * feature word userland could trust -- c/kernel/cpu/cpufeat.c has one, and
 * exporting it is an ABI decision for the line that owns logit_abi.h);
 * AT_PLATFORM, AT_CLKTCK, AT_SYSINFO_EHDR (there is no vDSO). */
#define AT_NULL     0
#define AT_IGNORE   1
#define AT_EXECFD   2
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_NOTELF  10
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_PLATFORM 15
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_HWCAP2  26
#define AT_EXECFN  31

/* Everything the loader learned about the image, for the caller that has to
 * build the process around it. Zero-initialise before the call. */
struct elf_image {
    uint64_t entry;         /* e_entry                                        */
    uint64_t top;           /* page-aligned top of EVERYTHING mapped, so a
                             * caller can place a stack above the image       */
    uint64_t load_base;     /* lowest mapped page                             */

    /* The program's own view of itself, for auxv. AT_PHDR is always a mapped,
     * readable address: if the link did not put the program-header table in
     * memory (ours do not -- see elf.c), the loader puts a copy there. */
    uint64_t phdr_va;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t random_va;     /* AT_RANDOM: 16 bytes of entropy, mapped RO      */
    uint64_t info_page;     /* the read-only page holding both of the above   */

    /* PT_TLS. tls_tp is the thread pointer -- the value %fs must hold. 0 when
     * the image has no thread-local storage. */
    uint64_t tls_tp;
    uint64_t tls_va, tls_filesz, tls_memsz, tls_align;

    /* PT_GNU_STACK's p_flags, or PF_R|PF_W when the image carried none. The
     * caller maps the user stack from this instead of deciding on its own. */
    uint32_t stack_flags;

    uint32_t relro_pages;   /* pages made read-only after load (PT_GNU_RELRO) */
    uint32_t seen;          /* ELF_SEEN_*                                     */
    uint32_t nload;         /* PT_LOADs that were mapped                      */
    uint64_t bytes_mapped;  /* how much memory the image cost                 */

    /* What the file-backed path actually did to THIS image, so a gate can read
     * it instead of inferring it from free-frame arithmetic. Both are 0 on the
     * eager path, which is also what a v1 image and a bare ELF report. */
    uint32_t file_pages;    /* pages handed to the page cache as a VMA        */
    uint32_t copied_pages;  /* pages allocated and copied, as before          */
    uint32_t file_runs;     /* VMAs created (0 = the whole image was copied)  */
};

/* WHERE THE IMAGE CAME FROM, for the file-backed path. Passing NULL is the
 * eager loader, unchanged and byte for byte -- which is what the host tests
 * (tests/unit/exechost) get, because they have no page cache and no VMAs.
 *
 * `fh` is a c/kernel/mm/pcache.h handle on the FILE (not the .aex's ELF
 * sub-range), so `base_off` is what turns a segment's p_offset into a file
 * offset. The loader takes no ownership of `fh`: each VMA it creates takes its
 * own reference, and the caller puts the one it came in with either way. */
struct elf_src {
    int      fh;            /* pcache handle, or -1 for "no file identity"    */
    uint64_t base_off;      /* byte offset of the ELF image inside the file   */
    uint64_t file_pages;    /* the whole file's size in 4 KiB pages           */
};

/* Load a static ELF64 executable image (already in memory, `image_size` bytes)
 * into the CURRENTLY ACTIVE address space as user pages. The image is untrusted
 * on-disk data and every field read from it is bounded against image_size.
 * Returns ELF_OK, or a negative ELF_E_* -- and prints a line naming the field
 * that was wrong. On failure nothing outside the address space has changed;
 * the address space itself may hold pages the caller must free (every caller
 * already does this by dropping the whole space). */
int elf_load_image(void *image, uint64_t image_size, struct elf_image *out);

/* The same, told where the image came from so read-only whole pages can be
 * mapped out of the page cache instead of copied. `src` NULL is exactly
 * elf_load_image(). Nothing about the refusal set, the permissions or the auxv
 * changes; the only difference is which pages are present when it returns. */
int elf_load_image_ex(void *image, uint64_t image_size, struct elf_image *out,
                      const struct elf_src *src);

/* The pre-existing shape, kept because two callers only want these two numbers.
 * Returns the entry virtual address, or 0 on failure. */
uint64_t elf_load(void *image, uint64_t image_size, uint64_t *out_top);

/* The maximum memory one image may ask for. A p_memsz is disk-controlled and
 * the loader allocates eagerly, so without a cap a 40-byte header can ask the
 * kernel for every frame it has. The largest thing this tree builds is a
 * browser test variant at ~108 MiB. */
#define ELF_MAX_IMAGE_BYTES (256ull * 1024 * 1024)

/* The private user region (PML4[0]/PDPT[1]). Anything outside it would be
 * mapped into the SHARED kernel page tables. */
#define USER_VA_BASE 0x40000000ull
#define USER_VA_END  0x80000000ull

#endif /* LOGIT_ELF_H */
