#include "as.h"
#include "lexer.h"
#include "logit_abi.h"  /* SYS_CAP_QUERY + the kernel's CAP_* bits (M28) */
#include <stdio.h>      /* FILE, fopen/fread/fclose, stdin */
#include <stdlib.h>     /* malloc/realloc/free */
#include <string.h>     /* strcmp (-c mode) */

/* /bin/as : run a script file (as foo.as) or, with no argument, read a whole
 * program from stdin (so `echo 'print(6*7)' | as` works). The interpreter core
 * is portable; this file is the Logit/host entry that does the file I/O. */

/* M28: learn what the KERNEL granted this process, and install it as the VM's
 * held set. Without this the two halves of the milestone never meet -- the
 * kernel maintains a per-process grant and narrows it across SYS_CAP_SPAWN, and
 * the VM enforces one it was never told about, so on the real machine every
 * script would run at the host default of DENY EVERYTHING and could not open a
 * file. Nothing in the host test suite can see that: as_cap_test.c installs its
 * own set through the C entry point, which is exactly the thing the device does
 * not have.
 *
 * THE TWO BITMAPS ARE NOT THE SAME BITMAP, and copying one into the other is
 * the bug this function exists to not have. The kernel's CAP_* (logit_abi.h)
 * and the language's AS_CAP_* (as.h) were designed on the two sides of the
 * contract and their values do NOT line up:
 *
 *     kernel   CAP_FS 0x01   CAP_NET 0x02   CAP_RAW 0x04   CAP_PROC 0x08 ...
 *     language FS_READ 0x01  FS_WRITE 0x02  NET 0x04       PROC 0x08 ...
 *
 * A straight assignment would hand a network-only process AS_CAP_FS_WRITE and a
 * raw-only process AS_CAP_NET -- silently, with no error anywhere, granting
 * authority the kernel refused. So the mapping is written out one bit at a time
 * and must stay that way; there is no shortcut here worth taking.
 *
 * CAP_FS becomes BOTH read and write. The kernel gates path access without
 * distinguishing direction (syscall_cap_class() classifies SYS_READ_FILE and
 * SYS_WRITE_FILE alike), so splitting it here would invent a refusal the kernel
 * never made. The language keeps the finer distinction because attenuation can
 * use it: a script holding both may hand a child read-only.
 *
 * FAILS CLOSED. A negative return means no kernel to ask -- the host build,
 * where as_ll_syscall stubs to -1 -- and the held set is then left at its
 * static default of zero. That is what keeps `make test-as-cap`'s
 * default-deny assertions true, and it is the right answer for a real failure
 * too: a process that cannot learn its grant has not been granted anything. */
static void install_kernel_grant(void)
{
    char prefix[128];
    prefix[0] = 0;
    long k = as_ll_syscall(SYS_CAP_QUERY, (long)(uintptr_t)prefix, (long)sizeof prefix, 0);
    if (k < 0) return;                      /* no kernel (host) -- stay at deny */

    uint32_t bits = 0;
    if (k & CAP_FS)   bits |= AS_CAP_FS_READ | AS_CAP_FS_WRITE;
    if (k & CAP_NET)  bits |= AS_CAP_NET;
    if (k & CAP_RAW)  bits |= AS_CAP_RAW;
    if (k & CAP_PROC) bits |= AS_CAP_PROC;
    if (k & CAP_GUI)  bits |= AS_CAP_GUI;

    /* An empty prefix from the kernel means "unscoped", which the language
     * spells NULL. Passing "" through would be read as a prefix that contains
     * nothing but the empty path. */
    as_caps_set(bits, prefix[0] ? prefix : NULL);
}

static char *slurp(FILE *f)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, 4096, f);
        len += r;
        if (r < 4096) break;       /* short read == EOF (our fread only stops short at EOF) */
    }
    buf[len] = 0;
    return buf;
}

/* Recursively stamp ->module on fn and every O_FN constant (mirrors the VM's
 * stamp_module). Recursion depth is safe: as_load caps FN-constant nesting
 * (AS_LA_MAX_DEPTH in as_bc.c). Pointer writes only -- allocates nothing. */
static void stamp_tree(ObjFn *fn, ObjModule *m)
{
    fn->module = m;
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i])) stamp_tree(AS_FN(fn->consts[i]), m);
}

