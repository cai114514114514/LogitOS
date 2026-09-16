/* These three were reached through legacy/vm.h, which included them on the way
 * to declaring the VM. Naming them directly is what deleting that header
 * exposed: the CLI depends on the diagnostics writer, the source-version reader
 * and the A3 project driver -- and on no engine at all. */
#include "common/diagnostic.h"   /* as_diagnostic_* + as_diagnostics */
#include "common/numeric.h"      /* as_source_version, AS_LANGUAGE_NATIVE */
#include "include/project.h"     /* as_typed_command, as_typed_check_cli */
#include "frontend/lexer.h"      /* as_lex + Token, for -lex: the A3 C lexer */
#include <stdio.h>      /* FILE, fopen/fread/fclose, stdin */
#include <stdlib.h>     /* malloc/realloc/free */
#include <string.h>     /* strcmp, memchr */

/* /bin/as : the AetherScript 3 entry point, on the host as `asc` and on the
 * machine as /bin/as. Subcommands live in cli/commands.c; this file is the
 * argv front door, `check`, and the refusal that is left where A2 used to be.
 *
 * ===========================================================================
 * WHAT THIS FILE USED TO BE, because the shape it left behind is still visible.
 *
 * Until the A2 engine was deleted this was TWO binaries out of one source:
 * build/asc carried the C compiler (legacy/compiler.c) and precompiled every
 * fsroot/as/lib module to .la at build time; build/as.elf shipped WITHOUT a C
 * compiler and compiled by interpreting /usr/as/lib/asc.la -- the AetherScript
 * compiler written in AetherScript -- on the C VM. Roughly 460 lines here were
 * that arrangement: stamp_tree, boot_compile, boot_interpret, an asc.la/aslex.la
 * preflight, and two as_compile stubs whose whole job was to fail by name when
 * the shipped binary was asked to compile.
 *
 * ALL OF IT IS GONE, and what replaced it is smaller because the question
 * changed. A3 does not produce bytecode for a VM to interpret; it lowers to
 * native code on the host and ships .aex. So there is nothing for the device to
 * interpret, no .la to load, and no second compiler to bootstrap.
 *
 * WHAT THAT COSTS, stated where someone will hit it rather than in a spec:
 * THE MACHINE CAN NO LONGER COMPILE AETHERSCRIPT. `as -c`, `as -run`, `as -dis`,
 * `as -lex` and `as script.as` for an A2 script are all gone with the engine
 * that answered them. `as check` still works on device and is still real -- it
 * runs the A3 front end, which is C and needs no VM -- and `as build` needs the
 * host. A user's own module is compiled by `asc build` on the host and arrives
 * as a native artifact, the same way every shipped example now does.
 * ======================================================================== */


static size_t slurp_bytes;
static char *slurp(FILE *f)
{
    size_t cap = 4096, len = 0;slurp_bytes=0;
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
        if (r < 4096) {
            if (ferror(f)) { free(buf); return NULL; }
            break;
        }
    }
    buf[len] = 0;slurp_bytes=len;
    return buf;
}


