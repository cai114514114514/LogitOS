/* Core dumps: an ELF64 ET_CORE file written by the kernel at the moment a
 * ring-3 program dies. See coredump.h for the format decision and the size
 * argument; this file is the two halves that follow from it.
 *
 * THE SPLIT, and it is the only structural decision in here. coredump_build()
 * is a pure function over callbacks -- registers in, bytes out, no kernel type
 * in its signature but `struct registers`. Everything that knows about VMAs,
 * page tables, the VFS and kprintf is below the LOGIT_COREDUMP_HOST guard.
 *
 * The alternative was one function reaching straight into vma_* and vmm_pte,
 * which is shorter and cannot be tested: a builder reachable only from a page
 * fault can be exercised only by crashing a real program on the real machine,
 * so every check of the FILE it produces would be a boot away, and the file is
 * the whole deliverable. With the split, tests/unit/coredump_test.c drives the
 * REAL builder with a modelled address space, writes the result to disk, and
 * hands it to gdb -- which is the check that the format claim rests on.
 */

#include <stdint.h>
#include "interrupts.h"     /* struct registers */
#include "coredump.h"

/* ------------------------------------------------------------------ local */
/* No memcpy/memset from the freestanding kernel string.c here on purpose: the
 * builder is compiled into a host test binary as well, where those names are
 * glibc's. Byte loops at these sizes (a 336-byte note, a 4 KiB page) are not
 * where a core dump spends its time -- the disk write is. */
static void cp(void *d, const void *s, uint64_t n)
{
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    for (uint64_t i = 0; i < n; i++) a[i] = b[i];
}
static void zero(void *d, uint64_t n)
{
    unsigned char *a = (unsigned char *)d;
    for (uint64_t i = 0; i < n; i++) a[i] = 0;
}

#define PAGE 4096u

/* ELF64 header and program header, laid out here rather than reused from
 * elf.h: elf.h's struct elf64_phdr is the loader's and is `packed`, and the
 * ELF header it parses is not exposed as a type at all. Repeating 16 fields is
 * cheaper than widening the loader's header for a writer, and the test diffs
 * what comes out against readelf, so a mistake here does not survive. */
