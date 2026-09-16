/* SPDX-License-Identifier: MIT */
#ifndef AS_TYPED_H
#define AS_TYPED_H
#include <stdio.h>
#include <stddef.h>
typedef struct AsTypedProject AsTypedProject;

typedef struct {
    const char *path, *source;
    size_t bytes;
} AsSourceOverlay;

/* Snapshots own their source bytes. The same typed tree feeds diagnostics,
 * symbol queries and LLVM; none of these operations execute imported code. */
AsTypedProject *as_typed_check(const char *entry, const AsSourceOverlay *overlays, int count);
/* Library resolution is part of the project configuration, not process cwd.
 * Studio can pass the same root as the build driver for identical symbols. */
AsTypedProject *as_typed_check_with_library(const char *entry, const AsSourceOverlay *overlays,
                                            int count, const char *library);
AsTypedProject *as_typed_check_cli(const char *entry, const AsSourceOverlay *overlays, int count,
                                   const char *executable, const char *library);
void as_typed_free(AsTypedProject *p);
int as_typed_errors(const AsTypedProject *p);
void as_typed_report(AsTypedProject *p, FILE *out, int json);
int as_typed_llvm(AsTypedProject *p, FILE *out, int tests);
/* Guest Run has no LLVM. Evaluate the checked tree instead of refusing. */
int as_typed_eval(AsTypedProject *p);
/* Returns -1 when the command belongs to the legacy CLI. */
int as_typed_command(int argc, char **argv);
#endif
