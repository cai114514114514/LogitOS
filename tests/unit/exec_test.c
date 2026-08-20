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

/* ================= part 4: the AEX container ============================== */
static uint8_t *g_v2;  static long g_v2n;

/* Copy the reference v2 file, hand the copy to a mutator, and require the given
 * code. Everything about the container is tested this way -- against a REAL
 * file the build produced, not a synthetic one -- because the thing most worth
 * catching is a disagreement between tools/mkaex.py and c/kernel/exec/aex.c,
 * and two hand-written structures cannot disagree with each other. */
typedef void (*mutate)(uint8_t *p, long *n);

static void aex_case(mutate f, int want, const char *what)
{
    uint8_t *c = malloc((size_t)g_v2n + 64);
    memcpy(c, g_v2, (size_t)g_v2n);
    long n = g_v2n;
    if (f) f(c, &n);
    space_quiet(1);
    space_msgs_reset();
    struct aex_info in;
    int rc = aex_parse(c, (uint64_t)n, &in);
    int msgs = space_msgs();
    char last[512];
    snprintf(last, sizeof last, "%s", space_last_msg());
    space_quiet(0);
    free(c);
    g_checks++;
    if (rc != want) { g_fails++; printf("FAIL: %s -- got %d, want %d (%s)\n", what, rc, want, last); return; }
    if (want != AEX_OK && msgs == 0) { g_fails++; printf("FAIL: %s -- refused silently\n", what); return; }
    if (want == AEX_OK) printf("ok: %s\n", what);
    else {
        size_t l = strlen(last);
        while (l && (last[l-1] == '\n' || last[l-1] == '\r')) last[--l] = 0;
        printf("ok: %s -> %s\n", what, last);
    }
}

static void m_trunc32(uint8_t *p, long *n)   { (void)p; *n = 32; }
static void m_magic(uint8_t *p, long *n)     { (void)n; p[2] = 'Z'; }
static void m_ver0(uint8_t *p, long *n)      { (void)n; p[4] = 0; p[5] = 0; }
static void m_verfuture(uint8_t *p, long *n) { (void)n; p[4] = 3; p[5] = 0; }
static void m_verwild(uint8_t *p, long *n)   { (void)n; p[4] = 0xFF; p[5] = 0xFF; }
static void m_flags(uint8_t *p, long *n)     { (void)n; p[6] = 0; p[7] = 0x80; }
static void m_arch(uint8_t *p, long *n)      { (void)n; p[56] = 2; }
static void m_abi(uint8_t *p, long *n)       { (void)n; p[57] = 7; }
static void m_hdrsmall(uint8_t *p, long *n)  { (void)n; p[52] = 60; p[53] = 0; }
/* Past AEX_HDR_MAX, which is 16384 since the alignment pad landed (mkaex.py's
 * HDR_ALIGN comment): 0x1008 = 4104 is now a perfectly legal hdr_size and this
 * case would have gone on passing for the WRONG reason -- hdr_size 4104 is not
 * 8-aligned-and-past-the-cap, it is past the END of a small reference file, so
 * the same AEX_E_HDRSIZE would have come back from a different check. */
static void m_hdrbig(uint8_t *p, long *n)    { (void)n; p[52] = 0x08; p[53] = 0x40; }
static void m_hdrodd(uint8_t *p, long *n)    { (void)n; p[52] = 68 + 1; p[53] = 0; }
/* hdr_size 3840 with only 100 bytes of file: past the END, which is a different
 * check from past the CAP. */
static void m_hdrpast(uint8_t *p, long *n)   { *n = 100; p[52] = 0x00; p[53] = 0x0F; }
static void m_elfzero(uint8_t *p, long *n)   { (void)n; p[60] = p[61] = p[62] = p[63] = 0; }
static void m_elfbig(uint8_t *p, long *n)
{ (void)n; p[60] = 0; p[61] = 0; p[62] = 0xFF; p[63] = 0xFF; }
static void m_crcbit(uint8_t *p, long *n)
{ uint16_t h = (uint16_t)(p[52] | (p[53] << 8)); p[h + 0x100] ^= 0x01; (void)n; }
static void m_tlvpast(uint8_t *p, long *n)   { (void)n; p[64 + 4] = 0xF0; p[64 + 5] = 0x0F; }
static void m_tlvcrclen(uint8_t *p, long *n) { (void)n; p[64 + 4] = 5; }
/* The metadata region is [CRC: 8+4 -> padded to 16][APPID: 8+len -> padded].
 * So the second record starts at 64 + 16 = 80. */
