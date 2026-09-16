/* SPDX-License-Identifier: MIT */
#include "ir/model.h"
#include "common/numeric.h"
#include "include/snapshot.h"
#include "include/completion.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/* The same manifest drives cross compilation and the runtime test harness.
 * A newly split module cannot accidentally disappear from just one link path. */
static const char *runtime_sources[] = {
#define AT_RUNTIME_SOURCE(name) #name ".c",
#include "runtime/sources.def"
#undef AT_RUNTIME_SOURCE
};

enum {
    NATIVE_RUNTIME_UNITS = sizeof runtime_sources / sizeof runtime_sources[0]
};

/* The CLI owns process creation and artifact publication. Parsing/type
 * checking/lowering are separate APIs so Studio can use them without starting
 * a compiler subprocess or changing files in the user's project. */
static int execute_program(const char *program, char *const args[], const char *capture)
{
    pid_t child = fork();
    if (child < 0) {
        return 126;
    }
    if (!child) {
        if (capture) {
            int fd = open(capture, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0) {
                _exit(126);
            }
            close(fd);
        }
        execvp(program, args);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return 126;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 126;
}

static int execute(char *const args[], const char *capture)
{
    return execute_program(args[0], args, capture);
}

static char *read_text(const char *path, size_t *bytes, int *truncated)
{
    *bytes = 0;
    *truncated = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *s = malloc(AT_SOURCE_MAX + 2);
    if (!s) {
        fclose(f);
        return NULL;
    }
    /* Read a sentinel byte so a full buffer cannot be mistaken for a complete
     * file. Source overlays reject truncation; output reports it explicitly. */
    size_t n = fread(s, 1, AT_SOURCE_MAX + 1, f);
    if (ferror(f)) {
        free(s);
        fclose(f);
        return NULL;
    }
    if (n > AT_SOURCE_MAX) {
        *truncated = 1;
        n = AT_SOURCE_MAX;
    }
    *bytes = n;
    s[n] = 0;
    fclose(f);
    return s;
}

static int option_error(const char *s)
{
    fprintf(stderr, "as: %s\n", s);
    return 2;
}

static int release_option_error(char *owned[AT_MODULES], int count, const char *message)
{
    for (int i = 0; i < count; i++) {
        free(owned[i]);
    }
    return option_error(message);
}

/* A failed build must not truncate the user's previous executable. Stage the
 * finished binary beside its destination, fsync it, then rename. The sibling
 * location also avoids a cross-filesystem rename from the private /tmp build.
 * A unique sibling avoids collisions with simultaneous builds. The directory
 * is flushed after rename as well, so its new entry reaches stable storage. */
static int publish_artifact(const char *binary_path, const char *output_path, mode_t mode)
{
    char temporary_path[1024];
    if (snprintf(temporary_path, sizeof temporary_path, "%s.tmp.XXXXXX", output_path) >=
        (int)sizeof temporary_path) {
        return 1;
    }

    FILE *input = fopen(binary_path, "rb");
    if (!input) {
        return 1;
    }
    int descriptor = mkstemp(temporary_path);
    if (descriptor < 0) {
        fclose(input);
        return 1;
    }
    FILE *output = fdopen(descriptor, "wb");
    if (!output) {
        close(descriptor);
    }
    int failed = !input || !output;

    if (!failed) {
        char buffer[8192];
        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, sizeof buffer, input))) {
            if (fwrite(buffer, 1, bytes_read, output) != bytes_read) {
                failed = 1;
                break;
            }
        }
        if (ferror(input) || fchmod(fileno(output), mode) || fflush(output) ||
            fsync(fileno(output))) {
            failed = 1;
        }
    }

    if (input) {
        fclose(input);
    }
    if (output && fclose(output)) {
        failed = 1;
    }
    if (!failed && rename(temporary_path, output_path)) {
        failed = 1;
    }
    if (!failed) {
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", output_path);
        char *slash = strrchr(parent, '/');
        if (slash) {
            slash[slash == parent ? 1 : 0] = 0;
        } else {
            strcpy(parent, ".");
        }
        int directory = open(parent, O_RDONLY);
        if (directory < 0) {
            failed = 1;
        } else {
            failed = fsync(directory) != 0;
            close(directory);
        }
    }
    if (failed) {
        unlink(temporary_path);
    }
    return failed;
}

