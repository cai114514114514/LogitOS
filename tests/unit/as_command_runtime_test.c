/* SPDX-License-Identifier: MIT */
/* Real host children with failures injected only at the command OS boundary.
 * A failed assertion must not strand children: verify_children() records the
 * failure, cleans up only the PIDs we launched, and then asserts the result. */
#include "../../c/apps/as/runtime/command.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t children[8];
static int child_count, fork_calls, fail_fork, fail_read, fail_grow;
static int pipe_descriptors[16], pipe_count;
static const char *executable;
static AtCommand *retained[16];
static AtRoot roots[16];
static int root_count;

pid_t command_test_fork(void)
{
    if (++fork_calls == fail_fork) {
        errno = EAGAIN;
        return -1;
    }
    pid_t child = fork();
    if (child > 0) {
        assert(child_count < 8);
        children[child_count++] = child;
    }
    return child;
}

int command_test_pipe(int descriptors[2])
{
    int result = pipe(descriptors);
    if (!result) {
        assert(pipe_count + 2 <= 16);
        pipe_descriptors[pipe_count++] = descriptors[0];
        pipe_descriptors[pipe_count++] = descriptors[1];
    }
    return result;
}

ssize_t command_test_read(int descriptor, void *buffer, size_t size)
{
    if (fail_read) {
        errno = EIO;
        return -1;
    }
    return read(descriptor, buffer, size);
}

void *command_test_realloc(void *pointer, size_t size)
{
    if (fail_grow) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(pointer, size);
}

static void scan_command(void *slot)
{
    at_gc_mark(*(AtCommand **)slot);
}

static AtCommand *command(const char *mode, const char *argument)
{
    AtNativeText values[] = {
        {executable, (int64_t)strlen(executable)},
        {mode, (int64_t)strlen(mode)},
        {argument, (int64_t)strlen(argument)},
    };
    assert(root_count < 16);
    AtCommand **slot = &retained[root_count];
    assert(at_command_new(slot, values, 3) == 0);
    at_gc_root(&roots[root_count++], slot, scan_command);
    return *slot;
}

static void verify_children(void)
{
    int all_reaped = 1;
    for (int i = 0; i < child_count; i++) {
        int status;
        pid_t result = waitpid(children[i], &status, WNOHANG);
        if (result != -1 || errno != ECHILD) {
            all_reaped = 0;
            if (!result) {
                kill(children[i], SIGKILL);
                while (waitpid(children[i], &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
    }
    assert(all_reaped && "command left an unreaped child");
    for (int i = 0; i < pipe_count; i++) {
        assert(fcntl(pipe_descriptors[i], F_GETFD) == -1 && errno == EBADF);
    }
    child_count = fork_calls = pipe_count = 0;
}

static int child_main(const char *mode, const char *argument)
{
    if (!strcmp(mode, "hold")) {
        for (;;) {
            pause();
        }
    }
    if (!strcmp(mode, "emit")) {
        char bytes[4096];
        memset(bytes, 'x', sizeof bytes);
        for (int i = 0; i < 64; i++) {
            if (write(1, bytes, sizeof bytes) != sizeof bytes) {
                return 80;
            }
        }
        return 9;
    }
    if (!strcmp(mode, "text")) {
        return write(1, "wired\n", 6) == 6 ? 0 : 81;
    }
    if (!strcmp(mode, "descriptor")) {
        return fcntl(atoi(argument), F_GETFD) == -1 && errno == EBADF ? 0 : 82;
    }
    if (!strcmp(mode, "sigpipe")) {
        raise(SIGPIPE);
        return 83; /* Reached only if the parent's SIG_IGN leaked through exec. */
    }
    return 84;
}

int main(int argc, char **argv)
{
    /* Exec children do not initialize the A3 runtime: this tests the signal
     * disposition inherited by an ordinary external program. */
    if (argc == 3) {
        return child_main(argv[1], argv[2]);
    }
    executable = argv[0];
    at_process_init(argc, argv);
    at_caps_init();
    void *frame = at_gc_frame();
    int64_t status;
    AtNativeText output;

    AtCommand *pipeline = command("hold", "");
    assert(at_command_pipe(pipeline, command("hold", "")) == 0);
    fail_fork = 2;
    assert(at_command_wait(&status, pipeline) == AT_E_IO);
    assert(child_count == 1 && fork_calls == 2);
    verify_children();
    fail_fork = 0;

    fail_read = 1;
    assert(at_command_out(&output, command("hold", "")) == AT_E_IO);
    assert(child_count == 1);
    verify_children();
    fail_read = 0;

    fail_grow = 1;
    assert(at_command_out(&output, command("emit", "")) == AT_E_MEMORY);
    assert(child_count == 1);
    verify_children();
    fail_grow = 0;

    assert(at_command_wait(&status, command("sigpipe", "")) == 0);
    assert(status == 128 + SIGPIPE);
    verify_children();

    /* Save stderr too: pipe/dup can occupy any missing standard descriptor.
     * Restore all three before asserting so diagnostics stay observable. */
    int saved[3];
    for (int i = 0; i < 3; i++) {
        saved[i] = dup(i);
        assert(saved[i] >= 3);
    }
    for (int i = 0; i < 3; i++) {
        close(i);
    }
    int result = at_command_out(&output, command("text", ""));
    for (int i = 0; i < 3; i++) {
        assert(dup2(saved[i], i) == i);
        close(saved[i]);
    }
    assert(result == 0 && output.length == 6 && !memcmp(output.data, "wired\n", 6));
    /* The pipe's original low numbers now belong to restored stdio. Child
     * reaping is still checked; descriptor closure is covered above. */
    pipe_count = 0;
    verify_children();

    int high = fcntl(STDOUT_FILENO, F_DUPFD, 4096);
    if (high < 0) {
        puts("SKIP high inherited fd: host limit does not allow fd 4096");
    } else {
        char number[32];
        snprintf(number, sizeof number, "%d", high);
        assert(at_command_wait(&status, command("descriptor", number)) == 0);
        close(high);
        assert(status == 0);
        verify_children();
    }

    at_gc_restore(frame);
    at_gc_collect();
    puts("native command lifecycle ok");
    return 0;
}
