#include "as.h"
#ifndef AS_SELFHOST_COMPILER
#include "lexer.h"      /* the C lexer -- host builds only; see the block below */
#endif
#include "logit_abi.h"  /* SYS_CAP_QUERY + the kernel's CAP_* bits (M28) */
#include <stdio.h>      /* FILE, fopen/fread/fclose, stdin */
#include <stdlib.h>     /* malloc/realloc/free */
#include <string.h>     /* strcmp (-c mode) */

/* /bin/as : run a script file (as foo.as) or, with no argument, read a whole
 * program from stdin (so `echo 'print(6*7)' | as` works). The interpreter core
 * is portable; this file is the Logit/host entry that does the file I/O. */

/* ===========================================================================
 * SW-shipped-compiler: THIS FILE IS NOW TWO BINARIES.
 *
 * as.c is the one .c in c/apps/as that is NOT in $(AS_CORE) -- it is the entry
 * point, and it is therefore the only place where the host `asc` and the
 * on-device /bin/as are allowed to differ. They now differ in the one way the
 * milestone is about:
 *
 *   build/asc   (host, no -DAS_SHIPPED)   = this file + AS_CORE, compiler.c and
 *       lexer.c included. It is the BOOTSTRAP and the ORACLE: it precompiles
 *       every fsroot/as/lib .as to .la at build time (including asc.la and
 *       the asboot.la embedded below), and tests/unit/run-as-crosscheck.sh
 *       diffs its bytecode against the self-hosted compiler's over the whole
 *       corpus. `asc -c` and `asc -lex` keep working exactly as before. This
 *       binary must keep containing a C compiler, and it does.
 *
 *   build/as.elf -> /bin/as  (-DAS_SELFHOST_COMPILER, AS_CORE minus compiler.c
 *       and lexer.c) = the binary that SHIPS. It contains no C compiler at
 *       all. Source -> bytecode is done by /usr/as/lib/asc.la, the compiler
 *       written in AetherScript, interpreted by the C VM. See boot_compile().
 *
 * WHAT THAT COSTS, stated where someone will hit it rather than in a spec:
 *
 *  - /bin/as needs TWO FILES ON THE DISK to compile anything:
 *    /usr/as/lib/asc.la and /usr/as/lib/aslex.la (asc.as imports aslex). If
 *    either is missing or stale, /bin/as says so BY PATH and exits 1. It never
 *    falls back to anything, because there is nothing to fall back to -- and a
 *    silent fallback is precisely what would make this change unverifiable.
 *
 *  - `import X` where X.la does not exist is now a hard error instead of
 *    compiling X.as. vm.c's as_import prefers X.la and only reaches the source
 *    fallback when the .la is absent or version-stale; that fallback called the
 *    C compiler, and the stub below is what is left of it. Every module the
 *    tree ships (the whole $(AS_LA) set, 17 files) has a .la on the disk, so no
 *    in-tree import changes behaviour. A user's own module must be compiled
 *    first: `as -c mod.as -o mod.la`. Making this case work instead of fail
 *    would need a REENTRANT compile -- as_import runs inside run_until(), and
 *    as_run() begins with reset_stack() + nmodules = 0, so driving the
 *    AetherScript compiler from there would destroy the program that asked.
 *    Closing it needs a save/restore entry point in vm.c; that is not this
 *    unit's file to edit.
 *
 *  - running a .la (`as -run x.la`, and every import of a .la) pays NOTHING
 *    for any of this: boot_compile() is only ever called from a path that has
 *    source in hand, and the preflight that opens asc.la lives inside it.
 * ======================================================================== */

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
 * ON THE DEVICE IT FAILS CLOSED: a negative return from a real kernel means a
 * process that cannot learn its grant, and such a process has not been
 * granted anything.
 *
 * ON THE HOST IT GRANTS EVERYTHING, and the distinction is COMPILE-TIME
 * (__STDC_HOSTED__, the same switch as_ll.c uses to stub the syscall bridge),
 * not the runtime -1. The first version treated the host stub's -1 as the
 * failure case and left the CLI at deny-everything -- which silently broke
 * `make test-ash` (ash.as could not open() or run() anything on the host; 4
 * checks red for a day before anyone connected them to M28). Its stated
 * justification -- "keeps test-as-cap's default-deny assertions true" -- was
 * checked and is FALSE: test-as-cap builds its own binary from as_cap_test.c
 * and never executes this main(), so those assertions test object.c's static
 * default, not this function. On a hosted build the LogitOS grant has no
 * referent (the host OS enforces its own boundaries) and asc is a dev tool;
 * unscoped-everything is the pre-M28 behavior every host harness was written
 * against. --scope still narrows on the host, which is what the language
 * tests use. */