static int emit_llvm_output(AsTypedProject *project, const char *output_path, int tests, int json)
{
    char temporary[] = "/tmp/aether-ir-XXXXXX";
    int descriptor = -1;
    FILE *output = stdout;
    if (output_path) {
        descriptor = mkstemp(temporary);
        output = descriptor < 0 ? NULL : fdopen(descriptor, "wb");
        if (!output && descriptor >= 0) {
            close(descriptor);
            unlink(temporary);
        }
    }
    if (!output) {
        return option_error("Cannot open LLVM output");
    }

    int result = as_typed_llvm(project, output, tests);
    if (output_path && fclose(output)) {
        result = -1;
    }
    if (output_path) {
        if (result >= 0 && publish_artifact(temporary, output_path, 0644)) {
            Token site = {.start = project->modules[0].source, .line = 1};
            at_error(project, 0, site, "AS3502", "Could not publish LLVM output");
            result = -1;
        }
        unlink(temporary);
    }
    if (result < 0) {
        as_typed_report(project, stderr, json);
    } else if (output_path && json) {
        /* With no output path, stdout is the IR stream. Appending JSON there
         * would make otherwise valid LLVM unreadable by the next tool. */
        as_typed_report(project, stdout, 1);
    }
    return result < 0 ? 1 : 0;
}

static void report_toolchain_failure(int exit_code, const char *log_path, int json)
{
    size_t bytes;
    int truncated;
    char *output = read_text(log_path, &bytes, &truncated);
    if (json) {
        fprintf(stdout, "{\"ok\":false,\"phase\":\"link\",\"exit_code\":%d,\"output\":", exit_code);
        if (output) {
            at_quote_bytes(stdout, output, bytes);
        } else {
            at_quote(stdout, "Toolchain failed");
        }
        fprintf(stdout, ",\"output_truncated\":%s}\n", truncated ? "true" : "false");
    } else {
        fprintf(stderr, "%s", output ? output : "Native toolchain failed\n");
    }
    free(output);
}

/* Both test and run results carry the same immutable source snapshot used to
 * build the program. The program's exit status stays separate from diagnostics
 * emitted while compiling it. */
static void report_execution(AsTypedProject *project, const char *log_path, int exit_code,
                             int tests)
{
    size_t bytes;
    int truncated;
    char *output = read_text(log_path, &bytes, &truncated);
    fprintf(stdout, "{\"ok\":%s,\"phase\":\"%s\",\"exit_code\":%d,\"output\":",
            exit_code ? "false" : "true", tests ? "test" : "run", exit_code);
    at_quote_bytes(stdout, output ? output : "", output ? bytes : 0);
    fprintf(stdout, ",\"output_truncated\":%s,\"snapshot\":", truncated ? "true" : "false");
    as_typed_report(project, stdout, 1);
    fputs("}\n", stdout);
    free(output);
}

AsTypedProject *as_typed_check_cli(const char *entry, const AsSourceOverlay *overlays, int count,
                                   const char *executable, const char *library)
{
    char default_library[1024];
    if (!library) {
        library = getenv("AETHER_STDLIB");
    }
    if (!library) {
        snprintf(default_library, sizeof default_library, "%s", executable);
        char *slash = strrchr(default_library, '/');
        if (slash) {
            slash[1] = 0;
        } else {
            default_library[0] = 0;
        }
        strncat(default_library, "aether-toolchain/lib",
                sizeof default_library - strlen(default_library) - 1);
        library = default_library;
#if !__STDC_HOSTED__
        library = "/usr/as/lib";
#endif
    }
    return as_typed_check_with_library(entry, overlays, count, library);
}