int main(int argc, char **argv)
{
    char *src;

    /* Before anything else, including the compile-only path: a capability the
     * VM does not know it holds is a capability nobody has. */
    install_kernel_grant();

    /* `as --scope PATH script.as ...` runs the script under a capability
     * narrowed to PATH. This is attenuation at the command line, and it is the
     * only reason it exists: without it there is no way to launch the SAME
     * script twice under two different grants, and a single run that reports
     * "denied" is equally consistent with the check working, with the file not
     * existing, and with the script never having started. The M28 on-device
     * gate (tests/boot/run-as-cap-test.sh) is exactly that pair of runs.
     *
     * IT CAN ONLY NARROW. as_caps_permit_path() answers "is PATH inside the
     * prefix I already hold", so a request that is not contained is REFUSED
     * rather than quietly applied -- `as --scope /` from a process scoped to
     * /usr must not be a way back out. Note the flag consumes its two argv
     * entries before as_set_args() runs, so args() never sees them. */
    if (argc >= 3 && strcmp(argv[1], "--scope") == 0) {
        if (!as_caps_permit_path(argv[2])) {
            as_emit_cstr("as: --scope ");
            as_emit_cstr(argv[2]);
            as_emit_cstr(" is not inside the capability this process holds\n");
            return 1;
        }
        as_caps_set(as_caps_bits(), argv[2]);
        argv += 2; argc -= 2;
    }

    /* Compile-only mode: `as -c in.as -o out.la` writes a .la (LAQ1 header +
     * serialized bytecode). as_compile gives a standalone ObjFn whose ->module is
     * the throwaway __main__ (as_dump never serializes ->module). as.c builds into
     * both the host asc and on-Logit /bin/as, so both get this mode. */
    if (argc == 5 && strcmp(argv[1], "-c") == 0 && strcmp(argv[3], "-o") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }
        ObjFn *fn = as_compile(src);
        free(src);
        if (!fn) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); as_free_objects(); return 1; }
        FILE *out = fopen(argv[4], "wb");
        if (!out) { as_emit_cstr("as: cannot create "); as_emit_cstr(argv[4]); as_emit_cstr("\n"); as_free_objects(); return 1; }
        int rc = as_dump(fn, out);
        fclose(out);
        if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); }
        as_free_objects();
        return rc;
    }

    /* Token-dump mode (M21-P3 S1 fixpoint harness): `as -lex file.as` prints one
     * "type line len checksum" per token -- compared byte-for-byte against the
     * self-hosted lexer (lib/aslex.as) over the whole corpus. */
    if (argc == 3 && strcmp(argv[1], "-lex") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }
        int count = 0;
        Token *toks = as_lex(src, &count);
        if (!toks) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n"); free(src); return 1; }
        for (int i = 0; i < count; i++) {
            unsigned sum = 0;
            for (int k = 0; k < toks[i].len; k++) sum = (sum + (unsigned char)toks[i].start[k]) % 9973;
            printf("%d %d %d %u\n", (int)toks[i].type, toks[i].line, toks[i].len, sum);
        }
        free(toks);
        free(src);
        return 0;
    }

    /* Disassemble a .la (`as -dis foo.la`): one line per instruction, so a
     * self-hosting fixpoint break can be diffed instead of hex-dumped. Loads
     * through as_load, so the verifier still gates malformed input. */
    if (argc == 3 && strcmp(argv[1], "-dis") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        if (!src) { fclose(f); as_emit_cstr("as: out of memory\n"); return 1; }
        long blen = ftell(f);            /* .la is binary; recover length like -run does */
        fclose(f);
        ObjFn *fn = as_load((const uint8_t *)src, (int)blen);
        free(src);
        if (!fn) { as_emit_cstr("as: bad .la file\n"); as_free_objects(); return 1; }
        as_disasm(fn);
        as_free_objects();
        return 0;
    }

    /* Run a compiled .la directly (M21-P3 S2 harness): load + stamp + run. */
    if (argc >= 3 && strcmp(argv[1], "-run") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[2]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        if (!src) { fclose(f); as_emit_cstr("as: out of memory\n"); return 1; }
        /* slurp NUL-terminates but .la is binary: recover the true length from the
         * SAME open file (slurp stopped at EOF, so the position is the byte count) --
         * re-opening would race a concurrent modification of the file. */
        long blen = ftell(f);
        fclose(f);
        ObjFn *fn = as_load((const uint8_t *)src, (int)blen);
        free(src);
        if (!fn) { as_emit_cstr("as: bad .la file\n"); as_free_objects(); return 1; }
        as_gc_push_disable();
        ObjModule *m = as_module_new("__main__", 8);
        if (!m) { as_gc_pop_disable(); as_emit_cstr("as: out of memory\n"); as_free_objects(); return 1; }
        stamp_tree(fn, m);   /* mirrors import's stamp_module (recursion is depth-capped by as_load) */
        as_gc_pop_disable();
        as_set_args(argc - 2, argv + 2);
        int rc = as_run(fn);
        if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n");
                  as_emit_cstr(as_traceback()); }
        as_free_objects();
        return rc;
    }

    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { as_emit_cstr("as: cannot open "); as_emit_cstr(argv[1]); as_emit_cstr("\n"); return 1; }
        src = slurp(f);
        fclose(f);
        as_set_args(argc - 1, argv + 1);    /* args() = [script, script-args...] */
    } else {
        src = slurp(stdin);
    }
    if (!src) { as_emit_cstr("as: out of memory\n"); return 1; }

    int rc = as_interpret(src);
    if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n");
              as_emit_cstr(as_traceback()); }   /* empty for a compile error: no VM stack */
    free(src);
    as_free_objects();
    return rc;
}