#define T2 (64 + 16)
static void m_unknown_tag(uint8_t *p, long *n)
{ (void)n; p[T2 + 0] = 'Z'; p[T2 + 1] = 'Z'; p[T2 + 2] = 'Z'; p[T2 + 3] = 'Z'; }
static void m_appid_nonul(uint8_t *p, long *n)
{
    (void)n;
    uint32_t len = (uint32_t)p[T2 + 4] | ((uint32_t)p[T2 + 5] << 8);
    p[T2 + 8 + len - 1] = 'x';            /* clobber the terminator */
}
static void m_nocrc(uint8_t *p, long *n)
{ (void)n; p[64] = 'Z'; p[65] = 'Z'; p[66] = 'Z'; p[67] = 'Z'; }  /* CRC -> unknown tag */

static void container(void)
{
    printf("\n-- part 4: the AEX container --\n");
    const char *v2 = getenv("EXEC_V2");
    const char *v1 = getenv("EXEC_V1");
    const char *em = getenv("EXEC_EMIT");
    if (!v2) { printf("(EXEC_V2 unset -- container tests skipped)\n"); return; }
    g_v2 = slurp(v2, &g_v2n);
    if (!g_v2) { printf("FAIL: cannot read %s\n", v2); g_fails++; return; }

    /* The reference file, and what it says. This is also the assertion that
     * tools/mkaex.py and c/kernel/exec/aex.c agree about the layout: the CRC
     * python computed over the ELF has to be the CRC the kernel computes. */
    struct aex_info in;
    space_quiet(1);
    int rc = aex_parse(g_v2, (uint64_t)g_v2n, &in);
    space_quiet(0);
    ck(rc == AEX_OK, "a v2 file the build produced parses");
    if (rc == AEX_OK) {
        ck(in.version == 2, "it says version 2");
        ck(in.arch == AEX_ARCH_X86_64 && in.abi == AEX_ABI_LOGIT1,
           "it names its machine and its syscall ABI");
        ck(in.hdr_size > AEX_HDR_SIZE, "it carries a metadata region");
        ck(in.elf_size == (uint32_t)(g_v2n - in.hdr_size),
           "elf_size accounts for every byte after the header");
        ck(in.crc32 != 0, "the CRC-32 the kernel computed matches the one mkaex.py wrote");
        ck(in.app_id && in.app_id[0], "it carries a stable app id");
        ck((in.flags & (AEX_F_GUI | AEX_F_CLI)) != 0, "it says whether it is a GUI or CLI program");
        printf("    (id=%s flags=0x%x cat=%u hdr=%u elf=%u crc=0x%08x)\n",
               in.app_id, in.flags, in.category, in.hdr_size, in.elf_size, in.crc32);
    }

    aex_case(0,             AEX_OK,         "the unmutated reference file");
    aex_case(m_trunc32,     AEX_E_SHORT,    "a truncated header");
    aex_case(m_magic,       AEX_E_MAGIC,    "a bad magic");
    aex_case(m_ver0,        AEX_E_VERSION,  "version 0");
    aex_case(m_verfuture,   AEX_E_VERSION,  "a version from the future");
    aex_case(m_verwild,     AEX_E_VERSION,  "version 65535");
    aex_case(m_flags,       AEX_E_FLAGS,    "a flag bit this loader does not know");
    aex_case(m_arch,        AEX_E_ARCH,     "built for another machine");
    aex_case(m_abi,         AEX_E_ARCH,     "built against another syscall ABI");
    aex_case(m_hdrsmall,    AEX_E_HDRSIZE,  "hdr_size below the fixed header");
    aex_case(m_hdrbig,      AEX_E_HDRSIZE,  "hdr_size past the cap");
    aex_case(m_hdrodd,      AEX_E_HDRSIZE,  "hdr_size not 8-aligned");
    aex_case(m_hdrpast,     AEX_E_HDRSIZE,  "hdr_size past the end of the file");
    aex_case(m_elfzero,     AEX_E_ELFSIZE,  "elf_size of 0");
    aex_case(m_elfbig,      AEX_E_ELFSIZE,  "elf_size larger than the file");
    aex_case(m_crcbit,      AEX_E_CRC,      "one bit flipped inside the ELF");
    aex_case(m_tlvpast,     AEX_E_TLV,      "a metadata record running past the header");
    aex_case(m_tlvcrclen,   AEX_E_TLV,      "a CRC record that is not 4 bytes");
    aex_case(m_appid_nonul, AEX_E_TLV,      "an app id with no NUL terminator");
    aex_case(m_nocrc,       AEX_E_NOCRC,    "a v2 file with no integrity record");
    aex_case(m_unknown_tag, AEX_OK,         "an unknown metadata tag is ignored, not refused");

    /* THE NEGATIVE CONTROL FOR THE FORMAT CHANGE. An old-format file must be
     * refused or migrated DELIBERATELY, never silently mis-loaded. It is
     * migrated: accepted with v1 rules, and the loader says on the log that it
     * has no integrity record. The message is the deliberate part, so the
     * message is what is asserted -- not just the return code. */
    if (v1) {
        long n = 0;
        uint8_t *f = slurp(v1, &n);
        if (!f) { printf("FAIL: cannot read %s\n", v1); g_fails++; }
        else {
            space_quiet(1);
            struct aex_info i1;
            uint32_t before = aex_v1_images();
            int r1 = aex_parse(f, (uint64_t)n, &i1);
            uint32_t after = aex_v1_images();
            space_quiet(0);
            ck(r1 == AEX_OK, "a v1 file still loads");
            ck(i1.version == 1 && i1.hdr_size == AEX_HDR_SIZE,
               "... under v1 rules: a 64-byte header and the ELF at +64");
            ck(i1.crc32 == 0 && i1.elf_size == (uint32_t)(n - 64),
               "... with no integrity record, and elf_size taken as the rest of the file");
            /* The deliberate part of the migration is that the loader NOTICES.
             * Asserted on the counter rather than on the log line, because the
             * line is printed once per boot on purpose (a disk full of v1 files
             * would otherwise bury everything else on the serial log) and part 1
             * has already loaded this very file. A counter is order-independent,
             * which a "did it print" check is not. */
            ck(after == before + 1,
               "... and the loader COUNTED it as a v1 image, which is what makes "
               "this a migration and not a silent mis-load");
            { uint32_t b2 = aex_v1_images();
              space_quiet(1); aex_parse(g_v2, (uint64_t)g_v2n, 0); space_quiet(0);
              ck(aex_v1_images() == b2, "... and a v2 file does not count as one"); }
            /* Same bytes, v1 header, but nothing after it. */
            space_quiet(1);
            int r2 = aex_parse(f, 64, 0);
            space_quiet(0);
            ck(r2 == AEX_E_ELFSIZE, "a v1 header with no ELF behind it is refused");
            free(f);
        }
    }

    /* A .aex a compiler could have emitted: no linker was involved. */
    if (em) {
        long n = 0;
        uint8_t *f = slurp(em, &n);
        if (!f) { printf("FAIL: cannot read %s\n", em); g_fails++; }
        else {
            space_quiet(1);
            struct elf_image img;
            space_reset(); space_set_nx(1);
            int r = aex_load_image(f, (uint64_t)n, 0, 0, &img);
            space_quiet(0);
            ck(r == 0, "a .aex built by mkaex --emit from a FLAT binary loads");
            if (r == 0) {
                ck(img.entry == 0x50000000, "its entry point is the base it was told to use");
                ck(!space_writable(0x50000000), "its text is not writable");
                ck(space_nx(0x50000000) == 0, "its text IS executable");
                ck(img.phdr_va != 0 && img.random_va != 0,
                   "it gets the same auxv material as a linked binary");
                ck((img.stack_flags & PF_X) == 0, "its PT_GNU_STACK is non-executable");
            }
            space_reset(); space_set_nx(0);
            free(f);
        }
    }
    free(g_v2);
}