int as_typed_command(int argc, char **argv)
{
    if (argc < 2) {
        return -1;
    }
    int build = !strcmp(argv[1], "build"), run = !strcmp(argv[1], "run"),
        tests = !strcmp(argv[1], "test"), check = !strcmp(argv[1], "check"),
        complete = !strcmp(argv[1], "complete");
    if (!build && !run && !tests && !check && !complete) {
        if (argv[1][0] == '-') {
            return -1;
        }
        size_t bytes;
        int truncated;
        char *source = read_text(argv[1], &bytes, &truncated);
        int native = source && as_source_version(source) == AS_LANGUAGE_NATIVE;
        free(source);
        if (!native) {
            return -1;
        }
        /* Existing launchers use `as FILE ARGS`, including Studio. Route an
         * explicitly native source through exactly the same build/run driver;
         * sending it to the retiring VM cannot execute A3. Everything after
         * FILE stays a program argument, never a compiler flag. */
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--scope")) {
                fprintf(stderr, "as: native shorthand cannot apply --scope; refusing to run\n");
                return 2;
            }
        }
        char **forward = calloc((size_t)argc + 3, sizeof *forward);
        if (!forward) {
            fprintf(stderr, "as: cannot allocate native command arguments\n");
            return 1;
        }
        forward[0] = argv[0];
        forward[1] = "run";
        forward[2] = argv[1];
        forward[3] = "--";
        for (int i = 2; i < argc; i++) {
            forward[i + 2] = argv[i];
        }
        int status = as_typed_command(argc + 2, forward);
        free(forward);
        return status;
    }
    /* The established --stdin checker retains ownership of stdin, including its
     * legacy syntax mode. Its version-3 branch calls the shared project API. */
    if (check) {
        int overlays = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--overlay") || !strcmp(argv[i], "--stdlib") ||
                !strcmp(argv[i], "--snapshot-stdin")) {
                overlays = 1;
            }
        }
        if (!overlays) {
            return -1;
        }
    }
    const char *entry = NULL, *output = NULL, *toolchain = getenv("AETHER_TOOLCHAIN"),
               *target = "host", *library = getenv("AETHER_STDLIB");
    int json = 0, llvm = 0, optimize = 1, program_arguments = argc, snapshot_stdin = 0;
    size_t caret = 0;
    int have_caret = 0;
    AsSourceOverlay overlays[AT_MODULES];
    int count = 0;
    char *owned[AT_MODULES] = {0};
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            if ((!run && !tests) || !entry) {
                return release_option_error(owned, count,
                                            "Program arguments require run|test FILE -- ARGS");
            }
            program_arguments = i + 1;
            break;
        } else if (!strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!strcmp(argv[i], "--at") && complete && i + 1 < argc) {
            const char *number = argv[++i];
            if (!*number || have_caret) {
                return release_option_error(owned, count, "Completion requires one byte offset");
            }
            for (; *number; number++) {
                if (*number < '0' || *number > '9' || caret > AT_SOURCE_MAX / 10) {
                    return release_option_error(owned, count, "Invalid completion byte offset");
                }
                caret = caret * 10 + (unsigned)(*number - '0');
            }
            have_caret = 1;
        } else if (!strcmp(argv[i], "--snapshot-stdin")) {
            snapshot_stdin = 1;
        } else if (!strcmp(argv[i], "--emit-llvm")) {
            llvm = 1;
        } else if (!strcmp(argv[i], "--debug")) {
            optimize = 0;
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            output = argv[++i];
        } else if (!strcmp(argv[i], "--target") && i + 1 < argc) {
            target = argv[++i];
        } else if (!strcmp(argv[i], "--toolchain") && i + 1 < argc) {
            toolchain = argv[++i];
        } else if (!strcmp(argv[i], "--stdlib") && i + 1 < argc) {
            library = argv[++i];
        } else if (!strcmp(argv[i], "--overlay") && i + 2 < argc && count < AT_MODULES) {
            const char *path = argv[++i], *input = argv[++i];
            size_t bytes;
            int truncated;
            owned[count] = read_text(input, &bytes, &truncated);
            if (!owned[count]) {
                return release_option_error(owned, count, "Cannot read overlay");
            }
            if (truncated || memchr(owned[count], 0, bytes)) {
                return release_option_error(owned, count + 1,
                                            "Overlay exceeds 1 MiB or contains NUL bytes");
            }
            overlays[count] = (AsSourceOverlay){path, owned[count], bytes};
            count++;
        } else if (argv[i][0] == '-' || entry) {
            return release_option_error(
                owned, count,
                "usage: as check|build|run|test|complete FILE [--at BYTE_OFFSET] [--json] "
                "[--debug] [--emit-llvm] [-o OUTPUT] "
                "[--target host|logitos-x86_64] [--toolchain DIR] [--stdlib DIR] "
                "[--overlay PATH CONTENT_FILE | --snapshot-stdin] [-- PROGRAM_ARGUMENTS]");
        } else {
            entry = argv[i];
        }
    }
    if (!entry) {
        return release_option_error(owned, count, "A source entry path is required");
    }
    if (strcmp(target, "host") && strcmp(target, "logitos-x86_64")) {
        return release_option_error(owned, count, "Unsupported native target");
    }
    AsSourceSnapshot snapshot = {0};
    if (snapshot_stdin) {
        if (count) {
            return release_option_error(owned, count, "Cannot mix --overlay and --snapshot-stdin");
        }
        const char *error = as_snapshot_read(stdin, &snapshot);
        if (error) {
            as_snapshot_free(&snapshot);
            return release_option_error(owned, count, error);
        }
    }
    AsTypedProject *p =
        as_typed_check_cli(entry, snapshot_stdin ? snapshot.sources : overlays,
                           snapshot_stdin ? snapshot.count : count, argv[0], library);
    as_snapshot_free(&snapshot);
    for (int i = 0; i < count; i++) {
        free(owned[i]);
    }
    if (complete) {
        int result = have_caret ? as_typed_complete_report(p, caret, stdout) : -1;
        as_typed_free(p);
        return result < 0 ? option_error("Completion offset is missing or outside the source") : 0;
    }
    if (check || as_typed_errors(p)) {
        as_typed_report(p, stdout, json);
        int result = as_typed_errors(p) ? 1 : 0;
        as_typed_free(p);
        return result;
    }
