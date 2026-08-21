/* The core-dump gate.
 *
 * Three oracles, in increasing independence:
 *
 *   1. GLIBC'S OWN HEADERS. <sys/procfs.h> and <sys/user.h> define
 *      `struct elf_prstatus`, `struct elf_prpsinfo` and `struct
 *      user_regs_struct`. Every offset in coredump.h's copies is diffed
 *      against them, and the 27-entry register-order enum is diffed against
 *      user_regs_struct's field offsets. This is the check that decides
 *      whether "ELF core" is a claim or a costume: a note laid out nearly
 *      right produces a file gdb opens and then lies about.
 *
 *   2. OUR OWN SECOND READER. c/apps/coreutils/corefmt.h is a parser written
 *      from the format description and not from the writer's code, and
 *      /bin/readcore is nothing but a printer around it. It re-reads every
 *      byte the builder produced.
 *
 *   3. gdb AND readelf. Neither came from this tree. gdb is run on the
 *      finished file and required to report the same rip, rsp and r15 this
 *      test put in, the same signal, and the same bytes at the same
 *      addresses.
 *
 * THE MACHINE UNDER THE BUILDER IS MODELLED, and only the machine: the region
 * list, the "is this page resident" predicate and the page reader are this
 * file's, and c/kernel/exec/coredump.c is compiled UNMODIFIED. That is what
 * lets the register file be checked against values chosen in advance -- on the
 * real machine no one chooses what is in r13.
 *
 * NEGATIVE CONTROL: -DCOREDUMP_SIGCTX, which builds the dump from the dumper's
 * own context instead of the trap frame. Every check that must redden under it
 * is prefixed REGFILE, and no other check may -- see tests/coredump.mk for what
 * is asserted and, more usefully, for why the COUNT is a range and the SET is
 * not. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/procfs.h>
#include <sys/user.h>
#include <signal.h>

#include "interrupts.h"
#include "coredump.h"
#include "corefmt.h"

/* ------------------------------------------------------------- accounting */
static int g_pass, g_fail;
static void ck(int ok, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void ck(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (ok) { g_pass++; printf("ok  : "); }
    else    { g_fail++; printf("FAIL: "); }
    vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

/* ------------------------------------------------------ the modelled space
 * Four areas, chosen so each answers a different question:
 *
 *   STACK  two pages, both resident            -> one PT_LOAD, whole
 *   HEAP   three pages, the MIDDLE one absent  -> TWO PT_LOADs from one area,
 *                                                 which is the case a writer
 *                                                 that assumed "an area is a
 *                                                 segment" gets wrong
 *   TEXT   one page, resident, FILE-backed     -> header only, filesz 0
 *   RSVD   one page, no resident page at all   -> no PT_LOAD, dumped 0
 */
#define VA_STACK 0x40000000ULL
#define VA_HEAP  0x41000000ULL
#define VA_TEXT  0x50000000ULL
#define VA_RSVD  0x52000000ULL
#define PG 4096ULL

static struct core_region MRG[4] = {
    { VA_STACK, VA_STACK + 2 * PG, 1 | 2,     CORE_RGN_ANON },
    { VA_HEAP,  VA_HEAP  + 3 * PG, 1 | 2,     CORE_RGN_ANON },
    { VA_TEXT,  VA_TEXT  + 1 * PG, 1 | 4,     CORE_RGN_FILE },
    { VA_RSVD,  VA_RSVD  + 1 * PG, 1 | 2,     CORE_RGN_ANON },
};
static int m_regions(void *c, struct core_region *o, int max)
{
    (void)c;
    int n = 4;
    for (int i = 0; i < n && i < max; i++) o[i] = MRG[i];
    return n;
}
static int m_mapped(void *c, uint64_t va)
{
    (void)c;
    if (va >= VA_STACK && va < VA_STACK + 2 * PG) return 1;
    if (va == VA_HEAP || va == VA_HEAP + 2 * PG)  return 1;   /* middle absent */
    if (va >= VA_TEXT && va < VA_TEXT + PG)       return 1;
    return 0;
}
/* Each page is filled with a byte derived from its address, so a page written
 * to the wrong offset in the file is caught by a value check and not only by a
 * length check -- the same argument c/fs's durability tests make about
 * byte-for-byte comparison. */
static unsigned char page_byte(uint64_t va) { return (unsigned char)((va >> 12) ^ 0x5A); }
static void m_read_page(void *c, uint64_t va, void *dst)
{
    (void)c;
    memset(dst, page_byte(va), PG);
}
static struct core_src MSRC = { m_regions, m_mapped, m_read_page, NULL };

/* A SECOND, UPSIDE-DOWN space for part 6: a big heap LOW and the stack HIGH,
 * which is the arrangement in which ascending-address order spends the whole
 * cap before it reaches the stack. */
#define LO_HEAP  0x41000000ULL
#define HI_STACK 0x7F000000ULL
static struct core_region HRG[2] = {
    { LO_HEAP,  LO_HEAP  + 8 * PG, 1 | 2, CORE_RGN_ANON },
    { HI_STACK, HI_STACK + 2 * PG, 1 | 2, CORE_RGN_ANON },
};
static int h_regions(void *c, struct core_region *o, int max)
{
    (void)c;
    for (int i = 0; i < 2 && i < max; i++) o[i] = HRG[i];
    return 2;
}
static int h_mapped(void *c, uint64_t va)
{
    (void)c;
    return (va >= LO_HEAP  && va < LO_HEAP  + 8 * PG) ||
           (va >= HI_STACK && va < HI_STACK + 2 * PG);
}
static struct core_src HSRC = { h_regions, h_mapped, m_read_page, NULL };

/* ------------------------------------------------------------- sentinels */
#define S_RIP 0x0000000040001234ULL
#define S_RSP 0x0000000040001f80ULL
#define S_R12 0xC0DE000000000012ULL
#define S_R13 0xC0DE000000000013ULL
#define S_R14 0xC0DE000000000014ULL
#define S_R15 0xC0DE000000000015ULL
#define S_RBX 0xC0DE0000000000B8ULL
#define S_RBP 0x0000000040001fc0ULL
#define S_RAX 0xC0DE0000000000AAULL
#define S_RDI 0xC0DE0000000000D1ULL
#define S_CR2 0x00000000DEADBEE0ULL
#define S_ERR 0x0000000000000006ULL     /* write, user, not-present            */
#define S_PID 41
#define S_PPID 7

static void fill_regs(struct registers *r)
{
    memset(r, 0, sizeof *r);
    r->rip = S_RIP; r->rsp = S_RSP;
    r->r12 = S_R12; r->r13 = S_R13; r->r14 = S_R14; r->r15 = S_R15;
    r->rbx = S_RBX; r->rbp = S_RBP; r->rax = S_RAX; r->rdi = S_RDI;
    r->cs = 0x2b; r->ss = 0x23; r->rflags = 0x202;
    r->vector = 14; r->error_code = S_ERR;
}
static struct core_meta MM = {
    S_PID, S_PPID, 0, 0, 11 /* SIGSEGV */, 14, S_ERR, S_CR2, 0x2af000, "crash"
};

/* --------------------------------------------------------------- gdb glue */
static int run(const char *cmd, char *out, int max)
{
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    int n = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (n < max - 1) out[n++] = (char)c;
    }
    out[n] = 0;
    pclose(f);
    return n;
}
/* Find "<name>  0xVALUE" in gdb's `info registers` output. */
static int gdb_reg(const char *text, const char *name, uint64_t *v)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\n%s ", name);
    const char *p = strstr(text, pat);
    if (!p && strncmp(text, name, strlen(name)) == 0) p = text - 1;
    if (!p) return 0;
    p = strstr(p, "0x");
    if (!p) return 0;
    *v = strtoull(p + 2, NULL, 16);
    return 1;
}

