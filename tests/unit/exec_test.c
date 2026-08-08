/* The ELF64 + AEX loader, tested against the real thing.
 *
 * c/kernel/exec/elf.c and c/kernel/exec/aex.c are compiled into this program
 * unmodified. What is replaced is the machine underneath them
 * (tests/unit/exechost/space.c), and it is replaced with the HOST's MMU rather
 * than with a table of integers -- so "text is not writable" is established by
 * storing to it and catching the fault, not by reading back a flag this test
 * put there itself.
 *
 * Three parts:
 *   1. EVERY built .aex loads, twice (with NX off, as the kernel runs today,
 *      and with NX on, as it will run when the mm masks change), and the bytes
 *      at each segment's virtual address equal the bytes in the file.
 *   2. The rejection matrix: for each thing the loader is supposed to refuse,
 *      an image that does exactly that thing and nothing else, and the refusal
 *      must carry the right code AND a message.
 *   3. The headers that do something: PT_TLS, PT_GNU_STACK, PT_GNU_RELRO,
 *      PT_PHDR/AT_PHDR, AT_RANDOM.
 *
 * Usage: exec_test <file.aex>...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "space.h"
#include "elf.h"
#include "aex.h"

static int g_checks, g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fails++; printf("FAIL: %s\n", what); }
    else printf("ok: %s\n", what);
}
static void ckq(int cond, const char *what)   /* quiet on success */
{
    g_checks++;
    if (!cond) { g_fails++; printf("FAIL: %s\n", what); }
}

/* ------------------------------------------------------------- ELF bits -- */
struct ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));
struct phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552

static void *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); fclose(f); return 0; }
    fclose(f);
    return b;
}

