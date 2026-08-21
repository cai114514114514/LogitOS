/* The relocatable-ELF half of the module loader: parse an ET_REL x86-64
 * object, lay its SHF_ALLOC sections out in a caller-supplied block, and apply
 * its relocations.
 *
 * THIS FILE IS PURE ON PURPOSE. It calls nothing -- no kmalloc, no kprintf, no
 * VFS, no locks -- and touches only the two buffers it is handed. That is what
 * lets `make test-modreloc` drive it as an ordinary host program against
 * objects built by the real cross-compiler, which is the only way the
 * relocation arithmetic gets tested without booting QEMU for every edit. The
 * kernel-side glue (memory, credentials, the driver registry, the syscall)
 * lives in modload.c and is the part that cannot be tested that way.
 *
 * WHY THE BOUNDS CHECKS ARE EVERYWHERE, since they are most of the line count.
 * Every offset, count and index below comes out of the file. The security
 * model (module.h) says root put that file on disk, which bounds who can
 * ATTACK this parser -- it does not make the parser's arithmetic correct, and
 * a truncated or half-written .ko is not an attack, it is a Tuesday. An
 * unchecked sh_offset walks off the end of a kmalloc'd buffer in ring 0 with
 * the BKL held. So: nothing read from the image is used as an offset before it
 * has been compared against imglen, and every such check is written as
 *     if (off > imglen || size > imglen - off) refuse;
 * rather than `off + size > imglen`, which is the same expression until
 * off + size overflows and then is the opposite of it.
 *
 * ONE-OBJECT ASSUMPTION, stated because it is load-bearing. A module here is
 * a single .o, not an archive and not a partially-linked group. So there is
 * nothing to resolve BETWEEN objects, no common-symbol merging and no
 * SHF_MERGE deduplication to perform -- .rodata.str1.1 is copied verbatim like
 * any other section. Building a module from several source files means
 * `ld -r` first (which does all of that) and handing the result here.
 */

#include <stdint.h>
#include <stddef.h>
#include "module.h"

/* Local copies rather than lib/string.c's, so the host test links nothing from
 * this tree and cannot be affected by tests/unit/libc_rename.h (see the header
 * of tests/libc.mk: a missing rename there silently REPLACES glibc's version
 * for the whole process, which is a trap worth simply not standing near). */
static void m_copy(void *d, const void *s, uint32_t n)
{
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void m_zero(void *d, uint32_t n)
{
    uint8_t *dd = (uint8_t *)d;
    for (uint32_t i = 0; i < n; i++) dd[i] = 0;
}
static int m_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ------------------------------------------------------------ ELF types -- */
struct e64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct e64_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed));

struct e64_sym {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed));

struct e64_rela {
    uint64_t r_offset;
    uint64_t r_info;         /* sym << 32 | type */
    int64_t  r_addend;
} __attribute__((packed));

#define ET_REL      1
#define EM_X86_64  62
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EV_CURRENT  1

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHT_REL      9

#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00u
#define SHN_ABS       0xfff1u
#define SHN_COMMON    0xfff2u

/* A section table this loader will walk. 64 is not a guess: the three real
 * drivers measured produce 15, 17 and 17 sections respectively, and an object
 * with more than 64 is either -ffunction-sections (which a module does not
 * need -- nothing here does --gc-sections) or is not a driver. Refusing is
 * better than a variable-length walk that needs an allocation in a file whose
 * whole property is that it makes none. */
#define MOD_MAX_SECTIONS 64

/* NEGATIVE CONTROL: -DMODRELOC_NO_RANGE_CHECK compiles the four range tests
 * below out. It is not "the loader with a feature removed" -- it is the
 * PLAUSIBLE WRONG LOADER, the one somebody writes after reasoning that a
 * 512 MiB machine cannot overflow a 32-bit field. Built that way, a module
 * placed above 4 GiB relocates "successfully" and every string pointer in it
 * is silently truncated to its low 32 bits -- it does not crash at the
 * relocation, it crashes later and somewhere else, which is the failure mode
 * the check exists to convert into a refusal.
 * tests/unit/modreloc_test.c loads a real object at a high address on purpose
 * and requires MOD_E_RANGE; `make test-modreloc-negctl` runs that same test
 * against this build and succeeds only when it FAILS. */
