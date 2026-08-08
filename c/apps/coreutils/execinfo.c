/* execinfo -- print (and check) the SysV initial stack this process was given.
 *
 * The kernel's execve used to build argc / argv[] / NULL / envp[] / NULL and
 * stop. That is not a small auxiliary vector, it is a MISSING one, and the
 * difference matters: a program that goes looking past the environment's NULL
 * terminator does not find nothing, it finds whatever is there -- on that stack,
 * the environment strings -- and reads them as tag/value pairs. So "there is no
 * auxv" and "the auxv is full of filenames" were the same layout.
 *
 * This program is the on-machine half of the loader's tests. The host battery
 * (tests/unit/exec_test.c) proves the loader accepts the bytes and maps them
 * with the right permissions; only a real ring-3 process can prove that what
 * arrived on ITS stack is what the kernel meant to put there, that AT_PHDR
 * points at readable memory containing this program's own headers, and that
 * AT_RANDOM is 16 bytes of something.
 *
 * Every line it prints is prefixed EXECINFO so a boot harness can grep for
 * them, and it ends with EXECINFO ok / EXECINFO FAIL plus a nonzero status.
 */
#include "logit.h"
#include "clib.h"

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID   11
#define AT_EUID  12
#define AT_GID   13
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

static int fails;

static void kv(const char *k, unsigned long v)
{
    outs("EXECINFO "); outs(k); outs(" = ");
    outs("0x");
    char t[17]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { int d = (int)(v & 15); t[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (i) outc(t[--i]);
    outc('\n');
}

static void claim(int ok, const char *what)
{
    outs(ok ? "EXECINFO ok: " : "EXECINFO FAIL: ");
    outs(what);
    outc('\n');
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    /* envp starts one past argv's NULL, and the auxv one past envp's. There is
     * no other way in: crt0_cli hands main only argc and argv, which is exactly
     * how a SysV program finds these on any system. */
    char **envp = argv + argc + 1;
    int envc = 0;
    while (envp[envc]) envc++;
    unsigned long *aux = (unsigned long *)(envp + envc + 1);

    unsigned long v[64];
    int have[64];
    for (int i = 0; i < 64; i++) { v[i] = 0; have[i] = 0; }

    int n = 0;
    for (; n < 64 && aux[n * 2] != AT_NULL; n++) {
        unsigned long tag = aux[n * 2], val = aux[n * 2 + 1];
        if (tag < 64) { v[tag] = val; have[tag] = 1; }
    }

    outs("EXECINFO argc="); outn(argc); outs(" envc="); outn(envc);
    outs(" auxc="); outn(n); outc('\n');

    kv("AT_PHDR",   v[AT_PHDR]);
    kv("AT_PHENT",  v[AT_PHENT]);
    kv("AT_PHNUM",  v[AT_PHNUM]);
    kv("AT_PAGESZ", v[AT_PAGESZ]);
    kv("AT_ENTRY",  v[AT_ENTRY]);
    kv("AT_RANDOM", v[AT_RANDOM]);
    kv("AT_BASE",   v[AT_BASE]);
    kv("AT_SECURE", v[AT_SECURE]);

    claim(n >= 10, "the auxiliary vector has entries and a real AT_NULL terminator");
    claim(have[AT_PAGESZ] && v[AT_PAGESZ] == 0x1000, "AT_PAGESZ is 4096");
    claim(have[AT_ENTRY] && v[AT_ENTRY] >= 0x40000000ul && v[AT_ENTRY] < 0x80000000ul,
          "AT_ENTRY is inside the private user region");
    claim(have[AT_BASE] && v[AT_BASE] == 0, "AT_BASE is 0: there is no interpreter");
    claim(have[AT_SECURE] && v[AT_SECURE] == 0, "AT_SECURE is present and 0");
    claim(have[AT_PHENT] && v[AT_PHENT] == 56, "AT_PHENT is 56 (an ELF64 phdr)");
    claim(have[AT_PHNUM] && v[AT_PHNUM] > 0 && v[AT_PHNUM] <= 64, "AT_PHNUM is sane");

    /* AT_PHDR must be READABLE and must actually hold this program's program
     * headers. Reading it is the test: if the loader had pointed it at an
     * address it never mapped, this dereference kills the process and the
     * harness sees no "ok" line at all. */
    int phok = 0, foundx = 0;
    if (have[AT_PHDR] && v[AT_PHDR]) {
        unsigned char *p = (unsigned char *)v[AT_PHDR];
        phok = 1;
        for (unsigned long i = 0; i < v[AT_PHNUM]; i++) {
            unsigned char *e = p + i * 56;
            unsigned int type = (unsigned int)e[0] | ((unsigned int)e[1] << 8) |
                                ((unsigned int)e[2] << 16) | ((unsigned int)e[3] << 24);
            unsigned int flags = (unsigned int)e[4] | ((unsigned int)e[5] << 8);
            if (type == 1 && (flags & 1)) foundx = 1;    /* PT_LOAD, PF_X */
        }
    }
    claim(phok, "AT_PHDR points at readable memory");
    claim(foundx, "... and the table there contains this program's executable PT_LOAD");

    /* The ELF header the loader copied in front of the table. */
    if (have[AT_PHDR] && v[AT_PHDR] >= 64) {
        unsigned char *eh = (unsigned char *)(v[AT_PHDR] - 64);
        claim(eh[0] == 0x7f && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F',
              "the ELF header sits directly in front of AT_PHDR");
    }

    int nz = 0;
    if (have[AT_RANDOM] && v[AT_RANDOM]) {
        unsigned char *r = (unsigned char *)v[AT_RANDOM];
        for (int i = 0; i < 16; i++) nz |= r[i];
    }
    claim(nz != 0, "AT_RANDOM is 16 readable bytes and not all zero");

    if (have[AT_EXECFN] && v[AT_EXECFN]) {
        outs("EXECINFO AT_EXECFN = "); outs((const char *)v[AT_EXECFN]); outc('\n');
        claim(1, "AT_EXECFN is a readable string");
    } else {
        claim(0, "AT_EXECFN is set");
    }

    if (fails) { outs("EXECINFO FAIL\n"); return 1; }
    outs("EXECINFO ok\n");
    return 0;
}