struct ehdr64 {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct phdr64 {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

#define PT_LOAD_ 1
#define PT_NOTE_ 4
#define PF_X_ 1
#define PF_W_ 2
#define PF_R_ 4

/* Linux's siginfo_t as a core dump carries it: si_signo/si_errno/si_code then
 * the union. For SIGSEGV/SIGBUS the union's first member is si_addr at offset
 * 16, which is the field gdb prints. 128 bytes total. Only the four fields
 * that mean anything here are set; the rest stays zero, which is the truth --
 * this kernel has no si_pid or si_uid to put there. */
struct core_siginfo_t128 {
    int32_t  si_signo, si_errno, si_code;
    int32_t  pad0;
    uint64_t si_addr;
    uint8_t  rest[104];
};

/* --------------------------------------------------------------- emitting */
struct emit {
    unsigned char *buf;
    int cap, at;
    int full;          /* set once something did not fit; never unset */
};

static void ebytes(struct emit *e, const void *p, int n)
{
    if (n < 0) { e->full = 1; return; }
    if (e->at + n > e->cap) { e->full = 1; return; }
    cp(e->buf + e->at, p, (uint64_t)n);
    e->at += n;
}
static void epad(struct emit *e, int n)
{
    if (n <= 0) return;
    if (e->at + n > e->cap) { e->full = 1; return; }
    zero(e->buf + e->at, (uint64_t)n);
    e->at += n;
}

/* One ELF note. `name` is NUL-terminated; namesz counts the NUL, and both name
 * and desc are padded to 4 -- which is what readelf and gdb both assume, and
 * getting it wrong makes every note AFTER the mistake unreadable rather than
 * the note itself, so the failure would point at the wrong place. */
static void enote(struct emit *e, const char *name, uint32_t type,
                  const void *desc, int descsz)
{
    int nl = 0; while (name[nl]) nl++;
    nl++;                                    /* the NUL is part of namesz */
    struct core_nhdr h;
    h.n_namesz = (uint32_t)nl;
    h.n_descsz = (uint32_t)descsz;
    h.n_type   = type;
    ebytes(e, &h, (int)sizeof h);
    ebytes(e, name, nl);
    epad(e, ((nl + 3) & ~3) - nl);
    ebytes(e, desc, descsz);
    epad(e, ((descsz + 3) & ~3) - descsz);
}

/* --------------------------------------------- the register file, reordered
 * struct registers is this kernel's trap-frame order; pr_reg is
 * <sys/user.h>'s struct user_regs_struct order. They are NOT the same and the
 * difference is invisible in a hex dump, which is why the mapping is one named
 * assignment per register rather than a memcpy of a struct that "looks right".
 *
 * orig_rax is -1: it means "this entry was not a syscall", and a fault is not
 * one. Putting 0 there would name syscall 0, which on this machine is nothing
 * but on Linux is read() -- a reader of the dump would be told a lie it has no
 * way to detect.
 *
 * fs_base/gs_base/ds/es/fs/gs are 0. This kernel keeps the user TLS base in
 * MSR_FS_BASE (SYS_SET_TLS) and does not save it in the trap frame, so the
 * value is not in `r` and cannot be recovered here; 0 is the honest "not
 * captured" and gdb reports it as such. Reading the MSR at dump time would
 * report the FAULTING THREAD'S base only by luck -- the dump runs on that
 * thread, but a caller who moved this call would silently get the dumper's. */
static void fill_gregs(uint64_t *g, const struct registers *r)
{
    g[CORE_R15] = r->r15; g[CORE_R14] = r->r14; g[CORE_R13] = r->r13;
    g[CORE_R12] = r->r12; g[CORE_RBP] = r->rbp; g[CORE_RBX] = r->rbx;
    g[CORE_R11] = r->r11; g[CORE_R10] = r->r10; g[CORE_R9]  = r->r9;
    g[CORE_R8]  = r->r8;  g[CORE_RAX] = r->rax; g[CORE_RCX] = r->rcx;
    g[CORE_RDX] = r->rdx; g[CORE_RSI] = r->rsi; g[CORE_RDI] = r->rdi;
    g[CORE_ORIG_RAX] = (uint64_t)-1;
    g[CORE_RIP] = r->rip; g[CORE_CS] = r->cs; g[CORE_EFLAGS] = r->rflags;
    g[CORE_RSP] = r->rsp; g[CORE_SS] = r->ss;
    g[CORE_FS_BASE] = 0; g[CORE_GS_BASE] = 0;
    g[CORE_DS] = 0; g[CORE_ES] = 0; g[CORE_FS] = 0; g[CORE_GS] = 0;
}

#ifdef COREDUMP_SIGCTX
/* =========================== THE NEGATIVE CONTROL ==========================
 * `make test-coredump-negctl` compiles this file with -DCOREDUMP_SIGCTX, which
 * writes the dump from the DUMPER'S context instead of the faulting one -- the
 * mistake a real implementation makes when it moves the dump out of the trap
 * handler and into a signal-delivery path or into proc_exit(), where the trap
 * frame is no longer in scope and "the registers" are whatever is live.
 *
 * IT MUST LOOK RIGHT, or it proves nothing. So it is not zeroes: the
 * callee-saved registers are read from the CPU, rip is a real code address and
 * rsp a real stack address. Every structural check in the gate still passes --
 * the ELF is well formed, readelf lists the same segments, gdb opens it, the
 * region contents are byte-identical, and the fault address and error code are
 * still right, because a signal handler DOES get those correctly (they arrive
 * in siginfo). Only the register file is wrong, which is exactly the set of
 * checks that must redden.
 *
 * Written with a memory clobber and read into locals first so the compiler
 * cannot hoist the reads past the call this function was reached through. */
static void sigctx_frame(struct registers *out, const struct registers *r)
{
    uint64_t r12, r13, r14, r15, rbx, rbp;
    __asm__ volatile ("movq %%r12, %0\n\tmovq %%r13, %1\n\tmovq %%r14, %2\n\t"
                      "movq %%r15, %3\n\tmovq %%rbx, %4\n\tmovq %%rbp, %5"
                      : "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15),
                        "=r"(rbx), "=r"(rbp) :: "memory");
    zero(out, sizeof *out);
    out->r12 = r12; out->r13 = r13; out->r14 = r14; out->r15 = r15;
    out->rbx = rbx; out->rbp = rbp;
    out->rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    out->rsp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    /* The segment selectors and rflags ARE known correctly in both contexts,
     * so they are copied from the real frame: a control that also corrupted
     * cs/ss would be caught by a check about ring 3 rather than by a check
     * about the register file, and would credit the gate with a property it
     * does not have. */
    out->cs = r->cs; out->ss = r->ss; out->rflags = r->rflags;
}
#endif

/* -------------------------------------------------------------- the build */
int coredump_build(void *bufv, int cap, const struct core_meta *m,
                   const struct registers *r, const void *fx,
                   const struct core_src *src, struct core_logit_note *out)
{
    if (!bufv || !m || !r || !src || cap <= 0) return CORE_E_ARG;

    struct emit e;
    e.buf = (unsigned char *)bufv; e.cap = cap; e.at = 0; e.full = 0;

    const struct registers *use = r;
#ifdef COREDUMP_SIGCTX
    struct registers bogus;
    sigctx_frame(&bogus, r);
    use = &bogus;
#endif

    /* --- 1. enumerate the regions ---------------------------------------
     * Done first because the program-header count depends on it and the ELF
     * header has to name that count before any of it is written. */
    struct core_region rg[CORE_RGN_MAX];
    int nexist = src->regions(src->ctx, rg, CORE_RGN_MAX);
    int nrg = nexist > CORE_RGN_MAX ? CORE_RGN_MAX : nexist;
    if (nrg < 0) nrg = 0;

    struct core_logit_note ln;
    zero(&ln, sizeof ln);
    ln.magic = CORE_LOGIT_MAGIC; ln.version = CORE_LOGIT_VER;
    ln.signo = (uint32_t)m->signo;
    ln.trapno = m->trapno; ln.err = m->err; ln.cr2 = m->cr2; ln.cr3 = m->cr3;
    ln.want_regions = (uint32_t)nexist;
    ln.nregion = (uint32_t)nrg;
    if (nexist > CORE_RGN_MAX) ln.flags |= CORE_F_RGNFULL;
#ifdef COREDUMP_SIGCTX
    ln.flags |= CORE_F_SIGCTX;
#endif

    /* --- 2. find the runs of resident pages ------------------------------
     * One PT_LOAD per contiguous run. File- and segment-backed areas get a
     * header with p_filesz 0 and contribute no bytes; see coredump.h for why
     * their contents are deliberately not copied. */
    struct phdr64 ph[CORE_PHDR_MAX];
    int nph = 1;                     /* [0] is PT_NOTE, filled in below */
    uint64_t want = 0;

    for (int i = 0; i < nrg; i++) {
        ln.region[i].start = rg[i].start;
        ln.region[i].end   = rg[i].end;
        ln.region[i].prot  = rg[i].prot;
        ln.region[i].kind  = rg[i].kind;
        ln.region[i].dumped = 0;

        uint32_t pf = 0;
        if (rg[i].prot & 1) pf |= PF_R_;
        if (rg[i].prot & 2) pf |= PF_W_;
        if (rg[i].prot & 4) pf |= PF_X_;

        if (rg[i].kind != CORE_RGN_ANON) {
            /* Header only. p_memsz says how big it was, p_filesz 0 says none
             * of it is here -- the format's own vocabulary, no annotation
             * needed for gdb and the LOGIT kind field for our reader. */
            if (nph < CORE_PHDR_MAX) {
                ph[nph].p_type = PT_LOAD_; ph[nph].p_flags = pf;
                ph[nph].p_offset = 0; ph[nph].p_vaddr = rg[i].start;
                ph[nph].p_paddr = 0; ph[nph].p_filesz = 0;
                ph[nph].p_memsz = rg[i].end - rg[i].start;
                ph[nph].p_align = PAGE;
                nph++;
            } else ln.flags |= CORE_F_PHDRFULL;
            continue;
        }

        uint64_t va = rg[i].start;
        while (va < rg[i].end) {
            if (!src->mapped(src->ctx, va)) { va += PAGE; continue; }
            uint64_t run = va;
            while (run < rg[i].end && src->mapped(src->ctx, run)) run += PAGE;
            want += run - va;
            if (nph < CORE_PHDR_MAX) {
                ph[nph].p_type = PT_LOAD_; ph[nph].p_flags = pf;
                ph[nph].p_offset = 0;      /* set once the note size is known */
                ph[nph].p_vaddr = va; ph[nph].p_paddr = 0;
                ph[nph].p_filesz = run - va; ph[nph].p_memsz = run - va;
                ph[nph].p_align = PAGE;
                nph++;
            } else {
                ln.flags |= CORE_F_PHDRFULL;
            }
            va = run;
        }
    }
    ln.want_bytes = want;

    /* --- 3. the notes ----------------------------------------------------
     * Written into a scratch area first, because the ELF header must carry
     * e_phoff and PT_NOTE must carry p_offset/p_filesz, and both need the
     * note block's finished size. A second pass over the notes would be the
     * alternative and would leave two copies of the layout to disagree. */
    int hdrsz = (int)sizeof(struct ehdr64) + nph * (int)sizeof(struct phdr64);
    if (hdrsz >= cap) return CORE_E_SMALL;

    e.at = hdrsz;
    int noteoff = e.at;

    {
        struct core_prstatus ps; zero(&ps, sizeof ps);
        ps.pr_info.si_signo = m->signo;
        ps.pr_cursig = (int16_t)m->signo;
        ps.pr_pid = m->pid; ps.pr_ppid = m->ppid;
        ps.pr_pgrp = m->pid; ps.pr_sid = m->pid;
        fill_gregs(ps.pr_reg, use);
        ps.pr_fpvalid = fx ? 1 : 0;
        enote(&e, "CORE", CORE_NT_PRSTATUS, &ps, (int)sizeof ps);
    }
    {
        struct core_prpsinfo pi; zero(&pi, sizeof pi);
        pi.pr_state = 0; pi.pr_sname = 'R'; pi.pr_zomb = 0; pi.pr_nice = 0;
        pi.pr_uid = m->uid; pi.pr_gid = m->gid;
        pi.pr_pid = m->pid; pi.pr_ppid = m->ppid;
        pi.pr_pgrp = m->pid; pi.pr_sid = m->pid;
        if (m->name) {
            int k = 0;
            while (m->name[k] && k < 15) { pi.pr_fname[k] = m->name[k]; k++; }
            for (int j = 0; j < k; j++) pi.pr_psargs[j] = m->name[j];
        }
        enote(&e, "CORE", CORE_NT_PRPSINFO, &pi, (int)sizeof pi);
    }
    {
        struct core_siginfo_t128 si; zero(&si, sizeof si);
        si.si_signo = m->signo;
        /* si_code: SEGV_MAPERR (1) when the page was not present, SEGV_ACCERR
         * (2) when it was and the access was refused. Bit 0 of the page-fault
         * error code is exactly that distinction, so this is read off the
         * hardware rather than guessed. Non-page-faults get SI_KERNEL (0x80),
         * which is what Linux reports for a trap with no better answer. */
        si.si_code = (m->trapno == 14) ? ((m->err & 1) ? 2 : 1) : 0x80;
        si.si_addr = m->cr2;
        enote(&e, "CORE", CORE_NT_SIGINFO, &si, (int)sizeof si);
    }
    if (fx) enote(&e, "CORE", CORE_NT_FPREGSET, fx, 512);

    /* The LOGIT note is LAST among the notes and its want/got byte counts are
     * patched after the segments are written -- so it is emitted here to
     * reserve its space and its offset is remembered. Emitting it first and
     * patching would work equally, and last is chosen so a reader walking the
     * notes in order meets the standard ones a stock tool understands before
     * one it does not. */
    int lnoff;
    {
        /* the descriptor's offset inside the note = header + padded name */
        lnoff = e.at + (int)sizeof(struct core_nhdr) + 8;   /* "LOGIT\0" -> 6 -> pad 8 */
        enote(&e, "LOGIT", CORE_NT_LOGIT, &ln, (int)sizeof ln);
    }
    if (e.full) return CORE_E_SMALL;
    int notesz = e.at - noteoff;

    /* --- 4. the segment bytes -------------------------------------------
     * Page aligned in the file, because gdb and every other reader are
     * entitled to assume p_offset % p_align == p_vaddr % p_align. */
    int at = e.at;
    at = (at + (int)PAGE - 1) & ~((int)PAGE - 1);
    if (at > cap) { at = cap; }

    uint64_t got = 0;
    int got_regions = 0;
    int truncated = 0;
    int rgi = 0;
    /* THE RUN HOLDING rsp GOES FIRST, and this is not an optimisation.
     *
     * Segments are otherwise written in ascending address order, which on this
     * machine puts the stack (exec.c places it just under the program's link
     * base) before the mmap arena at MM_MMAP_BASE -- so the stack survives the
     * cap by luck of the memory map rather than by rule. Measured on the real
     * machine, 2026-08-20: /bin/as faulting with a 24 MiB arena produced
     * "258048 of 528384 bytes, TRUNCATED", i.e. the cap really is reached by an
     * ordinary program, and the only reason the 8 KiB of stack was in it is
     * that 0x53f00000 sorts below 0x60000000.
     *
     * A dump with the register file and no stack has no backtrace in it, which
     * is most of the reason to write one; a heap that ran out of room is a
     * lesser loss and is reported. So the pass over the segments runs twice and
     * takes the one containing rsp first. Two passes rather than sorting the
     * array: the phdr order is what a reader sees, and reordering it to put the
     * stack first would make `readelf -l` list segments out of address order
     * for a reason nothing in the file explains. Only p_offset moves. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 1; i < nph; i++) {
        if (ph[i].p_filesz == 0 || ph[i].p_offset != 0) continue;
        int is_sp = (use->rsp >= ph[i].p_vaddr &&
                     use->rsp <  ph[i].p_vaddr + ph[i].p_filesz);
        if (pass == 0 ? !is_sp : is_sp) continue;
        uint64_t n = ph[i].p_filesz;
        if (at + (int)n > cap) {
            /* THE CAP. What fits is written and p_filesz shrinks to it; the
             * rest of this segment and every later one is described with
             * p_filesz 0 and its p_memsz intact, so nothing claims to hold
             * bytes it does not. */
            uint64_t fit = (uint64_t)(cap - at);
            fit &= ~(uint64_t)(PAGE - 1);
            n = fit;
            truncated = 1;
        }
        if (n == 0) { ph[i].p_offset = 0; ph[i].p_filesz = 0; truncated = 1; continue; }
        ph[i].p_offset = (uint64_t)at;
        for (uint64_t o = 0; o < n; o += PAGE)
            src->read_page(src->ctx, ph[i].p_vaddr + o, e.buf + at + o);
        at += (int)n;
        got += n;
        ph[i].p_filesz = n;
    }
    /* Attribute the written bytes back to their regions, for the note's table.
     * A second walk rather than bookkeeping inside the loop above, because the
     * loop's truncation branch has three exits and one of them shrinks a
     * segment -- three places to remember an increment is three places to
     * forget one. */
    for (rgi = 0; rgi < nrg; rgi++) {
        uint64_t d = 0;
        for (int i = 1; i < nph; i++)
            if (ph[i].p_vaddr >= ln.region[rgi].start &&
                ph[i].p_vaddr <  ln.region[rgi].end)
                d += ph[i].p_filesz;
        ln.region[rgi].dumped = d;
        if (d) got_regions++;
    }
    ln.got_bytes = got;
    ln.got_regions = (uint32_t)got_regions;
    if (truncated || got < want) ln.flags |= CORE_F_TRUNCATED;