int main(void)
{
    static unsigned char buf[CORE_BUF_MAX];
    static unsigned char fx[512];
    struct registers r;
    struct core_logit_note ln;

    for (int i = 0; i < 512; i++) fx[i] = (unsigned char)(i * 7);
    fill_regs(&r);

    /* ================================================================== 1 */
    printf("--- 1. the note layouts, against glibc's own headers ---\n");
    ck(sizeof(struct core_prstatus) == sizeof(struct elf_prstatus),
       "sizeof prstatus %zu == glibc %zu",
       sizeof(struct core_prstatus), sizeof(struct elf_prstatus));
#define OFF(f) ck(offsetof(struct core_prstatus, f) == offsetof(struct elf_prstatus, f), \
                  "prstatus." #f " at %zu == glibc %zu", \
                  offsetof(struct core_prstatus, f), offsetof(struct elf_prstatus, f))
    OFF(pr_info); OFF(pr_cursig); OFF(pr_sigpend); OFF(pr_sighold);
    OFF(pr_pid); OFF(pr_ppid); OFF(pr_pgrp); OFF(pr_sid);
    OFF(pr_utime); OFF(pr_stime); OFF(pr_cutime); OFF(pr_cstime);
    OFF(pr_reg); OFF(pr_fpvalid);
#undef OFF
    ck(sizeof(((struct core_prstatus *)0)->pr_reg) / 8 == ELF_NGREG,
       "pr_reg holds %zu registers == ELF_NGREG %zu",
       sizeof(((struct core_prstatus *)0)->pr_reg) / 8, (size_t)ELF_NGREG);

    ck(sizeof(struct core_prpsinfo) == sizeof(struct elf_prpsinfo),
       "sizeof prpsinfo %zu == glibc %zu",
       sizeof(struct core_prpsinfo), sizeof(struct elf_prpsinfo));
