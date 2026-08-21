/* corecheck -- read a core dump the MACHINE wrote and hold it against numbers
 * that came from somewhere else.
 *
 *   corecheck <dump> key=value ...      keys: signo pid cr2 err trapno rip rsp
 *
 * WHAT MAKES IT A CHECK RATHER THAN A FILE-EXISTS TEST. Every expected value
 * handed to it on the command line came from a different channel than the file:
 *
 *   cr2   was written down in fsroot/as/examples/crashme.as before the machine
 *         booted -- an oracle that existed before the fault did;
 *   rip
 *   rsp   were printed by c/kernel/cpu/interrupts.c's [fault] line, which
 *   err   quotes the TRAP FRAME. The dump quotes the FILE. Two paths.
 *
 * It then runs gdb on the same file, so a third reader that came from outside
 * this tree has to agree as well. The parser is c/apps/coreutils/corefmt.h --
 * the same one /bin/readcore uses, so the on-device reader is exercised by the
 * on-device gate even though the reader itself is not on the disk (see the
 * Makefile note in fsroot/as/examples/crashme.as).
 *
 * Prints everything it read before it judges anything, because a failure whose
 * log does not contain the file's actual contents is a failure somebody has to
 * reproduce to understand. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "interrupts.h"
#include "coredump.h"
#include "corefmt.h"

static unsigned char buf[CORE_BUF_MAX * 2];
static struct cf_seg segs[128];
static struct cf_dump D;
static int fails;

static void bad(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fails++; printf("FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}

static const char *cf_err(int e)
{
    switch (e) {
    case CF_E_SHORT:   return "file too short";
    case CF_E_MAGIC:   return "not an ELF64 little-endian file";
    case CF_E_TYPE:    return "ELF, but not ET_CORE";
    case CF_E_PHDR:    return "program-header table outside the file";
    case CF_E_NONOTE:  return "no PT_NOTE segment";
    case CF_E_NOREGS:  return "no NT_PRSTATUS -- no register file in it";
    case CF_E_NOLOGIT: return "no LOGIT note -- not written by this kernel";
    default:           return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: corecheck <dump> [key=value ...]\n"); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "corecheck: cannot open %s\n", argv[1]); return 2; }
    int n = (int)fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n <= 0) { fprintf(stderr, "corecheck: %s is empty\n", argv[1]); return 2; }

    int e = cf_parse(&D, buf, n, segs, 128);
    if (e != CF_OK) {
        fprintf(stderr, "corecheck: %s: %s (%d)\n", argv[1], cf_err(e), e);
        return 1;
    }

    printf("DUMP %s %d bytes\n", argv[1], n);
    printf("NAME %s\n", D.fname);
    printf("PID %d PPID %d SIGNO %d\n", D.pid, D.ppid, D.signo);
    printf("TRAPNO 0x%llx ERR 0x%llx CR2 0x%llx CR3 0x%llx\n",
           (unsigned long long)D.logit.trapno, (unsigned long long)D.logit.err,
           (unsigned long long)D.logit.cr2, (unsigned long long)D.logit.cr3);
    printf("SIADDR 0x%llx SICODE %d\n", (unsigned long long)D.fault_addr, D.fault_code);
    printf("RIP 0x%llx RSP 0x%llx RBP 0x%llx\n",
           (unsigned long long)D.greg[CORE_RIP],
           (unsigned long long)D.greg[CORE_RSP],
           (unsigned long long)D.greg[CORE_RBP]);
    printf("FLAGS 0x%x  BYTES %llu of %llu  REGIONS %u of %u  SEGMENTS %d\n",
           D.logit.flags, (unsigned long long)D.logit.got_bytes,
           (unsigned long long)D.logit.want_bytes,
           D.logit.got_regions, D.logit.want_regions, D.nseg);
    for (unsigned i = 0; i < D.logit.nregion && i < CORE_RGN_MAX; i++)
        printf("RGN 0x%llx-0x%llx prot %u kind %u dumped %llu\n",
               (unsigned long long)D.logit.region[i].start,
               (unsigned long long)D.logit.region[i].end,
               D.logit.region[i].prot, D.logit.region[i].kind,
               (unsigned long long)D.logit.region[i].dumped);

    /* --- internal consistency, which needs no argument from outside ------ */
    if (D.fault_addr != D.logit.cr2)
        bad("NT_SIGINFO si_addr 0x%llx != LOGIT cr2 0x%llx -- one variable, two"
            " notes, and they disagree",
            (unsigned long long)D.fault_addr, (unsigned long long)D.logit.cr2);
    if (D.greg[CORE_ORIG_RAX] != (uint64_t)-1)
        bad("orig_rax is 0x%llx, not -1: the dump claims the program was in a"
            " syscall", (unsigned long long)D.greg[CORE_ORIG_RAX]);
    if ((D.greg[CORE_CS] & 3) != 3)
        bad("cs 0x%llx is not a ring-3 selector -- these are not a user"
            " program's registers", (unsigned long long)D.greg[CORE_CS]);
    if (D.logit.flags & CORE_F_SIGCTX)
        bad("CORE_F_SIGCTX: this kernel was built with the negative control on");
    {   uint64_t sum = 0;
        for (int i = 0; i < D.nseg_stored; i++) sum += segs[i].filesz;
        if (sum != D.logit.got_bytes)
            bad("PT_LOAD filesz sums to %llu, note says got_bytes %llu",
                (unsigned long long)sum, (unsigned long long)D.logit.got_bytes);
        for (int i = 0; i < D.nseg_stored; i++)
            if (segs[i].offset + segs[i].filesz > (uint64_t)n)
                bad("PT_LOAD %d points past the end of the file", i);
    }
    /* The stack must be in it. A dump whose register file survives but whose
     * stack did not is a dump with no backtrace in it, which is most of the
     * reason to write one. */
    {   int have_sp = 0;
        for (int i = 0; i < D.nseg_stored; i++)
            if (segs[i].filesz && D.greg[CORE_RSP] >= segs[i].vaddr &&
                D.greg[CORE_RSP] < segs[i].vaddr + segs[i].filesz) have_sp = 1;
        if (!have_sp)
            bad("no dumped segment contains rsp 0x%llx -- there is no stack in"
                " this dump", (unsigned long long)D.greg[CORE_RSP]);
        else printf("ok  : rsp 0x%llx lands inside a dumped segment\n",
                    (unsigned long long)D.greg[CORE_RSP]);
    }

    /* --- the values that came from another channel ----------------------- */
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "corecheck: bad argument %s\n", argv[i]); return 2; }
        *eq = 0;
        const char *k = argv[i];
        uint64_t want = strtoull(eq + 1, NULL, 0);
        uint64_t got;
        if      (!strcmp(k, "signo"))  got = (uint64_t)D.signo;
        else if (!strcmp(k, "pid"))    got = (uint64_t)D.pid;
        else if (!strcmp(k, "cr2"))    got = D.logit.cr2;
        else if (!strcmp(k, "siaddr")) got = D.fault_addr;
        else if (!strcmp(k, "err"))    got = D.logit.err;
        else if (!strcmp(k, "trapno")) got = D.logit.trapno;
        else if (!strcmp(k, "rip"))    got = D.greg[CORE_RIP];
        else if (!strcmp(k, "rsp"))    got = D.greg[CORE_RSP];
        else { fprintf(stderr, "corecheck: unknown key %s\n", k); return 2; }
        if (got != want)
            bad("%s: the dump says 0x%llx, the other channel says 0x%llx",
                k, (unsigned long long)got, (unsigned long long)want);
        else
            printf("ok  : %s 0x%llx agrees with the other channel\n",
                   k, (unsigned long long)got);
    }

    /* --- gdb, the reader that did not come from this tree ---------------- */
    {   char cmd[512], line[8192];
        snprintf(cmd, sizeof cmd,
                 "gdb -batch -nx -c '%s' -ex 'info registers rip rsp' 2>&1", argv[1]);
        FILE *g = popen(cmd, "r");
        int seen_sig = 0; uint64_t grip = 0, grsp = 0;
        if (g) {
            while (fgets(line, sizeof line, g)) {
                if (strstr(line, "SIGSEGV") || strstr(line, "SIGILL") ||
                    strstr(line, "SIGFPE")  || strstr(line, "SIGBUS")) seen_sig = 1;
                if (!strncmp(line, "rip ", 4)) grip = strtoull(strstr(line, "0x") + 2, NULL, 16);
                if (!strncmp(line, "rsp ", 4)) grsp = strtoull(strstr(line, "0x") + 2, NULL, 16);
            }
            pclose(g);
        }
        if (!seen_sig) bad("gdb did not report a fatal signal for this core");
        else printf("ok  : gdb reports the program terminated by a signal\n");
        if (grip != D.greg[CORE_RIP])
            bad("gdb reads rip 0x%llx, our reader reads 0x%llx",
                (unsigned long long)grip, (unsigned long long)D.greg[CORE_RIP]);
        else printf("ok  : gdb agrees with our reader on rip 0x%llx\n",
                    (unsigned long long)grip);
        if (grsp != D.greg[CORE_RSP])
            bad("gdb reads rsp 0x%llx, our reader reads 0x%llx",
                (unsigned long long)grsp, (unsigned long long)D.greg[CORE_RSP]);
        else printf("ok  : gdb agrees with our reader on rsp 0x%llx\n",
                    (unsigned long long)grsp);
    }

    if (fails) { printf("corecheck: %d FAILED\n", fails); return 1; }
    printf("corecheck: the dump agrees with every other channel\n");
    return 0;
}
