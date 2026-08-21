/* Host gate for the module loader's format and relocation core
 * (c/kernel/module/modelf.c). No kernel, no QEMU.
 *
 * WHY A HOST TEST CAN PROVE ANYTHING HERE. modelf.c was deliberately written
 * to call nothing -- no kmalloc, no kprintf, no VFS, no lock -- so the exact
 * code the kernel runs can be compiled into an ordinary Linux program and
 * handed the exact objects the cross-compiler produces. Nothing is
 * re-implemented for the test and nothing is stubbed inside the unit.
 *
 * THE STRONGEST CHECK IN THIS FILE IS THAT IT EXECUTES THE MODULE.
 * Both sides are x86-64 SysV, so a correctly relocated freestanding object is
 * callable from here. Inspecting a relocated pointer only proves the loader
 * wrote a plausible number; calling through it proves it wrote the right one,
 * and calling TWICE proves the .bss it wrote into is real writable memory
 * inside the loaded block. A loader that skipped .data, mixed up two section
 * offsets, or applied PLT32 as an absolute would all pass inspection and fail
 * here on the first instruction.
 *
 * THE ADDRESS IS CHOSEN, NOT ACCEPTED. The module block is mmap'd at a FIXED
 * low address (0x30000000, 768 MiB) because that is where the kernel's is: a
 * kmalloc'd block is identity-mapped physical memory, below 512 MiB on a
 * -m 512M machine. Letting the loader run at whatever address mmap felt like
 * (0x7f..., above 4 GiB) would test a configuration the kernel never has --
 * and it is exactly the configuration test 12 uses on purpose to prove the
 * range check is alive.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "module.h"
#include "driver.h"     /* struct driver / struct dev_match, for the real driver */

static int pass, fail;
static void check(int ok, const char *what)
{
    if (ok) { pass++; }
    else    { fail++; printf("FAIL: %s\n", what); }
}
static void checkf(int ok, const char *what, long got, long want)
{
    if (ok) { pass++; }
    else    { fail++; printf("FAIL: %s (got %ld, want %ld)\n", what, got, want); }
}

/* ------------------------------------------------------- the module's ABI --
 * The two symbols tests/unit/modmod.c leaves undefined. They stand in for
 * kprintf and the dev_* calls a real driver leaves undefined, and they are
 * REAL host functions so the module's calls to them actually run. */
static int  g_ext_calls;
static char g_last_log[64];

int mt_ext(int x) { g_ext_calls++; return x + 1; }
void mt_log(const char *s)
{
    strncpy(g_last_log, s ? s : "(null)", sizeof g_last_log - 1);
    g_last_log[sizeof g_last_log - 1] = 0;
}

static int g_resolve_calls;
static const char *g_last_resolved;

static void *resolve_host(const char *name, void *ctx)
{
    (void)ctx;
    g_resolve_calls++;
    g_last_resolved = name;
    if (!strcmp(name, "mt_ext")) return (void *)(uintptr_t)mt_ext;
    if (!strcmp(name, "mt_log")) return (void *)(uintptr_t)mt_log;
    return NULL;
}

/* The seven names c/drivers/core/qemu_edu.c leaves undefined. Addresses are
 * this test's own functions -- never called, only pointed at -- because what
 * test 14 checks is the relocated struct, not the driver's behaviour. */
static void  fake_enable(struct device *d, int bm)   { (void)d; (void)bm; }
static uint64_t fake_bar(struct device *d, int i)    { (void)d; (void)i; return 0; }
static int   fake_irq_req(struct device *d, irq_handler_t f, void *a, const char *n)
                                                     { (void)d;(void)f;(void)a;(void)n; return -1; }
static void  fake_irq_rel(struct device *d)          { (void)d; }
static int   fake_irq_pref(int m)                    { (void)m; return 0; }
static uint64_t fake_irq_cnt(const struct device *d) { (void)d; return 0; }
static int   fake_printf(const char *f, ...)         { (void)f; return 0; }