static void install_kernel_grant(void)
{
#if __STDC_HOSTED__
    as_caps_set(AS_CAP_FS_READ | AS_CAP_FS_WRITE | AS_CAP_NET |
                AS_CAP_PROC | AS_CAP_GUI | AS_CAP_RAW, NULL);
    return;
#else
    char prefix[128];
    prefix[0] = 0;
    long k = as_ll_syscall(SYS_CAP_QUERY, (long)(uintptr_t)prefix, (long)sizeof prefix, 0);
    if (k < 0) return;                      /* real kernel refused -- stay at deny */

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
#endif
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

#ifdef AS_SELFHOST_COMPILER
/* --- the shipped compiler: /usr/as/lib/asc.la, driven through asboot ------ */

/* asboot.la: c/apps/as/asboot.as compiled by the host asc at build time.
 * `static const unsigned char as_boot_la[]` + `as_boot_la_len`. */
#include "asboot_la.inc"

/* Named here once, and printed verbatim when they are missing. Both are
 * /usr/as/lib/ because that is the second entry in vm.c's module search path
 * (after the cwd) and where the Makefile packs $(AS_LA). */
#define ASC_LA_PATH   "/usr/as/lib/asc.la"
#define ASLEX_LA_PATH "/usr/as/lib/aslex.la"

/* vm.c calls as_compile_module() when an imported module has no usable .la, and
 * as_interpret() (also in vm.c, unused here) calls as_compile(). Both are
 * compiler.c's, and compiler.c is not linked into this binary -- so these two
 * stubs are what the linker resolves against. They FAIL, loudly and by name.
 *
 * The alternative -- keeping compiler.c linked "just for imports" -- is the one
 * thing that would make the whole change unverifiable: the shipped binary would
 * still contain a C compiler, and no test could tell which of the two did the
 * work. */
ObjFn *as_compile_module(const char *src, ObjModule *m)
{
    (void)src;
    snprintf(as_err, sizeof as_err,
             "module '%.*s' has no compiled %.*s.la, and this /bin/as contains no C "
             "compiler -- run `as -c %.*s.as -o %.*s.la` first",
             m && m->name ? m->name->len : 1, m && m->name ? m->name->chars : "?",
             m && m->name ? m->name->len : 1, m && m->name ? m->name->chars : "?",
             m && m->name ? m->name->len : 1, m && m->name ? m->name->chars : "?",
             m && m->name ? m->name->len : 1, m && m->name ? m->name->chars : "?");
    return NULL;
}

ObjFn *as_compile(const char *src)
{
    (void)src;
    snprintf(as_err, sizeof as_err,
             "internal: as_compile() called in a /bin/as with no C compiler");
    return NULL;
}

/* Both halves of the compiler must be on the disk before we try. Reported as a
 * PATH, because "cannot import module 'asc'" (what the VM would say two layers
 * down) does not tell anyone which file to put back -- and /bin/as is what the
 * GUI Terminal's shell is, so this message is the one a stuck user sees. */
static int asc_preflight(void)
{
    static const char *const need[] = { ASC_LA_PATH, ASLEX_LA_PATH };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(need[i], "rb");
        if (f) { fclose(f); continue; }
        as_emit_cstr("as: cannot open ");
        as_emit_cstr(need[i]);
        as_emit_cstr("\nas: this /bin/as has no C compiler; it compiles AetherScript by "
                     "running the AetherScript compiler, and that file is where it lives\n");
        return 0;
    }
    return 1;
}