    /* Patch the LOGIT note in place, now that got_* are known. */
    cp(e.buf + lnoff, &ln, sizeof ln);

    /* --- 5. the headers -------------------------------------------------- */
    struct ehdr64 eh; zero(&eh, sizeof eh);
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2;      /* ELFCLASS64  */
    eh.e_ident[5] = 1;      /* ELFDATA2LSB */
    eh.e_ident[6] = 1;      /* EV_CURRENT  */
    eh.e_type = 4;          /* ET_CORE     */
    eh.e_machine = 62;      /* EM_X86_64   */
    eh.e_version = 1;
    eh.e_phoff = sizeof(struct ehdr64);
    eh.e_ehsize = (uint16_t)sizeof(struct ehdr64);
    eh.e_phentsize = (uint16_t)sizeof(struct phdr64);
    eh.e_phnum = (uint16_t)nph;
    cp(e.buf, &eh, sizeof eh);

    ph[0].p_type = PT_NOTE_; ph[0].p_flags = 0;
    ph[0].p_offset = (uint64_t)noteoff; ph[0].p_vaddr = 0; ph[0].p_paddr = 0;
    ph[0].p_filesz = (uint64_t)notesz; ph[0].p_memsz = 0; ph[0].p_align = 4;
    cp(e.buf + sizeof(struct ehdr64), ph, (uint64_t)nph * sizeof(struct phdr64));