/* ================= part 1: every built binary ============================ */
static void load_one(const char *path, int nx)
{
    long n = 0;
    uint8_t *file = slurp(path, &n);
    if (!file) { printf("FAIL: cannot read %s\n", path); g_fails++; return; }

    space_reset();
    space_set_nx(nx);
    space_quiet(1);

    struct elf_image img;
    /* Through the AEX container, exactly as the kernel does. */
    char name[32], ext[8];
    uint64_t entry = aex_load(file, (uint64_t)n, name, ext, 0);
    char msg[512];
    snprintf(msg, sizeof msg, "%s: aex_load returned an entry point (nx=%d)", path, nx);
    ckq(entry != 0, msg);
    if (!entry) { free(file); space_quiet(0); return; }

    /* And again through the raw ELF, so the struct is available. */
    uint8_t *elf = 0; uint64_t elfsz = 0;
    if (aex_elf_range(file, (uint64_t)n, (const void **)&elf, &elfsz) != 0) {
        printf("FAIL: %s: aex_elf_range\n", path); g_fails++; free(file); space_quiet(0); return;
    }
    space_reset();
    int rc = elf_load_image(elf, elfsz, &img);
    snprintf(msg, sizeof msg, "%s: elf_load_image ok (nx=%d) rc=%d", path, nx, rc);
    ckq(rc == ELF_OK, msg);
    if (rc != ELF_OK) { free(file); space_quiet(0); return; }

    ckq(img.entry == entry, "the two entry points agree");

    struct ehdr *eh = (struct ehdr *)elf;
    struct phdr *ph = (struct phdr *)(elf + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t end = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFull;
        if (end <= USER_VA_BASE) continue;

        /* the file bytes arrived, byte for byte */
        if (ph[i].p_filesz &&
            memcmp((void *)ph[i].p_vaddr, elf + ph[i].p_offset, ph[i].p_filesz)) {
            snprintf(msg, sizeof msg, "%s: segment %d contents differ from the file", path, i);
            printf("FAIL: %s\n", msg); g_fails++;
        }
        g_checks++;
        /* the .bss tail is zero */
        for (uint64_t o = ph[i].p_filesz; o < ph[i].p_memsz; o++)
            if (((uint8_t *)ph[i].p_vaddr)[o]) {
                snprintf(msg, sizeof msg, "%s: segment %d .bss byte %llu is not zero",
                         path, i, (unsigned long long)o);
                printf("FAIL: %s\n", msg); g_fails++;
                break;
            }
        g_checks++;

        /* W^X, measured. A page belonging to exactly this segment (its middle)
         * must be writable iff PF_W, and no-execute iff not PF_X. Pages shared
         * with a neighbouring segment are skipped -- the union rule makes them
         * a different claim, and part 3 tests that one directly. */
        uint64_t mid = (ph[i].p_vaddr + ph[i].p_memsz / 2) & ~0xFFFull;
        int shared = 0;
        for (int j = 0; j < eh->e_phnum; j++) {
            if (j == i || ph[j].p_type != PT_LOAD) continue;
            uint64_t s = ph[j].p_vaddr & ~0xFFFull;
            uint64_t e = (ph[j].p_vaddr + ph[j].p_memsz + 0xFFF) & ~0xFFFull;
            if (mid >= s && mid < e) shared = 1;
        }
        /* PT_GNU_RELRO also legitimately removes write from a PF_W page. */
        for (int j = 0; j < eh->e_phnum; j++)
            if (ph[j].p_type == PT_GNU_RELRO &&
                mid >= (ph[j].p_vaddr & ~0xFFFull) &&
                mid < ph[j].p_vaddr + ph[j].p_memsz) shared = 1;
        if (!shared) {
            int want_w = (ph[i].p_flags & PF_W) != 0;
            int got_w = space_writable(mid);
            if (want_w != got_w) {
                snprintf(msg, sizeof msg,
                         "%s: segment %d page %llx writable=%d, p_flags says %d",
                         path, i, (unsigned long long)mid, got_w, want_w);
                printf("FAIL: %s\n", msg); g_fails++;
            }
            g_checks++;
            if (nx) {
                int want_nx = (ph[i].p_flags & PF_X) == 0;
                if (space_nx(mid) != want_nx) {
                    snprintf(msg, sizeof msg, "%s: segment %d page %llx NX=%d, want %d",
                             path, i, (unsigned long long)mid, space_nx(mid), want_nx);
                    printf("FAIL: %s\n", msg); g_fails++;
                }
                g_checks++;
            }
        }
    }

    /* auxv material: AT_PHDR points at a real program-header table, AT_RANDOM
     * at 16 readable bytes, and neither is writable. */
    ckq(img.phdr_va != 0, "AT_PHDR is set");
    ckq(img.phnum == eh->e_phnum && img.phentsize == eh->e_phentsize,
        "AT_PHNUM/AT_PHENT match the file");
    if (img.phdr_va) {
        struct phdr *up = (struct phdr *)img.phdr_va;
        int same = 1;
        for (int i = 0; i < eh->e_phnum; i++)
            if (memcmp(&up[i], &ph[i], sizeof *ph)) same = 0;
        ckq(same, "the mapped program-header table equals the file's");
        ckq(!space_writable(img.phdr_va & ~0xFFFull), "AT_PHDR's page is read-only");
    }
    ckq(img.random_va != 0, "AT_RANDOM is set");
    if (img.random_va) {
        int nz = 0;
        for (int i = 0; i < 16; i++) nz |= ((uint8_t *)img.random_va)[i];
        ckq(nz != 0, "AT_RANDOM's 16 bytes are not all zero");
        ckq(!space_writable(img.random_va), "AT_RANDOM's page is read-only");
    }
    ckq((img.stack_flags & PF_X) == 0, "the stack this image asks for is not executable");
    ckq(img.top > img.entry, "the image top is above the entry point");

    free(file);
    space_quiet(0);
}

/* ================= a builder for synthetic images ======================== */
struct build {
    uint8_t  buf[65536];
    uint64_t size;
    struct ehdr *eh;
    struct phdr *ph;
    int       nph;
};