#ifdef MODRELOC_NO_RANGE_CHECK
#define RANGE_FAIL(cond) ((void)0)
#else
#define RANGE_FAIL(cond) do { if (cond) return MOD_E_RANGE; } while (0)
#endif

/* --------------------------------------------------------- one relocation --
 * The arithmetic and the range rule, per type, and nothing else. Split out so
 * the host test can hit every branch including the two refusals without
 * building an object file for each. */
int mod_reloc_apply(void *where, uint32_t type, uint64_t S, int64_t A)
{
    uint64_t P = (uint64_t)(uintptr_t)where;
    uint8_t *w = (uint8_t *)where;

    /* Written byte by byte rather than through a 4- or 8-byte pointer cast:
     * x86 tolerates the unaligned store, but the host test builds this file
     * under -fsanitize=undefined where the cast is a diagnostic, and a
     * sanitizer that fires on the loader itself is a sanitizer nobody can use
     * on the module. */
    switch (type) {
    case R_X86_64_NONE:
        return 0;                                   /* linkers emit these */

    case R_X86_64_64: {
        uint64_t v = S + (uint64_t)A;
        for (int i = 0; i < 8; i++) w[i] = (uint8_t)(v >> (8 * i));
        return 0;                                   /* 64 bits always fit */
    }

    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        /* PLT32 is treated as PC32, which is correct and not a shortcut: there
         * is no PLT in a statically linked kernel, so the "call via PLT"
         * encoding and the direct call encoding are the same rel32 and the
         * only difference would be a stub this kernel has no linker to build.
         * Every call in every driver measured came out as PLT32, so this is
         * the common case, not the exotic one. */
        int64_t v = (int64_t)(S + (uint64_t)A) - (int64_t)P;
        RANGE_FAIL(v < -2147483648LL || v > 2147483647LL);
        uint32_t u = (uint32_t)(int32_t)v;
        for (int i = 0; i < 4; i++) w[i] = (uint8_t)(u >> (8 * i));
        return 0;
    }

    case R_X86_64_32: {
        /* Zero-extended. This is the one that carries every string literal
         * under -fno-pic -mcmodel=small, so it is the most common relocation
         * in a driver and the one whose range rule actually binds. */
        uint64_t v = S + (uint64_t)A;
        RANGE_FAIL(v > 0xFFFFFFFFull);
        for (int i = 0; i < 4; i++) w[i] = (uint8_t)(v >> (8 * i));
        return 0;
    }

    case R_X86_64_32S: {
        /* Sign-extended: the field is 32 bits and the CPU will sign-extend it,
         * so the value must survive that round trip. Checked separately from
         * R_X86_64_32 rather than sharing one "fits in 32 bits" test, because
         * the two ranges differ over the whole top half of 4 GiB and a machine
         * with 3 GiB of RAM is exactly where they would disagree. */
        int64_t v = (int64_t)(S + (uint64_t)A);
        RANGE_FAIL(v < -2147483648LL || v > 2147483647LL);
        uint32_t u = (uint32_t)(int32_t)v;
        for (int i = 0; i < 4; i++) w[i] = (uint8_t)(u >> (8 * i));
        return 0;
    }

    default:
        /* NOT a no-op, and not a best guess. An unhandled relocation left
         * unapplied is a call to address zero or a pointer to the wrong
         * string, discovered later and somewhere else. Refusing names the
         * type, which is what tells the next person which case to add. */
        return MOD_E_RELOC;
    }
}

/* -------------------------------------------------------------- headers -- */
/* Validate the ELF header and return the section table, or a negative
 * MOD_E_*. On success *shdr / *nsec / *shstr are the section headers, their
 * count, and the section-name string table (which may be NULL if absent). */