int main(int argc, char **argv)
{
    char *src;

    /* NO CAPABILITY GRANT IS INSTALLED HERE, and the absence is the point.
     * install_kernel_grant() used to read SYS_CAP_QUERY and hand the result to
     * the VM before it executed a script -- the M28 grant had to reach the
     * interpreter, because the interpreter was what ran user code. This binary
     * no longer runs user code: it type-checks and it drives the host compiler.
     * The program that does run is a native .aex, and runtime/capability.c's
     * at_caps_init() -- the same SYS_CAP_QUERY read, the same kernel-bits to
     * language-bits mapping -- runs inside IT, where the authority is actually
     * used. Calling it here as well would be the second door on that jar, and
     * would also drag a runtime unit into the compiler, which sources.mk keeps
     * apart on purpose. */
    int native_command=as_typed_command(argc,argv);
    if(native_command>=0)return native_command;

    /* `check` compiles but NEVER executes user code, including imports.
     * stdin gives Studio a check of its unsaved buffer, without a save or a
     * shared temporary file. The label is for diagnostics only. */
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        int json=0,from_stdin=0;const char *path=NULL;
        for(int i=2;i<argc;i++) {
            if(!strcmp(argv[i],"--json"))json=1;
            else if(!strcmp(argv[i],"--stdin"))from_stdin=1;
            else if(!path)path=argv[i];
            else {fprintf(stderr,"usage: as check [--json] [--stdin] FILE\n");return 2;}
        }
        if(!path){fprintf(stderr,"usage: as check [--json] [--stdin] FILE\n");return 2;}
        FILE *input=from_stdin?stdin:fopen(path,"rb");
        if(!input){as_diagnostic_begin(path,"");as_diagnostic_add("AS9000","Cannot open source file",0,0);
            as_diagnostic_write(stdout,json);as_diagnostic_end();return 1;}
        src=slurp(input);int io_error=!src;
        if(!from_stdin&&fclose(input))io_error=1;
        as_diagnostic_begin_n(path,src?src:"",src?slurp_bytes:0);
        if(io_error)as_diagnostic_add("AS9000","Could not read the complete source file",0,0);
        else if(memchr(src,0,slurp_bytes)){
            size_t at=(size_t)((char *)memchr(src,0,slurp_bytes)-src);
            as_diagnostic_add("AS1001","NUL is not allowed in source text",at,at+1);
        }
        else if(as_source_version(src) == AS_LANGUAGE_NATIVE) {
            AsSourceOverlay overlay={path,src,slurp_bytes};
            AsTypedProject *project=as_typed_check_cli(path,&overlay,1,argv[0],NULL);
            as_typed_report(project,stdout,json);int status=as_typed_errors(project)?1:0;
            as_typed_free(project);as_diagnostic_end();free(src);return status;
        }
        else {
            /* Reported as a DIAGNOSTIC rather than silently passing, because
             * Studio drives this path for its unsaved buffer: an A2 file that
             * "checks clean" because nothing checked it is the worst of the
             * three possible answers. */
            as_diagnostic_add("AS3302",
                              "Not an A3 source: the first line must be "
                              "'# aether: 3.0'. The AetherScript 2 engine has "
                              "been removed and cannot check this file.", 0, 0);
        }
        int rc=as_diagnostics.count?1:0;
        as_diagnostic_write(stdout,json);as_diagnostic_end();
        /* as_free_objects() was the A2 VM's object-registry sweep and went with
         * it. The A3 check path allocates through its own project arena, which
         * as_typed_free() releases on the branch above; this branch reached the
         * diagnostic writer without building one. */
        free(src);return rc;
    }

    /* `-lex` STAYS, and it was deleted once by mistake in the A2 removal because
     * it sat among the A2 flags. The lexer it drives is frontend/lexer.c -- the
     * A3 C lexer, no VM anywhere near it -- and it is the ORACLE the A3 lexer
     * library is differentially tested against: test-as-lexer-lib compares
     * aslex.as's token stream against this one, so deleting it did not remove a
     * legacy feature, it removed the only independent reading of a live A3 gate.
     *
     * It calls as_lex_source() rather than as_lex(): the latter was the thin
     * wrapper that reported through the VM's as_err global, and it went with the
     * VM. Errors are reported through AsLexError, which carries the message and
     * distinguishes a lexical error from an allocation failure. The per-token
     * checksum rather than the text keeps the output diffable without quoting
     * rules. */
    if (argc == 3 && strcmp(argv[1], "-lex") == 0) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { fprintf(stderr, "as: cannot open %s\n", argv[2]); return 1; }
        src = slurp(f);
        fclose(f);
        if (!src) { fprintf(stderr, "as: out of memory\n"); return 1; }
        int count = 0;
        AsLexError lex_error = {0};
        Token *toks = as_lex_source(src, &count, &lex_error, NULL);
        if (!toks) {
            fprintf(stderr, "as: %s: %s\n", argv[2],
                    lex_error.out_of_memory ? "out of memory" : lex_error.message);
            free(src);
            return 1;
        }
        for (int i = 0; i < count; i++) {
            unsigned sum = 0;
            for (int k = 0; k < toks[i].len; k++) sum = (sum + (unsigned char)toks[i].start[k]) % 9973;
            printf("%d %d %d %u\n", (int)toks[i].type, toks[i].line, toks[i].len, sum);
        }
        free(toks);
        free(src);
        return 0;
    }

    /* Everything A2 answered here is gone with the engine that answered it:
     * -c (source -> .la bytecode), -dis, -lex, -run and the bare `as script.as`
     * VM execution. A3 sources do NOT fall through to this point -- commands.c
     * routes `as FILE ARGS` whose first line declares 3.0 into the native
     * build/run driver before main() ever reaches here, and `--scope` is
     * REFUSED there by name rather than silently ignored.
     *
     * So what is left is an A2 source, or a .la, or a flag that only the VM
     * understood. Each gets the same answer, because each has the same cause. */
    if (argc >= 2) {
        fprintf(stderr,
                "as: '%s' is not an A3 program. The AetherScript 2 engine, its "
                "bytecode (.la) and its on-device compiler were removed; a "
                "source file must begin with '# aether: 3.0' and is compiled "
                "on the host by `asc build`.\n",
                argv[1]);
        return 2;
    }
    fprintf(stderr, "usage: as build|run|test|check|complete FILE [ARGS...]\n");
    return 2;
}