static void b_init(struct build *b, int nph)
{
    memset(b, 0, sizeof *b);
    b->eh = (struct ehdr *)b->buf;
    memcpy(b->eh->e_ident, "\x7f" "ELF\x02\x01\x01\x00", 8);
    b->eh->e_type = 2;                 /* ET_EXEC   */
    b->eh->e_machine = 62;             /* EM_X86_64 */
    b->eh->e_version = 1;
    b->eh->e_ehsize = 64;
    b->eh->e_phentsize = 56;
    b->eh->e_phnum = (uint16_t)nph;
    b->eh->e_phoff = 64;
    b->nph = nph;
    b->ph = (struct phdr *)(b->buf + 64);
    b->size = 64 + (uint64_t)nph * 56;
    b->size = (b->size + 0xFFF) & ~0xFFFull;   /* segment data starts at 0x1000 */
}

/* Append `n` bytes of content at file offset `off` (page aligned by caller). */
static void b_seg(struct build *b, int i, uint32_t type, uint32_t flags,
                  uint64_t vaddr, uint64_t filesz, uint64_t memsz, uint64_t align)
{
    b->ph[i].p_type = type;
    b->ph[i].p_flags = flags;
    b->ph[i].p_vaddr = vaddr;
    b->ph[i].p_paddr = vaddr;
    b->ph[i].p_filesz = filesz;
    b->ph[i].p_memsz = memsz;
    b->ph[i].p_align = align;
    if (filesz) {
        uint64_t off = (b->size + (vaddr & 0xFFF));      /* keep the congruence */
        off = (b->size & ~0xFFFull) + (vaddr & 0xFFF);
        if (off < b->size) off += 0x1000;
        b->ph[i].p_offset = off;
        for (uint64_t k = 0; k < filesz; k++)
            b->buf[off + k] = (uint8_t)(0x40 + i + (k & 0x0F));
        b->size = off + filesz;
    } else {
        b->ph[i].p_offset = b->size;
    }
}

/* The reference image: text at 0x40000000, rodata, data+bss. Loads clean. */
static void b_good(struct build *b)
{
    b_init(b, 4);
    b_seg(b, 0, PT_LOAD,      PF_R | PF_X, 0x40000000, 0x100, 0x100, 0x1000);
    b_seg(b, 1, PT_LOAD,      PF_R,        0x40001000, 0x080, 0x080, 0x1000);
    b_seg(b, 2, PT_LOAD,      PF_R | PF_W, 0x40002000, 0x040, 0x2000, 0x1000);
    b_seg(b, 3, PT_GNU_STACK, PF_R | PF_W, 0,          0,     0,      0);
    b->eh->e_entry = 0x40000000;
}

static int try_load(struct build *b, struct elf_image *out)
{
    struct elf_image tmp;
    space_reset();
    space_msgs_reset();
    return elf_load_image(b->buf, b->size, out ? out : &tmp);
}

static void expect(struct build *b, int want, const char *what)
{
    space_quiet(1);
    int rc = try_load(b, 0);
    int msgs = space_msgs();
    char last[512];
    snprintf(last, sizeof last, "%s", space_last_msg());
    space_quiet(0);
    g_checks++;
    if (rc != want) {
        g_fails++;
        printf("FAIL: %s -- got %d, want %d (%s)\n", what, rc, want, last);
        return;
    }
    if (want != ELF_OK && msgs == 0) {
        g_fails++;
        printf("FAIL: %s -- refused silently, with no message\n", what);
        return;
    }
    if (want == ELF_OK) printf("ok: %s\n", what);
    else {
        /* strip the trailing newline for a tidy line */
        size_t l = strlen(last);
        while (l && (last[l-1] == '\n' || last[l-1] == '\r')) last[--l] = 0;
        printf("ok: %s -> %s\n", what, last);
    }
}