    if (out) cp(out, &ln, sizeof ln);
    return at;
}

/* ------------------------------------------------------- reading one back
 * Walk a finished dump's PT_NOTE and return NT_PRSTATUS's 27 registers.
 *
 * WHY THE WRITER SHIPS A READER AT ALL. The kernel's one-line [core] summary
 * has to quote registers, and quoting `r` again would print the same path
 * twice -- the trap frame, which the [fault] line one line below already
 * prints. Reading them back OUT OF THE BYTES THAT WERE WRITTEN makes the two
 * lines two paths, so a builder that dropped the register note, mis-sized a
 * note header, or wrote pr_reg in the trap-frame order would show up on the
 * serial log of every crash rather than only in a test.
 *
 * This is NOT the reader /bin/readcore uses. That one has its own walk
 * (c/apps/coreutils/corefmt.h) on purpose: a reader that shares the writer's
 * parser agrees with the writer by construction and can only check arithmetic,
 * never interpretation. Three independent readers see every dump this gate
 * produces -- this one, readcore's, and gdb's.
 *
 * Returns 0 and fills greg27, or -1. */
int coredump_read_gregs(const void *bufv, int n, uint64_t *greg27)
{
    const unsigned char *b = (const unsigned char *)bufv;
    if (!b || !greg27 || n < (int)sizeof(struct ehdr64)) return -1;
    struct ehdr64 eh; cp(&eh, b, sizeof eh);
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_type != 4) return -1;
    if (eh.e_phentsize != sizeof(struct phdr64)) return -1;

    for (int i = 0; i < eh.e_phnum; i++) {
        uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(struct phdr64);
        if (off + sizeof(struct phdr64) > (uint64_t)n) return -1;
        struct phdr64 p; cp(&p, b + off, sizeof p);
        if (p.p_type != PT_NOTE_) continue;
        if (p.p_offset + p.p_filesz > (uint64_t)n) return -1;
        uint64_t at = p.p_offset, end = p.p_offset + p.p_filesz;
        while (at + sizeof(struct core_nhdr) <= end) {
            struct core_nhdr h; cp(&h, b + at, sizeof h);
            uint64_t nm = (h.n_namesz + 3u) & ~3u;
            uint64_t ds = (h.n_descsz + 3u) & ~3u;
            uint64_t desc = at + sizeof h + nm;
            if (desc + ds > end) return -1;
            if (h.n_type == CORE_NT_PRSTATUS && h.n_namesz == 5 &&
                b[at + sizeof h] == 'C' && h.n_descsz >= sizeof(struct core_prstatus)) {
                struct core_prstatus ps;
                cp(&ps, b + desc, sizeof ps);
                for (int k = 0; k < 27; k++) greg27[k] = ps.pr_reg[k];
                return 0;
            }
            at = desc + ds;
        }
    }
    return -1;
}