static void *resolve_edu(const char *name, void *ctx)
{
    (void)ctx;
    if (!strcmp(name, "dev_enable"))      return (void *)(uintptr_t)fake_enable;
    if (!strcmp(name, "dev_bar_map"))     return (void *)(uintptr_t)fake_bar;
    if (!strcmp(name, "dev_irq_request")) return (void *)(uintptr_t)fake_irq_req;
    if (!strcmp(name, "dev_irq_release")) return (void *)(uintptr_t)fake_irq_rel;
    if (!strcmp(name, "dev_irq_prefer"))  return (void *)(uintptr_t)fake_irq_pref;
    if (!strcmp(name, "dev_irq_count"))   return (void *)(uintptr_t)fake_irq_cnt;
    if (!strcmp(name, "kprintf"))         return (void *)(uintptr_t)fake_printf;
    return NULL;
}

/* Resolves nothing: used to prove MOD_E_UNDEF names the missing symbol. */
static void *resolve_none(const char *name, void *ctx) { (void)name; (void)ctx; return NULL; }

/* ----------------------------------------------------------------- files -- */
static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); fail++; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

/* An INDEPENDENT reading of the same object's relocation table, written from
 * the spec rather than shared with the loader. Its job is to say WHICH
 * relocation types the corpus actually contains -- so that if a compiler
 * upgrade stops emitting one, the test reports the type that vanished instead
 * of silently covering four cases while claiming five. Sharing modelf.c's
 * parser here would make that impossible to detect. */
struct census { int n[16]; int other; };
static void census(const uint8_t *img, uint32_t len, struct census *c)
{
    memset(c, 0, sizeof *c);
    if (len < 64) return;
    uint64_t shoff; uint16_t shnum, shentsize;
    memcpy(&shoff, img + 0x28, 8);
    memcpy(&shentsize, img + 0x3A, 2);
    memcpy(&shnum, img + 0x3C, 2);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (uint64_t)i * shentsize;
        uint32_t type; uint64_t off, size, entsize;
        memcpy(&type, sh + 4, 4);
        memcpy(&off,  sh + 0x18, 8);
        memcpy(&size, sh + 0x20, 8);
        memcpy(&entsize, sh + 0x38, 8);
        if (type != 4 /*SHT_RELA*/ || entsize != 24) continue;
        for (uint64_t r = 0; r + 24 <= size; r += 24) {
            uint64_t info;
            memcpy(&info, img + off + r + 8, 8);
            uint32_t rt = (uint32_t)(info & 0xFFFFFFFFu);
            if (rt < 16) c->n[rt]++; else c->other++;
        }
    }
}

/* ------------------------------------------------------------ mmap helper --
 * MAP_FIXED_NOREPLACE so a collision is a visible error rather than this test
 * silently unmapping something of its own. */