#define OFFP(f) ck(offsetof(struct core_prpsinfo, f) == offsetof(struct elf_prpsinfo, f), \
                   "prpsinfo." #f " at %zu == glibc %zu", \
                   offsetof(struct core_prpsinfo, f), offsetof(struct elf_prpsinfo, f))
    OFFP(pr_state); OFFP(pr_flag); OFFP(pr_uid); OFFP(pr_gid);
    OFFP(pr_pid); OFFP(pr_ppid); OFFP(pr_pgrp); OFFP(pr_sid);
    OFFP(pr_fname); OFFP(pr_psargs);
#undef OFFP

    /* THE REGISTER ORDER. This is the check the whole format rests on: the
     * enum in coredump.h is a claim about where each register sits in
     * elf_gregset_t, and the only authority for that on this planet is
     * user_regs_struct. Diffed one register at a time so a failure names the
     * one that moved. */
#define ORD(f, i) ck(offsetof(struct user_regs_struct, f) / 8 == (size_t)(i), \
                     "pr_reg[%d] is " #f " (glibc index %zu)", (i), \
                     offsetof(struct user_regs_struct, f) / 8)
    ORD(r15, CORE_R15); ORD(r14, CORE_R14); ORD(r13, CORE_R13); ORD(r12, CORE_R12);
    ORD(rbp, CORE_RBP); ORD(rbx, CORE_RBX); ORD(r11, CORE_R11); ORD(r10, CORE_R10);
    ORD(r9, CORE_R9);   ORD(r8, CORE_R8);   ORD(rax, CORE_RAX); ORD(rcx, CORE_RCX);
    ORD(rdx, CORE_RDX); ORD(rsi, CORE_RSI); ORD(rdi, CORE_RDI);
    ORD(orig_rax, CORE_ORIG_RAX);
    ORD(rip, CORE_RIP); ORD(cs, CORE_CS);   ORD(eflags, CORE_EFLAGS);
    ORD(rsp, CORE_RSP); ORD(ss, CORE_SS);
    ORD(fs_base, CORE_FS_BASE); ORD(gs_base, CORE_GS_BASE);
    ORD(ds, CORE_DS);   ORD(es, CORE_ES);   ORD(fs, CORE_FS); ORD(gs, CORE_GS);