/* ==========================================================================
 * THE KERNEL HALF. Everything below knows about this machine.
 * ======================================================================== */
#ifndef LOGIT_COREDUMP_HOST

#include "proc.h"
#include "vma.h"
#include "vmm.h"
#include "vfs.h"
#include "kprintf.h"
#include "usercopy.h"

/* THE BUFFER. .bss, not kmalloc -- see coredump.h. Serialised by the BKL: the
 * one caller is the ring-3 fault path in c/kernel/cpu/interrupts.c, which runs
 * with g_bkl held (interrupts.c takes it for every vector < 32 that is not on
 * syscall_is_bkl_free()'s allow-list, and a trap is not a syscall). If a second
 * caller is ever added that does not hold it, this needs a lock of its own and
 * this comment is the thing that says so. */
static unsigned char g_corebuf[CORE_BUF_MAX];
static char g_lastpath[24];
static int  g_count;
static int  g_slot;
static int  g_budget_said;

struct ksrc {
    uint64_t cr3;
    struct vma v[CORE_RGN_MAX];
    int n;
};

static int k_regions(void *ctx, struct core_region *out, int max)
{
    struct ksrc *k = (struct ksrc *)ctx;
    int n = k->n < max ? k->n : max;
    for (int i = 0; i < n; i++) {
        out[i].start = k->v[i].start;
        out[i].end   = k->v[i].end;
        out[i].prot  = k->v[i].prot;
        out[i].kind  = k->v[i].file >= 0 ? CORE_RGN_FILE
                     : k->v[i].shm  >= 0 ? CORE_RGN_SHM : CORE_RGN_ANON;
    }
    return k->n;
}