static int hdrs(const void *img, uint32_t imglen,
                const struct e64_shdr **shdr_out, uint32_t *nsec_out,
                const char **shstr_out, uint32_t *shstr_len_out)
{
    if (!img || imglen < sizeof(struct e64_ehdr)) return MOD_E_FORMAT;
    const uint8_t *base = (const uint8_t *)img;
    const struct e64_ehdr *eh = (const struct e64_ehdr *)img;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')      return MOD_E_FORMAT;
    if (eh->e_ident[EI_CLASS]   != ELFCLASS64)                return MOD_E_FORMAT;
    if (eh->e_ident[EI_DATA]    != ELFDATA2LSB)               return MOD_E_FORMAT;
    if (eh->e_ident[EI_VERSION] != EV_CURRENT)                return MOD_E_FORMAT;
    /* ET_REL specifically. An ET_EXEC handed here would parse far enough to be
     * confusing (it has a section table too) and then have nothing to
     * relocate; refusing by type says which of the two loaders it wanted. */
    if (eh->e_type != ET_REL)                                 return MOD_E_FORMAT;
    if (eh->e_machine != EM_X86_64)                           return MOD_E_FORMAT;
    if (eh->e_shentsize != sizeof(struct e64_shdr))           return MOD_E_FORMAT;
    if (eh->e_shnum == 0 || eh->e_shnum > MOD_MAX_SECTIONS)   return MOD_E_FORMAT;

    uint64_t shoff = eh->e_shoff;
    uint64_t shbytes = (uint64_t)eh->e_shnum * sizeof(struct e64_shdr);
    if (shoff > imglen || shbytes > (uint64_t)imglen - shoff) return MOD_E_FORMAT;

    const struct e64_shdr *sh = (const struct e64_shdr *)(base + shoff);

    const char *shstr = NULL; uint32_t shstr_len = 0;
    if (eh->e_shstrndx != SHN_UNDEF && eh->e_shstrndx < eh->e_shnum) {
        const struct e64_shdr *s = &sh[eh->e_shstrndx];
        if (s->sh_type == SHT_STRTAB &&
            s->sh_offset <= imglen && s->sh_size <= (uint64_t)imglen - s->sh_offset) {
            shstr = (const char *)(base + s->sh_offset);
            shstr_len = (uint32_t)s->sh_size;
        }
    }

    /* Every section's own extent, checked once here so the three walks below
     * do not each have to repeat it. NOBITS occupies no file bytes, so it is
     * the one exemption -- and forgetting that exemption is the classic way to
     * refuse every object that has a .bss, i.e. all of them. */
    for (uint32_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_NOBITS) continue;
        if (sh[i].sh_offset > imglen ||
            sh[i].sh_size > (uint64_t)imglen - sh[i].sh_offset) return MOD_E_FORMAT;
    }

    *shdr_out = sh; *nsec_out = eh->e_shnum;
    *shstr_out = shstr; *shstr_len_out = shstr_len;
    return 0;
}

static const char *sec_name(const char *shstr, uint32_t shstr_len, uint32_t off)
{
    if (!shstr || off >= shstr_len) return "";
    return shstr + off;
}

/* ---------------------------------------------------------------- layout --
 * Assign every SHF_ALLOC section an offset in the destination block, honouring
 * sh_addralign. ONE function, called by both mod_elf_size() and
 * mod_elf_load(), because two copies of a layout rule is exactly the shape
 * where the size call and the load call quietly disagree by one alignment pad
 * and the last section runs off the end of the allocation. */
static long layout(const struct e64_shdr *sh, uint32_t nsec, uint32_t *off_out)
{
    uint64_t cur = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (off_out) off_out[i] = 0xFFFFFFFFu;              /* "not placed" */
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        if (sh[i].sh_type != SHT_PROGBITS && sh[i].sh_type != SHT_NOBITS) continue;

        uint64_t a = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        /* A power of two, and one this loader can honour. 4096 covers every
         * alignment clang emits for a freestanding object (16 for .text, 8 for
         * pointer data, 1 for strings); a larger one would be a page-aligned
         * table that wants its own allocation, not a pad inside this block. */
        if (a & (a - 1)) return MOD_E_FORMAT;
        if (a > 4096) return MOD_E_FORMAT;

        cur = (cur + a - 1) & ~(a - 1);
        if (cur > MOD_MAX_IMAGE || sh[i].sh_size > MOD_MAX_IMAGE - cur)
            return MOD_E_TOOBIG;
        if (off_out) off_out[i] = (uint32_t)cur;
        cur += sh[i].sh_size;
    }
    /* Never zero: a zero-size kmalloc has no address to hand back and every
     * caller would then have to distinguish "empty module" from "out of
     * memory". An object with no allocatable section is not a module. */
    if (cur == 0) return MOD_E_FORMAT;
    return (long)cur;
}