/* ===========================================================================
 * part 5: elf_file_runs() -- which pages may be mapped out of the file
 *
 * This is a PURE function (elf.h states the five conditions), so unlike every
 * other part of this file it can be driven by BUILDING program headers rather
 * than by loading an image: the whole decision table is reachable, including
 * the cases no linker in this tree emits. That is the point of it being pure.
 *
 * What is at stake here is not "does it map fewer frames". It is that every
 * page in a returned run is a page whose bytes the process may see UNCHANGED
 * FROM THE FILE and SHARED WITH EVERY OTHER PROCESS running the same binary.
 * A run that is one page too long at the end covers the p_filesz < p_memsz
 * boundary page -- part file, part .bss -- and the program's zero-initialised
 * data silently starts out holding the file's next bytes. Nothing crashes.
 * -DELF_NEGCTL_FILETAIL is exactly that mistake, and these cases must fail
 * against it. */
#define PGSZ 4096ull

static struct elf64_phdr mkload(uint64_t off, uint64_t va, uint64_t fs,
                                uint64_t ms, uint32_t fl)
{
    struct elf64_phdr p;
    memset(&p, 0, sizeof p);
    p.p_type = 1;                 /* PT_LOAD */
    p.p_flags = fl;
    p.p_offset = off; p.p_vaddr = va; p.p_paddr = va;
    p.p_filesz = fs; p.p_memsz = ms; p.p_align = PGSZ;
    return p;
}