/* PRESENCE IS ASKED OF THE PAGE TABLE, NOT OF THE VMA, and that distinction is
 * the whole reason this callback exists. A VMA says a page is legitimate; only
 * the PTE says it is there. Reading a reserved-but-untouched anonymous page, or
 * one that reclaim swapped out, from inside the page-fault handler would take a
 * SECOND fault in ring 0 -- which is a panic, from the path whose entire job is
 * to survive a program dying. vmm_pte() allocates nothing and returns NULL for
 * an absent level, so it is safe to ask about an address the process never
 * touched. */
static int k_mapped(void *ctx, uint64_t va)
{
    struct ksrc *k = (struct ksrc *)ctx;
    uint64_t *pte = vmm_pte(k->cr3, va);
    if (!pte) return 0;
    uint64_t e = *pte;
    if (!(e & 1)) return 0;               /* not present (or a swap entry)   */
    if (!(e & 4)) return 0;               /* not user-accessible             */
    return 1;
}

static void k_read_page(void *ctx, uint64_t va, void *dst)
{
    (void)ctx;
    cp(dst, (const void *)(uintptr_t)va, PAGE);
}

const char *coredump_last_path(void) { return g_lastpath; }
int         coredump_count(void)     { return g_count; }

void coredump_take(const struct registers *r, const void *fx,
                   int signo, uint64_t cr2)
{
    struct proc *p = proc_current();
    if (!p) return;                       /* no process: nothing to dump      */

    if (g_count >= CORE_MAX_PER_BOOT) {
        /* Said ONCE. A crash loop printing this every iteration would bury the
         * [fault] lines that say what is actually crashing. */
        if (!g_budget_said) {
            g_budget_said = 1;
            kprintf("[core] budget spent (%d dumps this boot) -- not writing more\n",
                    CORE_MAX_PER_BOOT);
        }
        return;
    }

    static struct ksrc k;                 /* 32 x 40 B; same BKL argument as
                                           * the buffer, and off the 32 KiB
                                           * kernel stack this path is on */
    k.cr3 = p->cr3;
    k.n = vma_snapshot(p->cr3, k.v, CORE_RGN_MAX);
    if (k.n < 0) k.n = 0;

    struct core_src src;
    src.regions = k_regions; src.mapped = k_mapped;
    src.read_page = k_read_page; src.ctx = &k;

    struct core_meta m;
    m.pid = p->pid; m.ppid = p->ppid;
    m.uid = 0; m.gid = 0;
    m.signo = signo;
    m.trapno = r->vector; m.err = r->error_code; m.cr2 = cr2; m.cr3 = p->cr3;
    m.name = p->name;

    struct core_logit_note ln;
    int n = coredump_build(g_corebuf, CORE_BUF_MAX, &m, r, fx, &src, &ln);
    if (n < 0) {
        kprintf("[core] pid %d: build failed (%d) -- no dump\n", p->pid, n);
        return;
    }

    /* /core.1 .. /core.4, round robin. The name is built by hand rather than
     * with a formatter because CORE_SLOTS is 4 and this runs in a fault. */
    g_slot = (g_slot % CORE_SLOTS) + 1;
    g_lastpath[0] = '/'; g_lastpath[1] = 'c'; g_lastpath[2] = 'o';
    g_lastpath[3] = 'r'; g_lastpath[4] = 'e'; g_lastpath[5] = '.';
    g_lastpath[6] = (char)('0' + g_slot); g_lastpath[7] = 0;

    int w = vfs_write(g_lastpath, g_corebuf, n);
    if (w < 0) {
        /* OUT LOUD, and the app still dies normally. A dump that could not be
         * written must never turn a dead app into a dead kernel -- and "the
         * filesystem is full" or "out of inodes" is exactly the state this
         * would be reached in, so a silent return here would remove the one
         * message that explains the missing file. */
        kprintf("[core] pid %d (%s): %s write failed (%d) -- no dump\n",
                p->pid, p->name, g_lastpath, w);
        g_lastpath[0] = 0;
        return;
    }
    g_count++;

    /* ONE line, and every register on it is READ BACK OUT OF THE FILE rather
     * than reprinted from `r`. The [fault] line one line below prints `r`;
     * these two lines are therefore two paths to the same numbers, which is
     * what makes the on-device harness a check instead of a file-exists test.
     * If the register note were dropped, mis-sized, or written in the trap
     * frame's order instead of user_regs_struct's, this line would say so on
     * the serial log of every crash. */
    uint64_t g[27];
    if (coredump_read_gregs(g_corebuf, n, g) != 0) {
        kprintf("[core] pid %d: WROTE %s (%d bytes) BUT CANNOT READ ITS OWN"
                " REGISTER NOTE BACK\n", p->pid, g_lastpath, n);
        return;
    }
    kprintf("[core] pid %d (%s) sig %d -> %s %d bytes: regions %u/%u bytes %u/%u"
            " rip=%p rsp=%p cr2=%p err=%x%s\n",
            p->pid, p->name, signo, g_lastpath, n,
            ln.got_regions, ln.want_regions,
            (unsigned)ln.got_bytes, (unsigned)ln.want_bytes,
            (void *)g[CORE_RIP], (void *)g[CORE_RSP],
            (void *)ln.cr2, (unsigned)ln.err,
            (ln.flags & CORE_F_TRUNCATED) ? " TRUNCATED" : "");
}

#endif /* LOGIT_COREDUMP_HOST */