long mod_elf_size(const void *img, uint32_t imglen)
{
    const struct e64_shdr *sh; uint32_t nsec; const char *ss; uint32_t sslen;
    int e = hdrs(img, imglen, &sh, &nsec, &ss, &sslen);
    if (e < 0) return e;
    return layout(sh, nsec, NULL);
}

/* ------------------------------------------------------ symbol resolution --
 * st_shndx says where a symbol lives; only three of its cases can appear in a
 * driver object and the rest are refused by name rather than defaulted. */
static int symval(const struct e64_sym *sym, const uint32_t *off,
                  uint32_t nsec,
                  uint8_t *dst, const char *strtab, uint32_t strtab_len,
                  mod_resolve_fn resolve, void *ctx,
                  uint64_t *out, const char **undef)
{
    uint16_t shndx = sym->st_shndx;

    if (shndx == SHN_UNDEF) {
        const char *nm = (strtab && sym->st_name < strtab_len)
                       ? strtab + sym->st_name : "";
        if (!nm[0]) { if (undef) *undef = "<unnamed>"; return MOD_E_UNDEF; }
        void *a = resolve ? resolve(nm, ctx) : NULL;
        if (!a) { if (undef) *undef = nm; return MOD_E_UNDEF; }
        *out = (uint64_t)(uintptr_t)a;
        return 0;
    }
    if (shndx == SHN_ABS) { *out = sym->st_value; return 0; }
    if (shndx == SHN_COMMON) {
        /* -fno-common is clang's default and the kernel does not override it,
         * so a common symbol means the module was built with -fcommon and has
         * a tentative definition this loader would have to allocate and merge.
         * Refusing is right: silently placing it would give the module a
         * variable at an address the rest of the kernel does not agree on. */
        if (undef) *undef = "<SHN_COMMON: rebuild the module with -fno-common>";
        return MOD_E_UNDEF;
    }
    if (shndx >= SHN_LORESERVE || shndx >= nsec) return MOD_E_FORMAT;

    /* A symbol in a section that was not laid out (a debug or .comment
     * section) has no address in the destination block. Only reachable via a
     * relocation in a section this loader also skipped, so it is a
     * malformed-input path, not a normal one. */
    if (off[shndx] == 0xFFFFFFFFu) return MOD_E_FORMAT;
    *out = (uint64_t)(uintptr_t)(dst + off[shndx]) + sym->st_value;
    return 0;
}

/* ------------------------------------------------------------------ load -- */
int mod_elf_load(const void *img, uint32_t imglen, void *dstv, uint32_t dstlen,
                 mod_resolve_fn resolve, void *ctx,
                 struct mod_layout *out, const char **undef)
{
    const struct e64_shdr *sh; uint32_t nsec; const char *ss; uint32_t sslen;
    int e = hdrs(img, imglen, &sh, &nsec, &ss, &sslen);
    if (e < 0) return e;
    if (!dstv || !out) return MOD_E_INVAL;

    const uint8_t *base = (const uint8_t *)img;
    uint8_t *dst = (uint8_t *)dstv;

    uint32_t off[MOD_MAX_SECTIONS];
    long need = layout(sh, nsec, off);
    if (need < 0) return (int)need;
    if ((uint64_t)need > dstlen) return MOD_E_TOOBIG;

    m_zero(out, sizeof *out);
    out->dst = dstv; out->dstlen = dstlen;

    /* 1. Place the sections. NOBITS is zeroed rather than copied -- it has no
     *    bytes in the file -- and a driver's .bss must start zero or its
     *    "have I probed yet" flags come up as whatever the heap block held. */
    for (uint32_t i = 0; i < nsec; i++) {
        if (off[i] == 0xFFFFFFFFu) continue;
        if (sh[i].sh_type == SHT_NOBITS) m_zero(dst + off[i], (uint32_t)sh[i].sh_size);
        else m_copy(dst + off[i], base + sh[i].sh_offset, (uint32_t)sh[i].sh_size);

        if (!out->text_size && (sh[i].sh_flags & SHF_EXECINSTR)) {
            out->text_off = off[i];
            out->text_size = (uint32_t)sh[i].sh_size;
        }
        if (m_streq(sec_name(ss, sslen, sh[i].sh_name), "logit_drivers")) {
            out->drv_start = dst + off[i];
            out->drv_stop  = dst + off[i] + sh[i].sh_size;
        }
    }