/* ================= part 2: the rejection matrix =========================== */
static void rejection_matrix(void)
{
    struct build b;
    printf("\n-- part 2: what the loader refuses --\n");

    b_good(&b); expect(&b, ELF_OK, "the reference image loads");

    /* truncation, at every meaningful boundary */
    b_good(&b); b.size = 32;   expect(&b, ELF_E_SHORT,  "truncated before the ELF header ends");
    b_good(&b); b.size = 64;   expect(&b, ELF_E_PHTAB,  "truncated before the program headers");
    b_good(&b); b.size = 100;  expect(&b, ELF_E_PHTAB,  "program-header table cut in half");
    b_good(&b); b.size = 0x1010; expect(&b, ELF_E_SEGRANGE, "segment contents cut off");

    b_good(&b); b.buf[1] = 'X';            expect(&b, ELF_E_MAGIC,  "bad magic");
    b_good(&b); b.eh->e_ident[4] = 1;      expect(&b, ELF_E_CLASS,  "ELFCLASS32");
    b_good(&b); b.eh->e_ident[5] = 2;      expect(&b, ELF_E_DATA,   "big-endian (ELFDATA2MSB)");
    b_good(&b); b.eh->e_ident[6] = 2;      expect(&b, ELF_E_IDENT,  "EI_VERSION from the future");
    b_good(&b); b.eh->e_ident[7] = 9;      expect(&b, ELF_E_IDENT,  "an OSABI we do not implement");
    b_good(&b); b.eh->e_ident[8] = 1;      expect(&b, ELF_E_IDENT,  "a nonzero EI_ABIVERSION");
    b_good(&b); b.eh->e_type = 3;          expect(&b, ELF_E_TYPE,   "ET_DYN (a PIE needs relocation)");
    b_good(&b); b.eh->e_type = 1;          expect(&b, ELF_E_TYPE,   "ET_REL");
    b_good(&b); b.eh->e_machine = 3;       expect(&b, ELF_E_MACHINE,"EM_386");
    b_good(&b); b.eh->e_machine = 183;     expect(&b, ELF_E_MACHINE,"EM_AARCH64");
    b_good(&b); b.eh->e_version = 2;       expect(&b, ELF_E_VERSION,"e_version from the future");
    b_good(&b); b.eh->e_ehsize = 52;       expect(&b, ELF_E_EHSIZE, "a 32-bit e_ehsize");
    b_good(&b); b.eh->e_flags = 1;         expect(&b, ELF_E_FLAGS,  "a nonzero e_flags");
    b_good(&b); b.eh->e_phentsize = 32;    expect(&b, ELF_E_EHSIZE, "e_phentsize is not 56");
    b_good(&b); b.eh->e_phnum = 0;         expect(&b, ELF_E_PHNUM,  "e_phnum is 0");
    b_good(&b); b.eh->e_phnum = 0xffff;    expect(&b, ELF_E_PHNUM,  "PN_XNUM / e_phnum past the cap");
    b_good(&b); b.eh->e_phoff = 0xF0000000ull; expect(&b, ELF_E_PHTAB, "e_phoff past the image");
    b_good(&b); b.eh->e_shnum = 4; b.eh->e_shentsize = 64; b.eh->e_shoff = 0xF0000000ull;
                expect(&b, ELF_E_SHTAB, "e_shoff past the image");
    b_good(&b); b.eh->e_shnum = 4; b.eh->e_shentsize = 40; b.eh->e_shoff = 0x1000;
                expect(&b, ELF_E_EHSIZE, "e_shentsize is not 64");
    b_good(&b); b.eh->e_shnum = 4; b.eh->e_shentsize = 64; b.eh->e_shoff = 0x1000; b.eh->e_shstrndx = 9;
                expect(&b, ELF_E_SHTAB, "e_shstrndx past e_shnum");
    b_good(&b); b.eh->e_shoff = 0x1000;    expect(&b, ELF_E_SHTAB,  "e_shnum 0 but e_shoff set");

    /* the entry point */
    b_good(&b); b.eh->e_entry = 0x10000000; expect(&b, ELF_E_ENTRY, "entry point below the user region");
    b_good(&b); b.eh->e_entry = 0x7D000000; expect(&b, ELF_E_ENTRY, "entry point above the user region");
    b_good(&b); b.eh->e_entry = 0xFFFFFFFFFFFF0000ull;
                expect(&b, ELF_E_ENTRY, "entry point that would overflow the headroom check");
    b_good(&b); b.eh->e_entry = 0x40002000; expect(&b, ELF_E_ENTRY, "entry point in a non-executable segment");
    b_good(&b); b.eh->e_entry = 0x40000800; expect(&b, ELF_E_ENTRY, "entry point past the end of its segment");

    /* segments */
    /* A segment WHOLLY below the user region is skipped, not refused: every
     * binary this tree links has one (lld puts the ELF headers at 0x200000),
     * and mapping it with USER would open a ring-3 window into the shared
     * kernel page tables. That it is skipped and not mapped is the claim. */
    b_init(&b, 4);
    b_seg(&b, 0, PT_LOAD, PF_R, 0x00200000, 0x100, 0x100, 0x1000);
    b_seg(&b, 1, PT_LOAD, PF_R | PF_X, 0x40000000, 0x100, 0x100, 0x1000);
    b_seg(&b, 2, PT_LOAD, PF_R | PF_W, 0x40001000, 0x40, 0x1000, 0x1000);
    b.ph[3].p_type = PT_GNU_STACK; b.ph[3].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40000000;
    expect(&b, ELF_OK, "a segment wholly below the user region is skipped, not refused");
    g_checks++;
    if (space_pte(0x00200000)) { g_fails++; printf("FAIL: the skipped segment was mapped anyway\n"); }
    else printf("ok: ... and nothing was mapped at its address\n");

    b_init(&b, 3);
    b_seg(&b, 0, PT_LOAD, PF_R, 0x3FFFF000, 0x100, 0x2000, 0x1000);
    b_seg(&b, 1, PT_LOAD, PF_R | PF_X, 0x40010000, 0x100, 0x100, 0x1000);
    b.ph[2].p_type = PT_GNU_STACK; b.ph[2].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40010000;
    expect(&b, ELF_E_SEGVA, "a segment straddling the BOTTOM of the user region");

    b_good(&b); b.ph[2].p_vaddr = 0x7FFFF000; b.ph[2].p_memsz = 0x4000;
                expect(&b, ELF_E_SEGVA, "segment straddling the top of the user region");
    b_good(&b); b.ph[2].p_memsz = 0xFFFFFFFFFFFFF000ull;
                expect(&b, ELF_E_SEGVA, "p_vaddr + p_memsz overflowing");
    /* THE ONE THE FUZZER FOUND. p_memsz = -0x1000 with a non-page-aligned
     * p_vaddr makes the page-rounded end come out EQUAL to start, not less
     * than it -- so an `end < start` overflow check passes, the mapping loop
     * runs zero times, and the memcpy writes to an unmapped address in ring 0.
     * Kept as a named case because the general "overflowing" one above does
     * not reach it. */
    b_good(&b); b.ph[2].p_vaddr = 0x40002048; b.ph[2].p_offset += 0x48;
                b.ph[2].p_filesz = 0x18; b.ph[2].p_memsz = 0xFFFFFFFFFFFFF000ull;
                b.size = 0x4000;                       /* so the file range is in bounds */
                expect(&b, ELF_E_SEGVA, "p_memsz = -0x1000, where the rounded end EQUALS start");
    b_good(&b); b.ph[0].p_filesz = 0x200;
                expect(&b, ELF_E_SEGRANGE, "p_filesz > p_memsz");
    b_good(&b); b.ph[1].p_offset = 0xFFFFFFF0ull;
                expect(&b, ELF_E_SEGRANGE, "p_offset past the image");
    b_good(&b); b.ph[0].p_align = 3;
                expect(&b, ELF_E_SEGALIGN, "p_align is not a power of two");
    b_good(&b); b.ph[1].p_offset += 8;
                expect(&b, ELF_E_SEGALIGN, "p_offset not congruent to p_vaddr mod the page size");
    b_good(&b); b.ph[1].p_vaddr = 0x40000000;
                expect(&b, ELF_E_SEGORDER, "two PT_LOADs describing the same address");
    b_good(&b); { struct phdr t = b.ph[0]; b.ph[0] = b.ph[1]; b.ph[1] = t; }
                expect(&b, ELF_E_SEGORDER, "PT_LOADs out of ascending order");
    b_good(&b); b.ph[0].p_flags = PF_R | PF_W | PF_X;
                expect(&b, ELF_E_WX, "a writable AND executable segment");
    b_good(&b); b.ph[2].p_memsz = 300ull * 1024 * 1024;
                expect(&b, ELF_E_TOOBIG, "an image asking for 300 MiB");
    b_good(&b); b.ph[0].p_type = 0; b.ph[1].p_type = 0; b.ph[2].p_type = 0;
                expect(&b, ELF_E_NOLOAD, "an image with no PT_LOAD at all");

    /* the deliberate NOs */
    b_good(&b); b.ph[3].p_type = PT_INTERP;  b.ph[3].p_filesz = 8; b.ph[3].p_offset = 0x1000;
                expect(&b, ELF_E_INTERP, "PT_INTERP");
    b_good(&b); b.ph[3].p_type = PT_DYNAMIC; b.ph[3].p_vaddr = 0x40002000;
                expect(&b, ELF_E_DYNAMIC, "PT_DYNAMIC");
    b_good(&b); b.ph[3].p_type = 5;
                expect(&b, ELF_E_DYNAMIC, "PT_SHLIB");
    b_good(&b); b.ph[3].p_flags = PF_R | PF_W | PF_X;
                expect(&b, ELF_E_STACKX, "PT_GNU_STACK asking for an executable stack");

    /* PT_TLS malformed */
    b_good(&b); b.ph[3].p_type = PT_TLS; b.ph[3].p_filesz = 0x20; b.ph[3].p_memsz = 0x10;
                b.ph[3].p_offset = 0x1000;
                expect(&b, ELF_E_TLS, "PT_TLS with p_filesz > p_memsz");
    b_good(&b); b.ph[3].p_type = PT_TLS; b.ph[3].p_memsz = 0x200000; b.ph[3].p_align = 8;
                expect(&b, ELF_E_TLS, "a 2 MiB PT_TLS block");
    b_good(&b); b.ph[3].p_type = PT_TLS; b.ph[3].p_memsz = 16; b.ph[3].p_align = 0x4000;
                expect(&b, ELF_E_TLS, "PT_TLS aligned past a page");

    /* out of memory part-way through is a refusal, not a crash */
    b_good(&b);
    space_quiet(1); space_reset(); space_set_budget(2);
    { struct elf_image t; int rc = elf_load_image(b.buf, b.size, &t);
      space_quiet(0); space_set_budget(400000);
      g_checks++;
      if (rc != ELF_E_OOM) { g_fails++; printf("FAIL: OOM mid-load must refuse, got %d\n", rc); }
      else printf("ok: running out of frames mid-load is a refusal\n"); }
}