static void file_runs(void)
{
    printf("\n-- part 5: elf_file_runs(), the file-backed predicate --\n");
    struct elf_run r[ELF_MAX_RUNS];
    const uint64_t HDR = PGSZ;    /* what mkaex.py pads hdr_size to */

    /* A REAL SHAPE, taken from a binary this tree builds: a text segment and a
     * rodata segment whose start is not page aligned, in the layout lld emits.
     * Text 0x45000000 filesz 0x2A5400 (browser.aex, measured), rodata at
     * 0x452A6900 -- the case elf.h names, where the previous segment's memsz
     * ends exactly on a page boundary and this one starts mid-page, so (a)
     * sees ONE segment on the leading partial page and only (c) excludes it. */
    struct elf64_phdr ph[3];
    ph[0] = mkload(0x1000, 0x45000000, 0x2A5400, 0x2A5400, PF_R | PF_X);
    ph[1] = mkload(0x2A6900, 0x452A6900, 0xC2212, 0xC2212, PF_R);
    ph[2] = mkload(0x369000, 0x45369000, 0x2000, 0x800000, PF_R | PF_W);

    int n = elf_file_runs(ph, 3, HDR, 0x1000, r, ELF_MAX_RUNS);
    ck(n == 2, "two runs for a text + rodata + data image (the writable one is not one)");
    if (n == 2) {
        ck(r[0].va == 0x45000000 && r[0].prot == (PF_R | PF_X),
           "the text run starts at the segment's own page, executable");
        ck(r[0].foff == HDR + 0x1000, "and at hdr_size + p_offset in the FILE");
        ck(r[0].va + (r[0].pages << 12) <= 0x45000000 + 0x2A5400,
           "and ends inside p_filesz -- never on the boundary page");
        ck(r[1].va == 0x452A7000,
           "the rodata run skips its LEADING partial page (0x452A6900 rounds up)");
        ck(r[1].foff == HDR + 0x2A7000,
           "and its file offset moves with it, not with p_offset alone");
        ck(r[1].prot == PF_R, "rodata is not executable");
        ck((r[0].foff & 0xFFF) == 0 && (r[1].foff & 0xFFF) == 0,
           "every run's file offset is page aligned -- the whole point");
    }
    /* THE COUNT is the honest headline, and it is what a reader will quote. */
    if (n >= 1) {
        uint64_t tot = 0;
        for (int i = 0; i < n; i++) tot += r[i].pages;
        printf("    (browser-shaped image: %llu pages file-backed across %d runs)\n",
               (unsigned long long)tot, n);
    }

    /* (b) A WRITABLE SEGMENT IS NEVER A RUN. There is no writeback and no
     * private-file COW case; a writable file mapping is refused out loud by
     * mmsys.c and must not arrive by another door. */
    {
        struct elf64_phdr w = mkload(0x1000, 0x50000000, 0x8000, 0x8000, PF_R | PF_W);
        ck(elf_file_runs(&w, 1, HDR, 0x100, r, ELF_MAX_RUNS) == 0,
           "(b) a writable PT_LOAD produces no run, whatever else is true of it");
    }

    /* (c) THE BOUNDARY PAGE. p_filesz stops mid-page and p_memsz runs on: the
     * page holding the transition is part file and part zero and may never be
     * shared. This is the case -DELF_NEGCTL_FILETAIL gets wrong. */
    {
        struct elf64_phdr b = mkload(0x1000, 0x50000000, 0x2800, 0x9000, PF_R);
        int m = elf_file_runs(&b, 1, HDR, 0x100, r, ELF_MAX_RUNS);
        ck(m == 1, "(c) a segment with .bss after it still has a run");
        if (m == 1) {
            ck(r[0].pages == 2,
               "and it is TWO pages, not three: the page where p_filesz ends "
               "is part file and part zero");
            ck(r[0].va + (r[0].pages << 12) == 0x50002000,
               "the run stops at 0x50002000 -- below the boundary page");
        }
        /* Exactly on a page boundary: nothing is partial, and the .bss page
         * that follows is entirely zero, so it is still not in the run. */
        struct elf64_phdr e = mkload(0x1000, 0x50000000, 0x3000, 0x9000, PF_R);
        m = elf_file_runs(&e, 1, HDR, 0x100, r, ELF_MAX_RUNS);
        ck(m == 1 && r[0].pages == 3,
           "with p_filesz landing exactly on a page boundary, all three file "
           "pages qualify and no .bss page does");
    }

    /* (a) TWO SEGMENTS ON ONE PAGE. The page's permission is the UNION of
     * both, which is a thing a shared read-only mapping cannot express, and
     * the bytes come from two places. Constructed so the shared page is at the
     * END of the first segment's run. */
    {
        struct elf64_phdr t[2];
        t[0] = mkload(0x1000, 0x50000000, 0x3000, 0x3400, PF_R | PF_X);
        t[1] = mkload(0x4400, 0x50003400, 0x1000, 0x1000, PF_R);
        int m = elf_file_runs(t, 2, HDR, 0x100, r, ELF_MAX_RUNS);
        ck(m == 1, "(a) the shared page belongs to two segments, so one run survives");
        if (m == 1)
            ck(r[0].pages == 3 && r[0].va == 0x50000000,
               "and it stops before the shared page rather than claiming it");
    }

    /* (d) A v1 .aex: hdr_size 64, so no page of it lines up with a file page.
     * There is no version test anywhere in the loader -- the arithmetic
     * refuses by itself, which is why this is written as a full congruence. */
    {
        struct elf64_phdr v = mkload(0x0, 0x50000000, 0x8000, 0x8000, PF_R | PF_X);
        ck(elf_file_runs(&v, 1, 64, 0x100, r, ELF_MAX_RUNS) == 0,
           "(d) a v1 image (hdr_size 64) produces no run at all -- refused by "
           "arithmetic, not by a version check");
        ck(elf_file_runs(&v, 1, 0, 0x100, r, ELF_MAX_RUNS) == 1,
           "and the same headers at hdr_off 0 (a bare ELF) do produce one");
    }

    /* (e) PAST THE END OF THE FILE. pcache_get() returns 0 past EOF and the
     * fault then kills the process, so a run must be trimmed to the file
     * rather than built and discovered at first touch. */
    {
        struct elf64_phdr s = mkload(0x1000, 0x50000000, 0x8000, 0x8000, PF_R | PF_X);
        int m = elf_file_runs(&s, 1, HDR, 4, r, ELF_MAX_RUNS);   /* file is 4 pages */
        ck(m == 1 && r[0].pages == 2,
           "(e) a run is TRIMMED to the file: 8 pages asked, 2 pages of file "
           "left after the header and the offset");
        ck(elf_file_runs(&s, 1, HDR, 2, r, ELF_MAX_RUNS) == 0,
           "and a run entirely past the end produces nothing rather than a "
           "mapping that faults fatally on first touch");
    }

    /* Outside the private user region. PASS 1 skips the low read-only headers
     * segment lld emits at 0x200000 rather than mapping it -- a USER mapping
     * there is a window into the shared kernel page tables -- so a run over it
     * is one the loader could never use. */
    {
        struct elf64_phdr l = mkload(0x0, 0x200000, 0x4000, 0x4000, PF_R);
        ck(elf_file_runs(&l, 1, HDR, 0x100, r, ELF_MAX_RUNS) == 0,
           "a segment below the private user region produces no run");
    }

    /* Degenerate inputs. The predicate is handed disk-controlled headers
     * before PASS 0 in the host test, so it refuses rather than trusting. */
    {
        struct elf64_phdr z = mkload(0x1000, 0x50000000, 0x800, 0x800, PF_R);
        ck(elf_file_runs(&z, 1, HDR, 0x100, r, ELF_MAX_RUNS) == 0,
           "a segment shorter than a page has no whole page to share");
        struct elf64_phdr o = mkload(0x1000, 0xFFFFFFFFFFFFF000ull,
                                     0xF000, 0xF000, PF_R);
        ck(elf_file_runs(&o, 1, HDR, 0x100, r, ELF_MAX_RUNS) >= 0,
           "a p_vaddr + p_filesz that wraps is refused, not wrapped around");
        ck(elf_file_runs(ph, 3, HDR, 0x1000, r, 0) == 0, "max 0 writes nothing");
        ck(elf_file_runs(0, 3, HDR, 0x1000, r, ELF_MAX_RUNS) == 0, "no headers, no runs");
    }
}