#undef ORD
    ck(sizeof(struct user_fpregs_struct) == 512,
       "NT_FPREGSET is the 512-byte FXSAVE area (glibc says %zu)",
       sizeof(struct user_fpregs_struct));
    ck(offsetof(siginfo_t, si_addr) == 16 && sizeof(siginfo_t) == 128,
       "siginfo_t: si_addr at %zu, size %zu",
       offsetof(siginfo_t, si_addr), sizeof(siginfo_t));

    /* ================================================================== 2 */
    printf("--- 2. the build ---\n");
    int n = coredump_build(buf, sizeof buf, &MM, &r, fx, &MSRC, &ln);
    ck(n > 0, "coredump_build returned %d", n);
    if (n <= 0) { printf("coredump: %d/%d\n", g_pass, g_pass + g_fail); return 1; }

    ck((ln.flags & CORE_F_TRUNCATED) == 0, "not truncated at the full cap (flags %#x)", ln.flags);
    ck(ln.want_bytes == 4 * PG, "want_bytes %llu == 4 resident pages (2 stack + 2 of 3 heap)",
       (unsigned long long)ln.want_bytes);
    ck(ln.got_bytes == ln.want_bytes, "got_bytes %llu == want_bytes %llu",
       (unsigned long long)ln.got_bytes, (unsigned long long)ln.want_bytes);
    ck(ln.want_regions == 4 && ln.nregion == 4, "4 regions recorded (want %u, n %u)",
       ln.want_regions, ln.nregion);

    /* ================================================================== 3 */
    printf("--- 3. our second reader (c/apps/coreutils/corefmt.h) ---\n");
    static struct cf_seg segs[64];
    struct cf_dump D;
    int e = cf_parse(&D, buf, n, segs, 64);
    ck(e == CF_OK, "cf_parse -> %d", e);

    /* THE REGISTER FILE. These are the checks the negative control must
     * redden, and they are the reason this file exists. */
    ck(D.greg[CORE_RIP] == S_RIP, "REGFILE rip  %#llx", (unsigned long long)D.greg[CORE_RIP]);
    ck(D.greg[CORE_RSP] == S_RSP, "REGFILE rsp  %#llx", (unsigned long long)D.greg[CORE_RSP]);
    ck(D.greg[CORE_R12] == S_R12, "REGFILE r12  %#llx", (unsigned long long)D.greg[CORE_R12]);
    ck(D.greg[CORE_R13] == S_R13, "REGFILE r13  %#llx", (unsigned long long)D.greg[CORE_R13]);
    ck(D.greg[CORE_R14] == S_R14, "REGFILE r14  %#llx", (unsigned long long)D.greg[CORE_R14]);
    ck(D.greg[CORE_R15] == S_R15, "REGFILE r15  %#llx", (unsigned long long)D.greg[CORE_R15]);
    ck(D.greg[CORE_RBX] == S_RBX, "REGFILE rbx  %#llx", (unsigned long long)D.greg[CORE_RBX]);
    ck(D.greg[CORE_RBP] == S_RBP, "REGFILE rbp  %#llx", (unsigned long long)D.greg[CORE_RBP]);
    ck(D.greg[CORE_RAX] == S_RAX, "REGFILE rax  %#llx", (unsigned long long)D.greg[CORE_RAX]);
    ck(D.greg[CORE_RDI] == S_RDI, "REGFILE rdi  %#llx", (unsigned long long)D.greg[CORE_RDI]);

    /* And these must NOT redden under the control -- they are what stops
     * "corrupt everything" from counting as a control. */
    ck(D.greg[CORE_ORIG_RAX] == (uint64_t)-1, "orig_rax is -1, not syscall 0");
    ck(D.greg[CORE_CS] == 0x2b && D.greg[CORE_SS] == 0x23, "cs/ss are ring 3");
    ck(D.logit.cr2 == S_CR2, "LOGIT cr2 %#llx", (unsigned long long)D.logit.cr2);
    ck(D.logit.err == S_ERR, "LOGIT err %#llx", (unsigned long long)D.logit.err);
    ck(D.logit.trapno == 14, "LOGIT trapno %llu", (unsigned long long)D.logit.trapno);
    ck(D.fault_addr == S_CR2, "NT_SIGINFO si_addr %#llx", (unsigned long long)D.fault_addr);
    ck(D.fault_code == 1, "si_code SEGV_MAPERR (err bit 0 clear) -> %d", D.fault_code);
    ck(D.signo == 11 && D.pid == S_PID && D.ppid == S_PPID,
       "signo/pid/ppid %d/%d/%d", D.signo, D.pid, D.ppid);
    ck(strcmp(D.fname, "crash") == 0, "pr_fname \"%s\"", D.fname);
    ck(D.fpvalid == 1 && D.has_fpregs == 1, "NT_FPREGSET present and pr_fpvalid set");

    /* The kernel's own read-back path, which is what its [core] line quotes. */
    uint64_t g2[27];
    ck(coredump_read_gregs(buf, n, g2) == 0 && g2[CORE_RIP] == S_RIP &&
       g2[CORE_RSP] == S_RSP,
       "REGFILE coredump_read_gregs agrees with corefmt.h");

    /* ------------------------------------------------------- the segments */
    ck(D.nseg == 4, "4 PT_LOADs: stack(1) + heap(2 runs) + text(header) = %d", D.nseg);
    int seen_stack = 0, seen_h0 = 0, seen_h2 = 0, seen_text = 0, seen_rsvd = 0;
    for (int i = 0; i < D.nseg_stored; i++) {
        if (segs[i].vaddr == VA_STACK) { seen_stack = 1;
            ck(segs[i].filesz == 2 * PG && segs[i].memsz == 2 * PG,
               "stack PT_LOAD is whole (%llu bytes)", (unsigned long long)segs[i].filesz); }
        if (segs[i].vaddr == VA_HEAP)          seen_h0 = 1;
        if (segs[i].vaddr == VA_HEAP + 2 * PG) seen_h2 = 1;
        if (segs[i].vaddr == VA_TEXT) { seen_text = 1;
            ck(segs[i].filesz == 0 && segs[i].memsz == PG,
               "file-backed PT_LOAD: filesz 0, memsz %llu",
               (unsigned long long)segs[i].memsz); }
        if (segs[i].vaddr == VA_RSVD) seen_rsvd = 1;
    }
    ck(seen_stack && seen_h0 && seen_h2 && seen_text,
       "every expected segment present");
    ck(!seen_rsvd, "the area with no resident page produced NO PT_LOAD");
    ck(seen_h0 && seen_h2 && !m_mapped(NULL, VA_HEAP + PG),
       "the hole in the heap area split it in two rather than being papered over");

    /* THE BYTES. Compared to the value the model would have produced for that
     * address, so a page written at the wrong offset fails here. */
    int bytes_ok = 1;
    for (int i = 0; i < D.nseg_stored; i++) {
        for (uint64_t o = 0; o < segs[i].filesz; o += PG) {
            unsigned char want = page_byte(segs[i].vaddr + o);
            const unsigned char *p = buf + segs[i].offset + o;
            for (int j = 0; j < 4096; j++)
                if (p[j] != want) { bytes_ok = 0; break; }
        }
    }
    ck(bytes_ok, "every dumped page holds the bytes that address had");

    /* --------------------------------------------------- the region table */
    ck(D.logit.region[0].dumped == 2 * PG && D.logit.region[0].kind == CORE_RGN_ANON,
       "region[0] stack: %llu bytes dumped",
       (unsigned long long)D.logit.region[0].dumped);
    ck(D.logit.region[1].dumped == 2 * PG,
       "region[1] heap: %llu of 3 pages (the middle one is absent)",
       (unsigned long long)D.logit.region[1].dumped);
    ck(D.logit.region[2].kind == CORE_RGN_FILE && D.logit.region[2].dumped == 0,
       "region[2] text: file-backed, 0 bytes -- deliberate, and named as such");
    ck(D.logit.region[3].dumped == 0, "region[3] reserved: nothing resident");

    /* ================================================================== 4 */
    printf("--- 4. gdb and readelf, which did not come from this tree ---\n");
    FILE *f = fopen("build/coredump_test.core", "wb");
    ck(f != NULL, "wrote build/coredump_test.core");
    if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }

    static char out[65536];
    run("readelf -h -l build/coredump_test.core 2>&1", out, sizeof out);
    ck(strstr(out, "CORE (Core file)") != NULL, "readelf: Type is CORE");
    ck(strstr(out, "X86-64") != NULL, "readelf: Machine is x86-64");
    {   int loads = 0; const char *p = out;
        while ((p = strstr(p, "\n  LOAD")) != NULL) { loads++; p += 3; }
        ck(loads == 4, "readelf counts %d LOAD segments", loads);
    }
    run("readelf -n build/coredump_test.core 2>&1", out, sizeof out);
    ck(strstr(out, "NT_PRSTATUS") != NULL, "readelf: NT_PRSTATUS present");
    ck(strstr(out, "NT_PRPSINFO") != NULL, "readelf: NT_PRPSINFO present");
    ck(strstr(out, "NT_SIGINFO") != NULL, "readelf: NT_SIGINFO present");
    ck(strstr(out, "NT_FPREGSET") != NULL, "readelf: NT_FPREGSET present");
    ck(strstr(out, "LOGIT") != NULL, "readelf: the LOGIT note is there");
    /* The private note must NOT be numbered where a stock tool will mistake it
     * for a standard one -- readelf printed it as NT_PRSTATUS when it was type
     * 1, and gdb built a second bogus thread from it. */
    {   const char *p = strstr(out, "LOGIT");
        ck(p && strstr(p, "Unknown note type") != NULL,
           "the LOGIT note is not mistaken for a standard note");
    }

    run("gdb -batch -nx -c build/coredump_test.core "
        "-ex 'info registers rip rsp r15 r12 rbx' 2>&1", out, sizeof out);
    ck(strstr(out, "SIGSEGV") != NULL, "gdb: terminated with SIGSEGV");
    ck(strstr(out, "Core was generated by `crash'") != NULL,
       "gdb: reads the program name out of NT_PRPSINFO");
    uint64_t v = 0;
    ck(gdb_reg(out, "rip", &v) && v == S_RIP, "REGFILE gdb: rip %#llx", (unsigned long long)v);
    ck(gdb_reg(out, "rsp", &v) && v == S_RSP, "REGFILE gdb: rsp %#llx", (unsigned long long)v);
    ck(gdb_reg(out, "r15", &v) && v == S_R15, "REGFILE gdb: r15 %#llx", (unsigned long long)v);
    ck(gdb_reg(out, "r12", &v) && v == S_R12, "REGFILE gdb: r12 %#llx", (unsigned long long)v);
    ck(gdb_reg(out, "rbx", &v) && v == S_RBX, "REGFILE gdb: rbx %#llx", (unsigned long long)v);

    {   char cmd[512];
        snprintf(cmd, sizeof cmd,
                 "gdb -batch -nx -c build/coredump_test.core "
                 "-ex 'x/1xb 0x%llx' -ex 'x/1xb 0x%llx' 2>&1",
                 (unsigned long long)VA_STACK, (unsigned long long)(VA_HEAP + 2 * PG));
        run(cmd, out, sizeof out);
        char w1[16], w2[16];
        snprintf(w1, sizeof w1, "0x%02x", page_byte(VA_STACK));
        snprintf(w2, sizeof w2, "0x%02x", page_byte(VA_HEAP + 2 * PG));
        ck(strstr(out, w1) != NULL, "gdb reads %s at the stack page", w1);
        ck(strstr(out, w2) != NULL, "gdb reads %s at the far heap page", w2);
    }

    /* ================================================================== 5 */
    printf("--- 5. the size cap, which is the whole reason this is hard ---\n");
    /* Big enough for headers and notes plus ONE page of segment, which forces
     * the writer to shrink one segment and drop the rest. */
    int small = 8192 + 4096;
    struct core_logit_note ln2;
    int n2 = coredump_build(buf, small, &MM, &r, fx, &MSRC, &ln2);
    ck(n2 > 0 && n2 <= small, "capped build returned %d of %d", n2, small);
    ck((ln2.flags & CORE_F_TRUNCATED) != 0, "CORE_F_TRUNCATED is set");
    ck(ln2.got_bytes < ln2.want_bytes, "got %llu < want %llu, and both are recorded",
       (unsigned long long)ln2.got_bytes, (unsigned long long)ln2.want_bytes);
    {   struct cf_dump D2;
        int e2 = cf_parse(&D2, buf, n2, segs, 64);
        ck(e2 == CF_OK, "a truncated dump is still a well-formed, readable ELF core");
        ck(D2.greg[CORE_RIP] == S_RIP,
           "REGFILE a truncated dump still carries the register file -- who died is never cut");
        uint64_t sum = 0;
        for (int i = 0; i < D2.nseg_stored; i++) sum += segs[i].filesz;
        ck(sum == D2.logit.got_bytes,
           "the PT_LOADs' filesz sums to got_bytes (%llu) -- nothing claims bytes it lacks",
           (unsigned long long)sum);
        int over = 0;
        for (int i = 0; i < D2.nseg_stored; i++)
            if (segs[i].offset + segs[i].filesz > (uint64_t)n2) over = 1;
        ck(!over, "no PT_LOAD points past the end of the truncated file");
    }
    /* And a cap so small even the notes do not fit must REFUSE, not produce a
     * short file that parses. */
    int n3 = coredump_build(buf, 200, &MM, &r, fx, &MSRC, NULL);
    ck(n3 == CORE_E_SMALL, "a cap below the headers refuses (%d)", n3);

    /* ================================================================== 6 */
    printf("--- 6. under the cap, the STACK is what survives ---\n");
    /* The address map is deliberately upside down: a large heap at a LOW
     * address and the stack HIGH, so ascending-address order would spend the
     * whole cap on the heap and leave no stack at all. On the real machine the
     * two happen to be the other way round, which is exactly why this case has
     * to be constructed -- the property would otherwise be true by luck of the
     * memory map and would break the first time a program was linked
     * differently. Measured on device the same day: /bin/as faulting with a
     * 24 MiB arena reports "258048 of 528384 bytes, TRUNCATED", so the cap is
     * reached by an ordinary program and this is not a hypothetical. */
    {
        struct core_logit_note ln3;
        struct registers r3 = r;
        r3.rsp = HI_STACK + PG;               /* inside the second stack page */
        int n4 = coredump_build(buf, 8192 + 3 * 4096, &MM, &r3, fx, &HSRC, &ln3);
        ck(n4 > 0, "upside-down build returned %d", n4);
        ck((ln3.flags & CORE_F_TRUNCATED) != 0, "it is truncated, as intended");
        struct cf_dump D3;
        ck(cf_parse(&D3, buf, n4, segs, 64) == CF_OK, "and still parses");
        int sp_in = 0, sp_first = 0;
        uint64_t lowest_off = (uint64_t)-1;
        for (int i = 0; i < D3.nseg_stored; i++) {
            if (!segs[i].filesz) continue;
            if (segs[i].offset < lowest_off) {
                lowest_off = segs[i].offset;
                sp_first = (r3.rsp >= segs[i].vaddr &&
                            r3.rsp < segs[i].vaddr + segs[i].filesz);
            }
            if (r3.rsp >= segs[i].vaddr && r3.rsp < segs[i].vaddr + segs[i].filesz)
                sp_in = 1;
        }
        ck(sp_in, "REGFILE the stack is in the dump even though the heap sorts first");
        ck(sp_first, "REGFILE and it is the FIRST thing written, not a leftover");
        ck(ln3.got_bytes < ln3.want_bytes,
           "the heap is what was lost: %llu of %llu bytes",
           (unsigned long long)ln3.got_bytes, (unsigned long long)ln3.want_bytes);
        /* Segment ORDER in the phdr table must still be by address -- only the
         * file offsets moved. A reader that sorted by p_offset and expected
         * ascending vaddr would otherwise be quietly wrong. */
        int ascending = 1;
        for (int i = 1; i < D3.nseg_stored; i++)
            if (segs[i].vaddr < segs[i - 1].vaddr) ascending = 0;
        ck(ascending, "the phdr table is still in ascending address order");
    }

    printf("coredump: %d/%d checks\n", g_pass, g_pass + g_fail);
    if (g_fail) printf("coredump: %d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