    /* 2. Relocate. SHT_REL (no explicit addend) is refused rather than
     *    supported: on x86-64 nothing emits it -- every relocation in every
     *    object measured is RELA -- so a REL implementation would be code that
     *    has never run, which this tree treats as worse than its absence. */
    for (uint32_t i = 0; i < nsec; i++) {
        if (sh[i].sh_type == SHT_REL) return MOD_E_RELOC;
        if (sh[i].sh_type != SHT_RELA) continue;
        if (sh[i].sh_entsize != sizeof(struct e64_rela)) return MOD_E_FORMAT;

        uint32_t tgt = sh[i].sh_info;                  /* section being patched */
        if (tgt >= nsec) return MOD_E_FORMAT;
        /* Relocations against a section that was not laid out (.rela.debug_*,
         * .rela.eh_frame) are skipped, not refused: linker.ld discards those
         * sections in the kernel too, and an object built with -g has them. */
        if (off[tgt] == 0xFFFFFFFFu) continue;

        uint32_t symsec = sh[i].sh_link;
        if (symsec >= nsec || sh[symsec].sh_type != SHT_SYMTAB) return MOD_E_FORMAT;
        if (sh[symsec].sh_entsize != sizeof(struct e64_sym)) return MOD_E_FORMAT;
        const struct e64_sym *syms = (const struct e64_sym *)(base + sh[symsec].sh_offset);
        uint64_t nsym = sh[symsec].sh_size / sizeof(struct e64_sym);

        uint32_t strsec = sh[symsec].sh_link;
        const char *strtab = NULL; uint32_t strtab_len = 0;
        if (strsec < nsec && sh[strsec].sh_type == SHT_STRTAB) {
            strtab = (const char *)(base + sh[strsec].sh_offset);
            strtab_len = (uint32_t)sh[strsec].sh_size;
        }

        const struct e64_rela *ra = (const struct e64_rela *)(base + sh[i].sh_offset);
        uint64_t nrel = sh[i].sh_size / sizeof(struct e64_rela);

        for (uint64_t r = 0; r < nrel; r++) {
            uint32_t rtype = (uint32_t)(ra[r].r_info & 0xFFFFFFFFu);
            uint64_t rsym  = ra[r].r_info >> 32;
            if (rsym >= nsym) return MOD_E_FORMAT;

            /* The patch site must lie wholly inside the target section, at the
             * width THIS relocation writes. Requiring 8 bytes for every type
             * would be one comparison instead of the little table below and is
             * wrong in a way that only shows up on real objects: a 4-byte
             * relocation is perfectly legal in the LAST four bytes of a
             * section, and demanding eight there refuses a correct module. The
             * widths must stay in step with what mod_reloc_apply actually
             * stores -- an unknown type is given 8, the maximum, so a type
             * added there without being added here is refused rather than
             * allowed to write past the section. */
            uint32_t w = (rtype == R_X86_64_NONE)  ? 0
                       : (rtype == R_X86_64_64)    ? 8
                       : (rtype == R_X86_64_PC32 || rtype == R_X86_64_PLT32 ||
                          rtype == R_X86_64_32    || rtype == R_X86_64_32S) ? 4
                       : 8;
            if (ra[r].r_offset > sh[tgt].sh_size ||
                sh[tgt].sh_size - ra[r].r_offset < w) return MOD_E_FORMAT;

            uint64_t S = 0;
            int se = symval(&syms[rsym], off, nsec, dst,
                            strtab, strtab_len, resolve, ctx, &S, undef);
            if (se < 0) return se;

            int pe = mod_reloc_apply(dst + off[tgt] + ra[r].r_offset,
                                     rtype, S, ra[r].r_addend);
            if (pe < 0) return pe;
        }
    }

    return 0;
}