/* The same predicate, against the REAL headers of every .aex the build
 * produced, rather than against headers this file wrote. Two different jobs:
 * the table above proves the decision, this proves it on the shapes lld
 * actually emits -- and prints the number, which is the thing anyone reading
 * this change wants to know and which no hand-built header can honestly give.
 *
 * The three e_* offsets are read raw because struct elf64_ehdr is private to
 * elf.c and exporting it would be a wider change than this needs. They are
 * ELF64 spec constants, not this tree's choices: e_phoff at 0x20 (u64),
 * e_phentsize at 0x36 and e_phnum at 0x38 (u16). elf.c's own PASS 0 checks
 * e_phentsize == 56 for every one of these files, so a wrong offset here shows
 * up as a nonsense count rather than as a silent zero. */
static void file_runs_real(int argc, char **argv)
{
    printf("\n-- part 5b: elf_file_runs() on every built .aex --\n");
    struct elf_run r[ELF_MAX_RUNS];
    int files = 0, backed = 0, eager = 0;
    uint64_t worst_pages = 0;
    char worst[256] = "";

    for (int a = 1; a < argc; a++) {
        long n = 0;
        uint8_t *f = slurp(argv[a], &n);
        if (!f) continue;
        struct aex_info in;
        space_quiet(1);
        int rc = aex_parse(f, (uint64_t)n, &in);
        space_quiet(0);
        if (rc != AEX_OK) { free(f); continue; }
        files++;

        const uint8_t *eh = in.elf;
        uint64_t phoff; uint16_t phent, phnum;
        memcpy(&phoff, eh + 0x20, 8);
        memcpy(&phent, eh + 0x36, 2);
        memcpy(&phnum, eh + 0x38, 2);
        if (phent != sizeof(struct elf64_phdr) || !phnum ||
            phoff + (uint64_t)phnum * phent > in.elf_size) { free(f); continue; }
        const struct elf64_phdr *ph = (const struct elf64_phdr *)(const void *)(eh + phoff);

        uint64_t fpages = ((uint64_t)in.hdr_size + in.elf_size + 4095) / 4096;
        int m = elf_file_runs(ph, phnum, in.hdr_size, fpages, r, ELF_MAX_RUNS);
        uint64_t tot = 0;
        for (int i = 0; i < m; i++) {
            /* The invariants, on every run of every real binary. Each one is a
             * silent wrong answer if it fails, not a crash. */
            ckq((r[i].foff & 0xFFF) == 0, "a real run's file offset is page aligned");
            ckq((r[i].va & 0xFFF) == 0, "a real run's virtual address is page aligned");
            ckq(!(r[i].prot & PF_W), "a real run is never writable");
            ckq(r[i].pages > 0, "a real run is not empty");
            ckq(r[i].foff / 4096 + r[i].pages <= fpages,
                "a real run stays inside the file");
            /* Every byte the run claims must really be in the FILE, at the
             * offset the run says. This is the assertion that catches an
             * off-by-one in the header arithmetic, and it is checked against
             * the bytes rather than against the headers that produced them. */
            for (uint64_t p = 0; p < r[i].pages; p++) {
                uint64_t fo = r[i].foff + p * 4096;
                ckq(fo + 4096 <= (uint64_t)n, "a real run's page is inside the file image");
            }
            tot += r[i].pages;
        }
        if (m) backed++; else eager++;
        if (tot > worst_pages) {
            worst_pages = tot;
            snprintf(worst, sizeof worst, "%s", argv[a]);
        }
    }
    printf("    %d .aex parsed: %d have file-backed runs, %d take the eager path\n",
           files, backed, eager);
    if (worst_pages)
        printf("    largest: %s at %llu pages (%llu KiB of text+rodata shared "
               "rather than copied per process)\n",
               worst, (unsigned long long)worst_pages,
               (unsigned long long)(worst_pages * 4));
    ck(files > 0, "at least one .aex was available to measure");
    ck(backed > 0, "and at least one of them has pages the loader can map "
                   "from its file -- 0 here means the alignment pad is gone");
}

int main(int argc, char **argv)
{
    /* Line-buffered even when stdout is a file. This test is capable of
     * CRASHING -- that is what the overflow negative control does, because the
     * harness maps with the real MMU and a write the loader never mapped is a
     * real fault -- and a fully buffered stdout throws away everything printed
     * before the crash, which is precisely the part that says how far it got. */
    setvbuf(stdout, NULL, _IOLBF, 0);
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
    container();
    file_runs();
    file_runs_real(argc, argv);

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
