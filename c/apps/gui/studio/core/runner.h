/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_RUNNER_H
#define STUDIO_RUNNER_H
#include "document.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define ST_OUTPUT_LIMIT (128 * 1024)
#define ST_RUNNER_SOURCES 8

enum {
    ST_RUN_PROGRAM,
    ST_CHECK_PROJECT,
    ST_COMPLETE_PROJECT
};

typedef struct {
    char path[128];
    char *text;
    int length;
    uint64_t revision;
} StCapturedSource;

typedef struct {
    pid_t pid;
    int input, output, mode, finished, status, cancelled, truncated, io_error, sent;
    int source_length, length, tab;
    int caret;
    uint64_t revision;
    char *source, *text;
    /* source borrows the active entry below. These owned copies survive until
     * the next job, so diagnostics can be checked against the actual bytes. */
    StCapturedSource sources[ST_RUNNER_SOURCES];
    int source_count;
    char *payload;
    int payload_length;
} StRunner;

void st_runner_init(StRunner *r);
int st_runner_busy(const StRunner *r);
void st_runner_stop(StRunner *r);
void st_runner_dispose(StRunner *r);
int st_runner_start(StRunner *r, const StDocument *docs, int count, int tab, int check,
                    const char *compiler, const char *root);
int st_runner_poll(StRunner *r);
int st_runner_current(const StRunner *r, const StDocument *d, int tab);
int st_runner_capture(StRunner *r, const StDocument *docs, int count, int tab);
void st_runner_release_sources(StRunner *r);
int st_runner_imports_current(const StRunner *r, const StDocument *docs, int count);
const StCapturedSource *st_runner_source(const StRunner *r, const char *path);
#endif