/* Compile `src` to .la bytes with the AetherScript compiler.
 *
 * Returns a malloc'd buffer (caller frees) + its length, or NULL with the
 * reason already printed. The bytes are copied OUT of the VM heap on purpose:
 * the next as_run() sets nmodules = 0, which unroots every module from this
 * run, and the blob would then be collectable while we were still reading it.
 *
 * GC discipline: everything from as_load() to the moment as_run() registers the
 * module happens under push_disable, because until then `m` is reachable from
 * nothing at all. as_run() itself re-enables only after modules[0] = m. */
static uint8_t *boot_compile(const char *src, int *out_len)
{
    if (!asc_preflight()) return NULL;

    as_gc_push_disable();
    ObjFn *boot = as_load(as_boot_la, (int)as_boot_la_len);
    ObjModule *m = boot ? as_module_new("__asboot", 8) : NULL;
    ObjStr *key = m ? as_str_copy("__src", 5) : NULL;
    ObjStr *text = key ? as_str_copy(src, (int)strlen(src)) : NULL;
    Value *slot = text ? as_module_slot(m, key, 1) : NULL;
    if (!slot) {
        as_gc_pop_disable();
        as_emit_cstr(boot ? "as: out of memory preparing the compiler\n"
                          : "as: the embedded asboot bytecode did not load "
                            "(rebuild: build/asboot_la.inc is stale)\n");
        return NULL;
    }
    *slot = OBJ_VAL(text);
    stamp_tree(boot, m);          /* pointer writes only; mirrors the -run path */
    as_gc_pop_disable();

    if (as_run(boot)) {           /* a failure INSIDE the compiler, not in the program */
        as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n");
        as_emit_cstr(as_traceback());
        return NULL;
    }

    /* m is still rooted: as_run left it at modules[0]. Safe to allocate keys. */
    Value *ev = as_module_slot(m, as_str_copy("__err", 5), 0);
    if (ev && IS_STR(*ev)) {
        ObjStr *e = AS_STR(*ev);
        as_emit_cstr("as: ");
        as_emit(e->chars, e->len);
        as_emit_cstr("\n");
        return NULL;
    }
    Value *bv = as_module_slot(m, as_str_copy("__blob", 6), 0);
    if (!bv || !IS_STR(*bv)) {
        as_emit_cstr("as: the compiler returned no bytecode and no error -- "
                     "asboot and " ASC_LA_PATH " disagree about their contract\n");
        return NULL;
    }
    ObjStr *blob = AS_STR(*bv);
    uint8_t *buf = (uint8_t *)malloc((size_t)blob->len ? (size_t)blob->len : 1);
    if (!buf) { as_emit_cstr("as: out of memory\n"); return NULL; }
    memcpy(buf, blob->chars, (size_t)blob->len);
    *out_len = blob->len;
    return buf;
}

/* Compile + run, the shipped replacement for as_interpret(). Two as_run()s: the
 * compiler's, then the program's.
 *
 * as_free_objects() BETWEEN THEM IS LOAD-BEARING, not tidiness. Without it the
 * compiler's heap -- asc's ~100 functions, its token list, the whole constant
 * pool -- stays allocated until the program's first collection, so a program
 * that itself imports asc (fsroot/as/examples/selfhost.as does) holds TWO
 * copies of the compiler at once inside a 24 MiB arena. Freeing here also means
 * the program starts on exactly the heap it started on when the compiler was C.
 *
 * It is only safe because NO GARBAGE COLLECTION CAN HAPPEN between the free and
 * as_run()'s `nmodules = 0`: modules[] still points at freed modules until
 * then, and gc_mark_roots() walks it. as_free_objects() resets gc_disabled to
 * 0, so the push must come after it; as_load() disables collection across its
 * whole body; and the only other allocations here are inside the same
 * push/pop. Reordering any of that is a use-after-free, not a style change. */
