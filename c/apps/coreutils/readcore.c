#include "clib.h"
#include "corefmt.h"

/* readcore [PATH]        print an ELF64 ET_CORE dump written by this kernel
 *
 * Default PATH is /core.1. c/kernel/exec/coredump.h explains why the slots are
 * four fixed names and not one per pid; the kernel's [core] line on the serial
 * log names the one it just wrote.
 *
 * WHY A PROGRAM AND NOT A HOST SCRIPT. A dump nothing on the machine can read
 * is a file, not a dump: the machine that produced it is the machine somebody
 * is sitting in front of, and "copy the disk image out and run gdb" is a
 * procedure, not a reader. gdb is still the better tool when it is available
 * -- this prints what gdb prints plus the two things gdb has no field for,
 * which are the CPU error code and WHY a region has no bytes in the file.
 *
 * THE `kind` COLUMN IS THE POINT OF THE REGION TABLE. ELF says p_memsz >
 * p_filesz means zero fill, so gdb reads 00 out of a region that was
 * deliberately not dumped and out of a region that really was zeroes, and
 * prints the same thing for both. This is the only reader that can tell them
 * apart, because the LOGIT note is where the difference is written down.
 *
 * Output is one `KEY value` line per fact, not a table: the on-device harness
 * greps it, and a formatted table is a thing whose columns move. */

#define MAXSEG 64
#define MAXBUF (256 * 1024)

static unsigned char buf[MAXBUF];
static struct cf_seg segs[MAXSEG];
static struct cf_dump D;

static void hex(unsigned long v)
{
    char t[20]; int k = 0;
    const char *d = "0123456789abcdef";
    if (!v) t[k++] = '0';
    while (v) { t[k++] = d[v & 15]; v >>= 4; }
    outs("0x");
    while (k) outc(t[--k]);
}

static void kvx(const char *k, unsigned long v) { outs(k); outc(' '); hex(v); outc('\n'); }
static void kvd(const char *k, long v)          { outs(k); outc(' '); outn(v); outc('\n'); }

static const char *reason(unsigned kind, unsigned long dumped, unsigned long span)
{
    if (kind == CORE_RGN_FILE) return "not-dumped:file-backed";
    if (kind == CORE_RGN_SHM)  return "not-dumped:shared-segment";
    if (dumped == 0)           return "not-dumped:no-resident-page";
    if (dumped < span)         return "partial:pages-absent-or-cap";
    return "dumped:whole";
}

static const char *cf_err(int e)
{
    switch (e) {
    case CF_E_SHORT:   return "file too short";
    case CF_E_MAGIC:   return "not an ELF64 little-endian file";
    case CF_E_TYPE:    return "ELF, but not ET_CORE";
    case CF_E_PHDR:    return "the program-header table is outside the file";
    case CF_E_NONOTE:  return "no PT_NOTE segment";
    case CF_E_NOREGS:  return "no NT_PRSTATUS: there is no register file in it";
    case CF_E_NOLOGIT: return "no LOGIT note: not written by this kernel";
    default:           return "?";
    }
}

/* The 27 registers, in the order the file stores them, named. Printed all of
 * them rather than a chosen few: a crash whose cause is in r10 is exactly the
 * crash where somebody would otherwise have to edit this program. */
static const char *const RN[27] = {
    "r15","r14","r13","r12","rbp","rbx","r11","r10","r9","r8","rax","rcx",
    "rdx","rsi","rdi","orig_rax","rip","cs","eflags","rsp","ss","fs_base",
    "gs_base","ds","es","fs","gs"
};

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/core.1";

    int fd = sys_open(path, 0);
    if (fd < 0) { errs("readcore: cannot open "); errs(path); errs("\n"); return 1; }
    int n = 0;
    for (;;) {
        int r = sys_read(fd, buf + n, MAXBUF - n);
        if (r <= 0) break;
        n += r;
        if (n >= MAXBUF) break;
    }
    sys_close(fd);
    if (n <= 0) { errs("readcore: "); errs(path); errs(" is empty\n"); return 1; }

    int e = cf_parse(&D, buf, n, segs, MAXSEG);
    if (e != CF_OK) {
        errs("readcore: "); errs(path); errs(": "); errs(cf_err(e)); errs("\n");
        return 1;
    }

    /* TRUNCATED FIRST, on its own line, before anything a reader would start
     * believing. A dump that holds less than the program had is still worth
     * reading; one that is read as complete is not. */
    if (D.logit.flags & CORE_F_TRUNCATED) outs("TRUNCATED\n");
    if (D.logit.flags & CORE_F_PHDRFULL)  outs("TRUNCATED-SEGMENTS\n");
    if (D.logit.flags & CORE_F_RGNFULL)   outs("TRUNCATED-REGIONS\n");
    if (D.logit.flags & CORE_F_SIGCTX)
        outs("NEGATIVE-CONTROL: this dump was built from the wrong context\n");

    outs("CORE "); outs(path); outc(' '); outn(n); outs(" bytes\n");
    outs("NAME "); outs(D.fname); outc('\n');
    kvd("PID", D.pid);
    kvd("PPID", D.ppid);
    kvd("SIGNO", D.signo);
    kvx("TRAPNO", (unsigned long)D.logit.trapno);
    kvx("ERR", (unsigned long)D.logit.err);
    kvx("CR2", (unsigned long)D.logit.cr2);
    kvx("CR3", (unsigned long)D.logit.cr3);
    kvd("SICODE", D.fault_code);
    kvx("SIADDR", (unsigned long)D.fault_addr);
    kvd("FPVALID", D.fpvalid);
    kvd("FPREGS", D.has_fpregs);

    /* CR2 and SIADDR are written from one variable by the writer and read from
     * two different notes here. Printing both is what lets a reader see them
     * disagree, which is the only way this file could be internally
     * inconsistent without the ELF being malformed. */
    if (D.fault_addr != D.logit.cr2) outs("INCONSISTENT cr2 != si_addr\n");

    for (int i = 0; i < 27; i++) {
        outs("REG "); outs(RN[i]); outc(' '); hex((unsigned long)D.greg[i]); outc('\n');
    }

    kvd("SEGMENTS", D.nseg);
    if (D.nseg > D.nseg_stored) {
        outs("SEGMENTS-UNLISTED "); outn(D.nseg - D.nseg_stored); outc('\n');
    }
    for (int i = 0; i < D.nseg_stored; i++) {
        outs("SEG "); hex((unsigned long)segs[i].vaddr);
        outs(" memsz "); hex((unsigned long)segs[i].memsz);
        outs(" filesz "); hex((unsigned long)segs[i].filesz);
        outs(" flags "); hex(segs[i].flags);
        outc('\n');
    }

    kvd("REGIONS", (long)D.logit.nregion);
    outs("BYTES "); outn((long)D.logit.got_bytes);
    outs(" of "); outn((long)D.logit.want_bytes); outc('\n');
    for (unsigned i = 0; i < D.logit.nregion && i < CORE_RGN_MAX; i++) {
        struct core_region_note *R = &D.logit.region[i];
        outs("RGN "); hex((unsigned long)R->start);
        outc('-'); hex((unsigned long)R->end);
        outs(" prot ");
        outc((R->prot & 1) ? 'r' : '-');
        outc((R->prot & 2) ? 'w' : '-');
        outc((R->prot & 4) ? 'x' : '-');
        outs(" dumped "); outn((long)R->dumped);
        outc(' ');
        outs(reason(R->kind, (unsigned long)R->dumped,
                    (unsigned long)(R->end - R->start)));
        outc('\n');
    }
    return 0;
}
