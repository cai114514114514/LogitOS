/* SPDX-License-Identifier: MIT */
#include "runner.h"
#include <stdio.h>

void st_runner_init(StRunner *r)
{
    memset(r, 0, sizeof *r);
    r->pid = -1;
    r->input = r->output = -1;
    r->tab = -1;
}

int st_runner_busy(const StRunner *r)
{
    return r->pid > 0 && !r->finished;
}

void st_runner_stop(StRunner *r)
{
    if (r->pid > 0 && !r->finished) {
        kill(r->pid, SIGKILL);
        r->cancelled = 1;
    }
    if (r->input >= 0) {
        close(r->input);
        r->input = -1;
    }
}

void st_runner_dispose(StRunner *r)
{
    st_runner_stop(r);
    if (r->output >= 0) {
        close(r->output);
    }
    st_runner_release_sources(r);
    free(r->text);
    st_runner_init(r);
}

int st_runner_start(StRunner *r, const StDocument *docs, int count, int tab, int check,
                    const char *compiler, const char *root)
{
    if (st_runner_busy(r)) {
        return -1;
    }
    st_runner_dispose(r);
    if (st_runner_capture(r, docs, count, tab) < 0) {
        return -1;
    }
    const StDocument *d = &docs[tab];
    r->caret = d->caret;
    r->text = malloc(ST_OUTPUT_LIMIT + 1);
    if (!r->text) {
        st_runner_dispose(r);
        return -1;
    }
    r->text[0] = 0;
    r->tab = tab;
    r->mode = check;
    int in[2], out[2];
    if (pipe(in) < 0) {
        st_runner_dispose(r);
        return -1;
    }
    if (pipe(out) < 0) {
        close(in[0]);
        close(in[1]);
        st_runner_dispose(r);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        st_runner_dispose(r);
        return -1;
    }
    if (!pid) {
        if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 || dup2(out[1], 2) < 0) {
            _exit(126);
        }
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        if (chdir(root) < 0) {
            _exit(126);
        }
        /* The native driver locates its runtime/library beside argv[0]. A
         * display label of "as" loses that installation after chdir(project),
         * so an actual Studio run looked for aether-toolchain in the project. */
        char *check_args[] = {(char *)compiler,           (char *)"check", (char *)"--json",
                              (char *)"--snapshot-stdin", (char *)d->path, NULL};
        char *run_args[] = {(char *)compiler, (char *)d->path, NULL};
        char position[24];
        snprintf(position, sizeof position, "%d", d->caret);
        char *complete_args[] = {(char *)compiler,
                                 (char *)"complete",
                                 (char *)d->path,
                                 (char *)"--snapshot-stdin",
                                 (char *)"--at",
                                 position,
                                 NULL};
        execve(compiler,
               check == ST_COMPLETE_PROJECT ? complete_args
               : check                      ? check_args
                                            : run_args,
               NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    r->pid = pid;
    r->input = in[1];
    r->output = out[0];
    if (fcntl(r->input, F_SETFL, O_NONBLOCK) < 0 || fcntl(r->output, F_SETFL, O_NONBLOCK) < 0) {
        st_runner_stop(r);
        r->io_error = 1;
    }
    if (!check && r->input >= 0) {
        close(r->input);
        r->input = -1;
    }
    return 0;
}

int st_runner_poll(StRunner *r)
{
    if (r->pid < 0 || r->finished) {
        return 0;
    }
    int changed = 0;
    if (r->input >= 0) {
        int remaining = r->payload_length - r->sent;
        if (remaining) {
            ssize_t n = write(r->input, r->payload + r->sent,
                              (size_t)(remaining > 4096 ? 4096 : remaining));
            if (n > 0) {
                r->sent += (int)n;
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                r->io_error = 1;
                close(r->input);
                r->input = -1;
            }
        }
        if (r->input >= 0 && r->sent == r->payload_length) {
            close(r->input);
            r->input = -1;
        }
    }
    /* Limit work per UI turn. A program continuously writing stdout must not
     * monopolize the event loop and make its own Stop button unreachable. */
    for (int i = 0; r->output >= 0 && i < 16; i++) {
        char buf[4096];
        ssize_t n = read(r->output, buf, sizeof buf);
        if (n > 0) {
            int keep = (int)n;
            if (keep > ST_OUTPUT_LIMIT - r->length) {
                keep = ST_OUTPUT_LIMIT - r->length;
                r->truncated = 1;
            }
            memcpy(r->text + r->length, buf, (size_t)keep);
            r->length += keep;
            r->text[r->length] = 0;
            changed = 1;
        } else if (!n) {
            close(r->output);
            r->output = -1;
            break;
        } else {
            if (errno != EAGAIN && errno != EINTR) {
                r->io_error = 1;
                close(r->output);
                r->output = -1;
            }
            break;
        }
    }
    int status = 0;
    pid_t p = waitpid(r->pid, &status, WNOHANG);
    if (p == r->pid) {
#if !__STDC_HOSTED__
        /* LogitOS libc currently returns the kernel's plain exit code, despite
         * sys/wait.h exposing POSIX macros. Normalize only at this boundary:
         * changing libc would break existing callers that consume raw codes.
         * Without this, compiler exit 1 looks like signal 1 and every real
         * guest error report is rejected. Host waitpid already encodes it. */
        status = (status & 0xff) << 8;
#endif
        r->status = status;
        r->finished = 1;
        changed = 1;
        /* EOF is not an exit status. Reap the actual child even if it closed
         * stdout early, and never make a live child's blocking wait a GUI call. */
        if (r->input >= 0) {
            close(r->input);
            r->input = -1;
        }
        if (r->output >= 0) {
            for (int i = 0; i < 32; i++) {
                char b[4096];
                ssize_t n = read(r->output, b, sizeof b);
                if (n <= 0) {
                    break;
                }
                int k = (int)n;
                if (k > ST_OUTPUT_LIMIT - r->length) {
                    k = ST_OUTPUT_LIMIT - r->length;
                    r->truncated = 1;
                }
                memcpy(r->text + r->length, b, (size_t)k);
                r->length += k;
                r->text[r->length] = 0;
                if (i == 31) {
                    r->truncated = 1;
                }
            }
            close(r->output);
            r->output = -1;
        }
    } else if (p < 0 && errno != EINTR) {
        r->finished = 1;
        r->io_error = 1;
        changed = 1;
    }
    return changed;
}

int st_runner_current(const StRunner *r, const StDocument *d, int tab)
{
    /* A checksum alone cannot authorize attaching results. Match the tab's
     * generation and the complete captured bytes before using any offset. */
    return r->tab == tab && r->revision == d->revision && r->source_length == d->length &&
           r->source && !memcmp(r->source, d->text, (size_t)d->length);
}