static int boot_interpret(const char *src)
{
    int len = 0;
    uint8_t *bc = boot_compile(src, &len);
    if (!bc) return 1;

    as_free_objects();
    as_gc_push_disable();
    ObjFn *fn = as_load(bc, len);
    ObjModule *m = fn ? as_module_new("__main__", 8) : NULL;
    if (!m) {
        as_gc_pop_disable();
        free(bc);
        as_emit_cstr(fn ? "as: out of memory\n"
                        : "as: the compiler produced bytecode this VM will not load "
                          "(AS_BC_VERSION skew between /bin/as and " ASC_LA_PATH ")\n");
        return 1;
    }
    free(bc);
    stamp_tree(fn, m);
    as_gc_pop_disable();

    /* args() was already installed by main() before we were called (the caller
     * owns argv); as_run() does not touch it, so the compiler run above cannot
     * have disturbed it. */
    int rc = as_run(fn);
    if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n");
              as_emit_cstr(as_traceback()); }
    return rc;
}
#endif /* AS_SELFHOST_COMPILER */

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
    /* A trailing --scope is REFUSED, not passed through. Everything after the
     * script path belongs to the script's args(), so `as script.as --scope P`
     * used to run UNNARROWED with two extra args -- a silent grant of exactly
     * what the caller asked to give up, which is the one failure mode M28's
     * spec names as worse than any refusal. The tree's own on-device gate
     * (run-as-cap-test.sh) shipped with that spelling and proved the silence
     * for real: its scoped run read /etc. The spelling "--scope" is hereby
     * reserved out of script argv; a script that wants those bytes as data
     * can take them from a file. */
    for (int ai = 2; ai < argc; ai++) {
        if (strcmp(argv[ai], "--scope") == 0) {
            as_emit_cstr("as: --scope must come BEFORE the script path "
                         "(as --scope PATH script.as ...); refusing to run "
                         "unnarrowed with the flag as script args\n");
            return 1;
        }
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
#ifdef AS_SELFHOST_COMPILER
        /* Same output as the C path, produced by the AetherScript compiler:
         * asc's dump_module() serializes the SAME LAQ1 bytes as_dump() does
         * (that equality is what test-as-crosscheck measures over 52 files), so
         * writing the blob straight out is not an approximation of `-c`, it is
         * `-c`. */
        int blen = 0;
        uint8_t *blob = boot_compile(src, &blen);
        free(src);
        if (!blob) { as_free_objects(); return 1; }
        FILE *out = fopen(argv[4], "wb");
        if (!out) { free(blob); as_emit_cstr("as: cannot create "); as_emit_cstr(argv[4]); as_emit_cstr("\n"); as_free_objects(); return 1; }
        size_t wr = fwrite(blob, 1, (size_t)blen, out);
        fclose(out);
        free(blob);
        int rc = (wr == (size_t)blen) ? 0 : 1;
        if (rc) { as_emit_cstr("as: short write to "); as_emit_cstr(argv[4]); as_emit_cstr("\n"); }
        as_free_objects();
        return rc;
#else
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
#endif
    }

    /* Token-dump mode (M21-P3 S1 fixpoint harness): `as -lex file.as` prints one
     * "type line len checksum" per token -- compared byte-for-byte against the
     * self-hosted lexer (lib/aslex.as) over the whole corpus.
     *
     * HOST ONLY, and that is the point rather than an omission: -lex is the C
     * lexer's output, its single caller is tests/unit/run-selfhost-lex.sh which
     * runs build/asc, and keeping it in /bin/as would keep as_lex() -- i.e.
     * lexer.c -- linked into the binary this milestone is emptying. `nm` finding
     * as_lex in as.elf is one of the things tests/unit/run-as-shipped.sh
     * refuses. */
#ifndef AS_SELFHOST_COMPILER
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
#endif

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

#ifdef AS_SELFHOST_COMPILER
    /* as_interpret() is vm.c's compile-then-run, and its compile half is
     * compiler.c. boot_interpret() is the same two steps with the AetherScript
     * compiler doing the first one. */
    int rc = boot_interpret(src);
    free(src);
    as_free_objects();
    return rc;
#else
    int rc = as_interpret(src);
    if (rc) { as_emit_cstr("as: "); as_emit_cstr(as_err); as_emit_cstr("\n");
              as_emit_cstr(as_traceback()); }   /* empty for a compile error: no VM stack */
    free(src);
    as_free_objects();
    return rc;
#endif
}