#if !__STDC_HOSTED__
    /* Studio Run on the machine has no clang/LLVM. The A2 VM used to execute
     * the source here; deleting it made every Run print AS3501, including
     * print("hello world"). Evaluate the checked tree instead. Host builds
     * still go through LLVM so guest and host stay the same language. */
    if (!llvm) {
        int status = as_typed_eval(p);
        as_typed_free(p);
        return status;
    }
#endif
    if (llvm) {
        int result = emit_llvm_output(p, output, tests, json);
        as_typed_free(p);
        return result;
    }
    char default_toolchain[1024];
    if (!toolchain) {
        snprintf(default_toolchain, sizeof default_toolchain, "%s", argv[0]);
        char *slash = strrchr(default_toolchain, '/');
        if (slash) {
            slash[1] = 0;
        } else {
            default_toolchain[0] = 0;
        }
        strncat(default_toolchain, "aether-toolchain",
                sizeof default_toolchain - strlen(default_toolchain) - 1);
        toolchain = default_toolchain;
    }
    char scratch[] = "/tmp/aether-native-XXXXXX";
    if (!mkdtemp(scratch)) {
        as_typed_free(p);
        return option_error("Cannot create private build directory");
    }
    char ir[1024], object[1024], binary[1024], log[1024], crt[1024], libc[1024], rtobject[1024],
        package[1024], packager[1024];
    char runtime[NATIVE_RUNTIME_UNITS][1024];
    snprintf(ir, sizeof ir, "%s/module.ll", scratch);
    snprintf(object, sizeof object, "%s/module.o", scratch);
    snprintf(binary, sizeof binary, "%s/program", scratch);
    snprintf(package, sizeof package, "%s/program.aex", scratch);
    snprintf(log, sizeof log, "%s/output.txt", scratch);
    for (int i = 0; i < NATIVE_RUNTIME_UNITS; i++) {
        int length =
            snprintf(runtime[i], sizeof runtime[i], "%s/%s", toolchain, runtime_sources[i]);
        if (length < 0 || length >= (int)sizeof runtime[i]) {
            as_typed_free(p);
            rmdir(scratch);
            return option_error("Runtime source path is too long");
        }
    }
    snprintf(rtobject, sizeof rtobject, "%s/native.a", toolchain);
    snprintf(crt, sizeof crt, "%s/crt0.o", toolchain);
    snprintf(libc, sizeof libc, "%s/libc.a", toolchain);
    snprintf(packager, sizeof packager, "%s/mkaex.py", toolchain);
    int rc = 1;
    FILE *f = fopen(ir, "wb");
    if (!f) {
        goto done;
    }
    int emitted = as_typed_llvm(p, f, tests);
    if (fclose(f) || emitted < 0) {
        as_typed_report(p, stdout, json);
        goto done;
    }
    const char *clang = getenv("AETHER_CLANG");
    if (!clang) {
        clang = "clang";
    }
    const char *ld = getenv("AETHER_LD");
    if (!ld) {
        ld = "ld.lld";
    }
    if (!strcmp(target, "host")) {
        char *args[NATIVE_RUNTIME_UNITS + 8];
        int count = 0;
        args[count++] = (char *)clang;
        args[count++] = optimize ? "-O2" : "-O0";
        args[count++] = "-g";
        args[count++] = ir;
        for (int i = 0; i < NATIVE_RUNTIME_UNITS; i++) {
            args[count++] = runtime[i];
        }
        args[count++] = "-o";
        args[count++] = binary;
        args[count] = NULL;
        rc = execute(args, log);
    } else {
        char *args[] = {(char *)clang,
                        "--target=x86_64-elf",
                        "-c",
                        optimize ? "-O2" : "-O0",
                        "-mno-red-zone",
                        "-fno-pic",
                        "-fno-pie",
                        ir,
                        "-o",
                        object,
                        NULL};
        rc = execute(args, log);
        if (!rc) {
            char *link[] = {(char *)ld, "-nostdlib", "-e",     "_start", "-Ttext=0x50000000",
                            crt,        object,      rtobject, libc,     "-o",
                            binary,     NULL};
            rc = execute(link, log);
        }
        if (!rc) {
            /* Use the OS format's authoritative packager. The generated
             * program has no Python dependency; this is a host build tool,
             * just like LLVM. Duplicating AEX headers here would drift from
             * kernel metadata, checksums and page-alignment requirements. */
            char *pack[] = {"python3", packager, binary, package, "aether-program", "--cli", NULL};
            rc = execute(pack, log);
        }
    }
    if (rc) {
        report_toolchain_failure(rc, log, json);
        goto done;
    }
    if (run || tests) {
        if (strcmp(target, "host")) {
            rc = option_error("Cross-compiled programs must run in a LogitOS guest");
            goto done;
        }
        int argument_count = argc - program_arguments;
        char **args = calloc((size_t)argument_count + 2, sizeof(*args));
        if (!args) {
            rc = option_error("Cannot allocate program arguments");
            goto done;
        }
        /* The temporary host executable is an implementation detail. args()[0]
         * remains the source entry for `as run`, matching direct script launch;
         * a separately built executable receives its ordinary process argv[0]. */
        args[0] = (char *)entry;
        for (int i = 0; i < argument_count; i++) {
            args[i + 1] = argv[program_arguments + i];
        }
        rc = execute_program(binary, args, json ? log : NULL);
        free(args);
        if (json) {
            report_execution(p, log, rc, tests);
        }
    } else {
        if (!output) {
            output = !strcmp(target, "host") ? "a.out" : "a.out.aex";
        }
        const char *artifact = !strcmp(target, "host") ? binary : package;
        rc = publish_artifact(artifact, output, 0755);
        if (json) {
            fprintf(stdout, "{\"ok\":%s,\"phase\":\"build\",\"artifact\":", rc ? "false" : "true");
            at_quote(stdout, output);
            fputs(",\"snapshot\":", stdout);
            as_typed_report(p, stdout, 1);
            fputs("}\n", stdout);
        } else if (!rc) {
            printf("Built %s (%s)\n", output, target);
        }
    }
done:
    unlink(ir);
    unlink(object);
    unlink(binary);
    unlink(package);
    unlink(log);
    rmdir(scratch);
    as_typed_free(p);
    return rc;
}