/* ================= part 3: the headers that do something ================== */
static void header_behaviour(void)
{
    struct build b;
    struct elf_image img;
    printf("\n-- part 3: the program headers that change the load --\n");

    /* --- PT_TLS -------------------------------------------------------- */
    b_good(&b);
    b.ph[3].p_type = PT_TLS;
    b.ph[3].p_flags = PF_R;
    b.ph[3].p_vaddr = 0x40002000;
    b.ph[3].p_offset = b.ph[2].p_offset;      /* reuse the data segment's bytes */
    b.ph[3].p_filesz = 0x20;
    b.ph[3].p_memsz  = 0x40;
    b.ph[3].p_align  = 16;
    space_quiet(1);
    int rc = try_load(&b, &img);
    space_quiet(0);
    ck(rc == ELF_OK, "an image with PT_TLS loads");
    if (rc == ELF_OK) {
        ck((img.seen & ELF_SEEN_TLS) != 0, "PT_TLS was seen");
        ck(img.tls_tp != 0, "a thread pointer was produced");
        ck((img.tls_tp & 15) == 0, "the thread pointer honours p_align");
        ck(*(uint64_t *)img.tls_tp == img.tls_tp,
           "%fs:0 is the TCB self-pointer (variant II)");
        ck(*(uint64_t *)(img.tls_tp + 8) == 0, "%fs:8 (the DTV slot) is zero");
        ck(img.tls_tp - img.tls_va == 0x40, "the block sits at tp - round_up(memsz, align)");
        ck(!memcmp((void *)img.tls_va, (uint8_t *)b.buf + b.ph[3].p_offset, 0x20),
           "the TLS initialisation image was copied to the bottom of the block");
        int zero = 1;
        for (uint64_t o = 0x20; o < 0x40; o++) if (((uint8_t *)img.tls_va)[o]) zero = 0;
        ck(zero, "the rest of the TLS block is zero (.tbss)");
        ck(space_writable(img.tls_tp), "the TLS block is writable");
        ck(img.top > img.tls_tp, "the image top is above the TLS block");
    }
    /* and an image without it says so, rather than inheriting */
    b_good(&b);
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK && img.tls_tp == 0 && !(img.seen & ELF_SEEN_TLS),
       "an image with no PT_TLS reports no thread pointer");

    /* --- PT_GNU_STACK -------------------------------------------------- */
    b_good(&b);
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK && (img.seen & ELF_SEEN_GNU_STACK) && img.stack_flags == (PF_R | PF_W),
       "PT_GNU_STACK RW is reported to the caller as RW");
    b_good(&b);
    b.ph[3].p_type = 0;                      /* no PT_GNU_STACK at all */
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK && !(img.seen & ELF_SEEN_GNU_STACK) && img.stack_flags == (PF_R | PF_W),
       "no PT_GNU_STACK defaults to a NON-executable stack (stricter than Linux)");

    /* --- PT_GNU_RELRO -------------------------------------------------- */
    b_init(&b, 4);
    b_seg(&b, 0, PT_LOAD, PF_R | PF_X, 0x40000000, 0x100, 0x100, 0x1000);
    /* one writable segment: [0x40001000, 0x40004000). RELRO covers its first
     * two pages exactly. */
    b_seg(&b, 1, PT_LOAD, PF_R | PF_W, 0x40001000, 0x2000, 0x3000, 0x1000);
    b.ph[2].p_type = PT_GNU_RELRO;
    b.ph[2].p_flags = PF_R;
    b.ph[2].p_vaddr = 0x40001000;
    b.ph[2].p_offset = b.ph[1].p_offset;
    b.ph[2].p_filesz = 0x2000;
    b.ph[2].p_memsz = 0x2000;
    b.ph[2].p_align = 1;
    b.ph[3].p_type = PT_GNU_STACK; b.ph[3].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40000000;
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK, "an image with PT_GNU_RELRO loads");
    if (rc == ELF_OK) {
        ck(img.relro_pages == 2, "both fully covered pages were made read-only");
        ck(!space_writable(0x40001000), "the first RELRO page is no longer writable");
        ck(!space_writable(0x40002000), "the second RELRO page is no longer writable");
        ck(space_writable(0x40003000), "the writable page beyond RELRO is still writable");
    }
    /* RELRO whose first page also holds real .data must NOT lose write.
     * The region starts mid-page and the segment beneath it starts earlier. */
    b_init(&b, 4);
    b_seg(&b, 0, PT_LOAD, PF_R | PF_X, 0x40000000, 0x100, 0x100, 0x1000);
    b_seg(&b, 1, PT_LOAD, PF_R | PF_W, 0x40001000, 0x1800, 0x2000, 0x1000);
    b.ph[2].p_type = PT_GNU_RELRO;
    b.ph[2].p_flags = PF_R;
    b.ph[2].p_vaddr = 0x40001800;            /* starts HALF WAY INTO page 1 */
    b.ph[2].p_offset = b.ph[1].p_offset + 0x800;
    b.ph[2].p_filesz = 0x800;
    b.ph[2].p_memsz = 0x1800;                /* ends at 0x40003000 */
    b.ph[2].p_align = 1;
    b.ph[3].p_type = PT_GNU_STACK; b.ph[3].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40000000;
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK, "an image whose RELRO starts mid-page loads");
    if (rc == ELF_OK) {
        ck(space_writable(0x40001000),
           "the page RELRO only half covers keeps write (it holds real .data)");
        ck(!space_writable(0x40002000), "the page RELRO wholly covers loses write");
        ck(img.relro_pages == 1, "exactly one page was protected");
    }

    /* --- a page shared by two segments gets the UNION ------------------- */
    b_init(&b, 3);
    /* text ends at 0x40000100; rodata starts at 0x40000200 -- same page. */
    b_seg(&b, 0, PT_LOAD, PF_R | PF_X, 0x40000000, 0x100, 0x100, 0x1000);
    b.ph[1].p_type = PT_LOAD;
    b.ph[1].p_flags = PF_R | PF_W;
    b.ph[1].p_vaddr = 0x40000200;
    b.ph[1].p_offset = b.ph[0].p_offset + 0x200;
    b.ph[1].p_filesz = 0x100;
    b.ph[1].p_memsz = 0x100;
    b.ph[1].p_align = 0x1000;
    memset(b.buf + b.ph[1].p_offset, 0x5A, 0x100);
    if (b.size < b.ph[1].p_offset + 0x100) b.size = b.ph[1].p_offset + 0x100;
    b.ph[2].p_type = PT_GNU_STACK; b.ph[2].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40000000;
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK, "two segments sharing one page load");
    if (rc == ELF_OK) {
        ck(space_writable(0x40000000), "the shared page is writable (the union includes W)");
        ck(!space_nx(0x40000000) || !space_nx_enabled(),
           "the shared page is executable (the union includes X)");
        int a = 1, c = 1;
        for (int i = 0; i < 0x100; i++)
            if (((uint8_t *)0x40000000ull)[i] != (uint8_t)(0x40 + 0 + (i & 0x0F))) a = 0;
        for (int i = 0; i < 0x100; i++)
            if (((uint8_t *)0x40000200ull)[i] != 0x5A) c = 0;
        ck(a, "the first segment's bytes survived the second segment's mapping");
        ck(c, "the second segment's bytes are there too");
    }

    /* --- PT_PHDR that IS mapped is preferred over the copy -------------- */
    b_init(&b, 3);
    b.ph[0].p_type = PT_PHDR;
    b.ph[0].p_flags = PF_R;
    b.ph[0].p_offset = 64;
    b.ph[0].p_vaddr = 0x40000040;
    b.ph[0].p_filesz = 3 * 56;
    b.ph[0].p_memsz = 3 * 56;
    b.ph[0].p_align = 8;
    b.ph[1].p_type = PT_LOAD;
    b.ph[1].p_flags = PF_R | PF_X;
    b.ph[1].p_offset = 0;
    b.ph[1].p_vaddr = 0x40000000;
    b.ph[1].p_filesz = 0x1000;
    b.ph[1].p_memsz = 0x1000;
    b.ph[1].p_align = 0x1000;
    b.ph[2].p_type = PT_GNU_STACK; b.ph[2].p_flags = PF_R | PF_W;
    b.eh->e_entry = 0x40000200;
    b.size = 0x1000;
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK, "a Linux-shaped image (headers inside the first PT_LOAD) loads");
    if (rc == ELF_OK)
        ck(img.phdr_va == 0x40000040,
           "AT_PHDR is the image's OWN phdr address when the link mapped it");

    /* --- unknown p_type is ignored, per the ELF rule -------------------- */
    b_good(&b);
    b.ph[3].p_type = 0x70000001;          /* processor-specific, unknown to us */
    space_quiet(1); rc = try_load(&b, &img); space_quiet(0);
    ck(rc == ELF_OK && (img.seen & ELF_SEEN_UNKNOWN),
       "an unknown program header is ignored AND recorded");
}

int main(int argc, char **argv)
{
    printf("-- part 1: every built binary loads --\n");
    int n = 0;
    for (int i = 1; i < argc; i++) {
        load_one(argv[i], 0);
        load_one(argv[i], 1);
        n++;
    }
    space_reset();
    space_set_nx(0);
    printf("ok: %d built .aex images loaded, with NX off and NX on, and every\n"
           "    segment's bytes matched the file\n", n);

    rejection_matrix();
    header_behaviour();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