static void *map_at(uintptr_t want, size_t len)
{
    void *p = mmap((void *)want, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if ((uintptr_t)p != want) { munmap(p, len); return NULL; }
    return p;
}

#define LOW_ADDR  0x30000000UL          /* 768 MiB: where a kmalloc block is */
#define HIGH_ADDR 0x400000000UL         /* 16 GiB: where one never is */

int main(int argc, char **argv)
{
    const char *mod_path = (argc > 1) ? argv[1] : "build/modmod.o";
    const char *edu_path = (argc > 2) ? argv[2] : "build/edu_mod.o";

    printf("modreloc: module=%s edu=%s\n", mod_path, edu_path);

    /* ======================================================= 1..9: one reloc
     * mod_reloc_apply in isolation: the arithmetic and the range rule per
     * type. Every case here is a boundary or a refusal, because the interior
     * of each range is covered a thousand times over by the real objects
     * below and adding more of it would only inflate the count. */
    {
        uint8_t buf[16];

        /* 64-bit: no range rule, and the addend is signed. */
        memset(buf, 0xAA, sizeof buf);
        check(mod_reloc_apply(buf, R_X86_64_64, 0x1122334455667788ull, 8) == 0
              && *(uint64_t *)buf == 0x1122334455667790ull, "R_X86_64_64 S+A");

        /* NONE writes nothing. Getting this wrong zeroes 4 bytes of code. */
        memset(buf, 0xAA, sizeof buf);
        check(mod_reloc_apply(buf, R_X86_64_NONE, 0x1000, 0) == 0
              && buf[0] == 0xAA && buf[3] == 0xAA, "R_X86_64_NONE writes nothing");

        /* PC32: S + A - P, where P is the patch address itself. */
        memset(buf, 0, sizeof buf);
        uint64_t P = (uint64_t)(uintptr_t)buf;
        check(mod_reloc_apply(buf, R_X86_64_PC32, P + 0x100, -4) == 0
              && *(int32_t *)buf == 0xFC, "R_X86_64_PC32 arithmetic");

        /* PLT32 must produce the IDENTICAL bytes as PC32 -- treating it as a
         * distinct case is the mistake, since there is no PLT to route via. */
        uint8_t b2[16]; memset(b2, 0, sizeof b2);
        mod_reloc_apply(b2, R_X86_64_PC32,  (uint64_t)(uintptr_t)b2 + 0x2000, -4);
        uint8_t b3[16]; memset(b3, 0, sizeof b3);
        mod_reloc_apply(b3, R_X86_64_PLT32, (uint64_t)(uintptr_t)b3 + 0x2000, -4);
        check(memcmp(b2, b3, 4) == 0, "PLT32 == PC32 byte for byte");

        /* PC32 range: exactly +2^31-1 fits, +2^31 does not. */
        check(mod_reloc_apply(buf, R_X86_64_PC32, P + 2147483647ull, 0) == 0,
              "PC32 +2147483647 accepted");
        check(mod_reloc_apply(buf, R_X86_64_PC32, P + 2147483648ull, 0) == MOD_E_RANGE,
              "PC32 +2147483648 refused");

        /* 32 is ZERO-extended: 0xFFFFFFFF is legal, 0x100000000 is not. */
        check(mod_reloc_apply(buf, R_X86_64_32, 0xFFFFFFFFull, 0) == 0,
              "R_X86_64_32 0xFFFFFFFF accepted");
        check(mod_reloc_apply(buf, R_X86_64_32, 0x100000000ull, 0) == MOD_E_RANGE,
              "R_X86_64_32 0x100000000 refused");

        /* 32S is SIGN-extended, so it refuses at half of what 32 accepts. The
         * two sharing one range test is the plausible bug and this is the
         * only case that separates them. */
        check(mod_reloc_apply(buf, R_X86_64_32S, 0x7FFFFFFFull, 0) == 0,
              "R_X86_64_32S 0x7FFFFFFF accepted");
        check(mod_reloc_apply(buf, R_X86_64_32S, 0x80000000ull, 0) == MOD_E_RANGE,
              "R_X86_64_32S 0x80000000 refused (where 32 accepts it)");

        /* An unknown type is refused, not skipped. R_X86_64_GOTPCREL (9) is
         * the realistic one to meet next: it is what -fpic emits. */
        check(mod_reloc_apply(buf, 9, 0x1000, 0) == MOD_E_RELOC,
              "unknown relocation type refused by name, not ignored");
    }

    /* ================================================== 10: the type census
     * What the corpus actually contains. Asserted so a compiler change that
     * stops emitting a type cannot silently shrink this test's coverage. */
    uint32_t mlen = 0;
    uint8_t *mimg = slurp(mod_path, &mlen);
    if (!mimg) { printf("modreloc: %d passed, %d FAILED\n", pass, fail); return 1; }
    {
        struct census c;
        census(mimg, mlen, &c);
        printf("  census: 64=%d PC32=%d PLT32=%d 32=%d 32S=%d other=%d\n",
               c.n[1], c.n[2], c.n[4], c.n[10], c.n[11], c.other);
        check(c.n[R_X86_64_64] > 0 && c.n[R_X86_64_PC32] > 0 &&
              c.n[R_X86_64_PLT32] > 0 && c.n[R_X86_64_32] > 0 &&
              c.n[R_X86_64_32S] > 0 && c.other == 0,
              "the test module exercises all five relocation types and no others");
    }

    /* ================================ 11: size agrees with load, and EXECUTE */
    long need = mod_elf_size(mimg, mlen);
    check(need > 0, "mod_elf_size accepts the module");
    if (need > 0) {
        size_t maplen = ((size_t)need + 4095) & ~(size_t)4095;
        void *blk = map_at(LOW_ADDR, maplen);
        if (!blk) {
            printf("FAIL: could not map at %#lx (ASLR collision?)\n", LOW_ADDR);
            fail++;
        } else {
            /* Poison, so a section the loader forgets to write is visibly
             * garbage rather than accidentally-zero. .bss is the one that
             * MUST come out zero, and against a zeroed block that assertion
             * would pass without the loader doing anything. */
            memset(blk, 0x5A, maplen);

            struct mod_layout lay; const char *undef = NULL;
            int e = mod_elf_load(mimg, mlen, blk, (uint32_t)need,
                                 resolve_host, NULL, &lay, &undef);
            checkf(e == 0, "mod_elf_load at a low address succeeds", e, 0);

            if (e == 0) {
                check(lay.text_size > 0 && lay.text_off < (uint32_t)need,
                      "a text section was located");
                check(lay.drv_start && lay.drv_stop &&
                      (uint8_t *)lay.drv_stop - (uint8_t *)lay.drv_start == 8,
                      "the module's logit_drivers section was found by NAME");
                check(g_resolve_calls >= 2, "both undefined symbols were resolved");

                /* The section named logit_drivers holds a relocated pointer to
                 * the module's own g_marker. This is the DRIVER_DECLARE
                 * mechanism, checked by dereferencing it. */
                int *marker = *(int **)lay.drv_start;
                check((uint8_t *)marker >= (uint8_t *)blk &&
                      (uint8_t *)marker < (uint8_t *)blk + need,
                      "the logit_drivers pointer points INSIDE the module block");
                checkf(marker && *marker == 0xC0FFEE,
                       "and dereferences to the right object", marker ? *marker : -1,
                       0xC0FFEE);

                /* --- EXECUTE. Everything above is inspection; this is proof. */
                typedef int (*entry_fn)(int, int);
                typedef const char *(*msg_fn)(void);
                entry_fn mt_entry = NULL; msg_fn mt_msg = NULL;

                /* The two entry points are found by walking the symbol table
                 * the same way a loader with a module_init would. Kept in the
                 * test rather than in modelf.c because the kernel loader does
                 * NOT need it -- it enters through logit_drivers, not through
                 * a named entry symbol -- and adding an unused symbol-lookup
                 * path to the kernel to make a test easier is how dead code
                 * gets in. */
                {
                    uint64_t shoff; uint16_t shnum, shent;
                    memcpy(&shoff, mimg + 0x28, 8);
                    memcpy(&shent, mimg + 0x3A, 2);
                    memcpy(&shnum, mimg + 0x3C, 2);
                    for (uint16_t i = 0; i < shnum; i++) {
                        const uint8_t *sh = mimg + shoff + (uint64_t)i * shent;
                        uint32_t t; memcpy(&t, sh + 4, 4);
                        if (t != 2 /*SHT_SYMTAB*/) continue;
                        uint64_t off, size; uint32_t link;
                        memcpy(&off, sh + 0x18, 8); memcpy(&size, sh + 0x20, 8);
                        memcpy(&link, sh + 0x28, 4);
                        const uint8_t *strsh = mimg + shoff + (uint64_t)link * shent;
                        uint64_t stroff; memcpy(&stroff, strsh + 0x18, 8);
                        const char *str = (const char *)(mimg + stroff);
                        for (uint64_t s = 0; s + 24 <= size; s += 24) {
                            const uint8_t *sym = mimg + off + s;
                            uint32_t nm; memcpy(&nm, sym, 4);
                            uint16_t shndx; memcpy(&shndx, sym + 6, 2);
                            uint64_t val; memcpy(&val, sym + 8, 8);
                            if (shndx == 0 || shndx >= 0xff00) continue;
                            /* Section base = (its placed address). Recovered
                             * from the loader's own output by reading the text
                             * section's offset, which is all these two symbols
                             * need since both are functions. */
                            if (!strcmp(str + nm, "mt_entry"))
                                mt_entry = (entry_fn)(uintptr_t)
                                    ((uint8_t *)blk + lay.text_off + val);
                            if (!strcmp(str + nm, "mt_msg"))
                                mt_msg = (msg_fn)(uintptr_t)
                                    ((uint8_t *)blk + lay.text_off + val);
                        }
                    }
                }

                check(mt_entry != NULL && mt_msg != NULL,
                      "the module's entry symbols were located");

                if (mt_entry && mt_msg) {
                    g_ext_calls = 0; g_last_log[0] = 0;

                    /* mt_entry(3,4) = mt_ext(12) + g_counter + g_table[0]
                     *               = 13 + 3 + 10 = 26   (g_counter was 0) */
                    int r1 = mt_entry(3, 4);
                    checkf(r1 == 26, "CALLED the relocated module", r1, 26);
                    check(g_ext_calls == 1, "the module's PLT32 call reached the host");
                    check(strcmp(g_last_log, "literal") == 0,
                          "the R_X86_64_32 string literal address is correct");

                    /* Again: g_counter is now 6, so 13 + 6 + 10 = 29. A
                     * different answer from identical arguments is what proves
                     * .bss is live writable memory in the loaded block. */
                    int r2 = mt_entry(3, 4);
                    checkf(r2 == 29, "second call sees the module's own .bss", r2, 29);

                    /* Different arguments so a DIFFERENT table slot is read:
                     * mt_ext(3*1)=4, g_counter now 9, g_table[1&3]=20 -> 33.
                     * Element 1 rather than element 0, which is what separates
                     * "the array was copied" from "its first word was" -- a
                     * loader that placed .rodata.cst16 one slot early passes
                     * the b=4 case (index 0) and fails this one. */
                    int r3 = mt_entry(3, 1);
                    checkf(r3 == 33, "indexed table read (R_X86_64_32S)", r3, 33);

                    const char *m = mt_msg();
                    check(m && strcmp(m, "hello-from-module") == 0,
                          "R_X86_64_64: .data pointer into .rodata resolves");
                }
            }
            munmap(blk, maplen);
        }
    }

    /* ============================================ 12: THE RANGE CHECK IS LIVE
     * The same object at 16 GiB. Its absolute relocations cannot fit 32 bits
     * there, so the loader must REFUSE. This is the assertion that
     * -DMODRELOC_NO_RANGE_CHECK inverts, and the reason the check exists is
     * that `-m 512M` in the Makefile is one character away from `-m 8G`. */
    if (need > 0) {
        size_t maplen = ((size_t)need + 4095) & ~(size_t)4095;
        void *hi = map_at(HIGH_ADDR, maplen);
        if (!hi) {
            printf("SKIP: could not map at %#lx\n", HIGH_ADDR);
        } else {
            struct mod_layout lay; const char *undef = NULL;
            int e = mod_elf_load(mimg, mlen, hi, (uint32_t)need,
                                 resolve_host, NULL, &lay, &undef);
            checkf(e == MOD_E_RANGE,
                   "a module above 4 GiB is REFUSED, not silently truncated",
                   e, MOD_E_RANGE);
            munmap(hi, maplen);
        }
    }

    /* ==================================== 13: an undefined symbol is NAMED */
    if (need > 0) {
        size_t maplen = ((size_t)need + 4095) & ~(size_t)4095;
        void *blk = map_at(LOW_ADDR, maplen);
        if (blk) {
            struct mod_layout lay; const char *undef = NULL;
            int e = mod_elf_load(mimg, mlen, blk, (uint32_t)need,
                                 resolve_none, NULL, &lay, &undef);
            checkf(e == MOD_E_UNDEF, "an unresolvable symbol fails the load",
                   e, MOD_E_UNDEF);
            check(undef && (!strcmp(undef, "mt_ext") || !strcmp(undef, "mt_log")),
                  "...and the loader reports WHICH symbol");
            munmap(blk, maplen);
        }
    }

    /* ============================ 14: THE REAL DRIVER, relocated and inspected
     * c/drivers/core/qemu_edu.c, the object the boot gate actually loads. Not
     * called (its probe does MMIO), but its `struct driver` is read back
     * through the same pointer the device model would follow. Every field
     * checked here arrived via a relocation:
     *   ->name      R_X86_64_64 into .rodata.str1.1
     *   ->match     R_X86_64_64 into .rodata
     *   ->probe     R_X86_64_64 into .text
     * and the pointer to the struct itself is the R_X86_64_64 in the object's
     * own logit_drivers section. If any of those is wrong, dev_probe_all()
     * would follow it in ring 0. */
    {
        uint32_t elen = 0;
        uint8_t *eimg = slurp(edu_path, &elen);
        if (eimg) {
            long en = mod_elf_size(eimg, elen);
            check(en > 0, "mod_elf_size accepts the real edu driver object");
            size_t maplen = ((size_t)en + 4095) & ~(size_t)4095;
            void *blk = (en > 0) ? map_at(LOW_ADDR, maplen) : NULL;
            if (blk) {
                memset(blk, 0x5A, maplen);
                struct mod_layout lay; const char *undef = NULL;
                int e = mod_elf_load(eimg, elen, blk, (uint32_t)en,
                                     resolve_edu, NULL, &lay, &undef);
                checkf(e == 0, "the real edu driver relocates cleanly", e, 0);
                if (e == 0) {
                    check(lay.drv_start && lay.drv_stop,
                          "edu: logit_drivers section present");
                    check((uint8_t *)lay.drv_stop - (uint8_t *)lay.drv_start
                          == (long)sizeof(void *),
                          "edu: exactly one DRIVER_DECLARE entry");
                    struct driver *drv = *(struct driver **)lay.drv_start;
                    check((uint8_t *)drv >= (uint8_t *)blk &&
                          (uint8_t *)drv < (uint8_t *)blk + en,
                          "edu: the driver struct is inside the module block");
                    if (drv) {
                        check(drv->name && !strcmp(drv->name, "edu"),
                              "edu: ->name relocated to \"edu\"");
                        checkf(drv->bus_type == DEV_BUS_PCI,
                               "edu: ->bus_type is PCI", drv->bus_type, DEV_BUS_PCI);
                        check(drv->probe != NULL &&
                              (uint8_t *)(uintptr_t)drv->probe >= (uint8_t *)blk &&
                              (uint8_t *)(uintptr_t)drv->probe < (uint8_t *)blk + en,
                              "edu: ->probe points into the module's text");
                        check(drv->next == NULL,
                              "edu: ->next is NULL (driver_register would accept it)");
                        const struct dev_match *m = drv->match;
                        check(m != NULL, "edu: ->match relocated");
                        if (m) {
                            checkf(m[0].vendor == 0x1234, "edu: match vendor",
                                   m[0].vendor, 0x1234);
                            checkf(m[0].device == 0x11E8, "edu: match device",
                                   m[0].device, 0x11E8);
                            check(m[1].vendor == 0 && m[1].device == 0,
                                  "edu: match table is terminated");
                        }
                    }
                }
                munmap(blk, maplen);
            }
            free(eimg);
        }
    }

    /* ================================= 15..20: malformed images are REFUSED
     * The security model says root put the file there, which bounds who can
     * attack the parser and does nothing at all about a half-written file. */
    {
        uint8_t junk[64];
        memset(junk, 0, sizeof junk);
        check(mod_elf_size(junk, sizeof junk) == MOD_E_FORMAT, "not-an-ELF refused");
        check(mod_elf_size(mimg, 16) == MOD_E_FORMAT, "truncated header refused");
        check(mod_elf_size(NULL, 100) == MOD_E_FORMAT, "NULL image refused");

        /* Truncated in the MIDDLE: header intact, section table past the end.
         * This is the shape a half-written file actually has, and the one an
         * `off + size > len` check gets wrong once the addition overflows. */
        uint8_t *cut = malloc(mlen); memcpy(cut, mimg, mlen);
        check(mod_elf_size(cut, mlen / 2) == MOD_E_FORMAT,
              "an image cut in half is refused (section table past the end)");

        /* e_type flipped to ET_EXEC (2): a real ELF, wrong kind. Must be
         * refused by TYPE so the message says which loader it wanted. */
        memcpy(cut, mimg, mlen);
        cut[16] = 2; cut[17] = 0;
        check(mod_elf_size(cut, mlen) == MOD_E_FORMAT, "ET_EXEC refused");

        /* e_machine to EM_386 (3). */
        memcpy(cut, mimg, mlen);
        cut[18] = 3; cut[19] = 0;
        check(mod_elf_size(cut, mlen) == MOD_E_FORMAT, "wrong e_machine refused");

        /* 32-bit ELFCLASS. */
        memcpy(cut, mimg, mlen);
        cut[4] = 1;
        check(mod_elf_size(cut, mlen) == MOD_E_FORMAT, "ELFCLASS32 refused");
        free(cut);
    }

    free(mimg);
    printf("modreloc: %d passed, %d FAILED\n", pass, fail);
    return fail ? 1 : 0;
}
