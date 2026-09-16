/* SPDX-License-Identifier: MIT */
#include "command_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int at_command_io_error(void)
{
    return errno == EACCES || errno == EPERM ? AT_E_PERMISSION : AT_E_IO;
}

int at_command_lift_descriptor(int *descriptor)
{
    int temporary[3], count = 0;
    while (*descriptor >= 0 && *descriptor < 3) {
        temporary[count++] = *descriptor;
        *descriptor = dup(*descriptor);
    }
    int status = *descriptor < 0 ? at_command_io_error() : 0;
    while (count) {
        close(temporary[--count]);
    }
    return status;
}

static int exit_status(int status)
{
#if !__STDC_HOSTED__
    /* LogitOS waitpid currently returns the kernel's plain exit code, despite
     * sys/wait.h documenting a POSIX status word. Decoding it again turns exit
     * 7 into signal 7 and exit 127 into -1. Keep this adaptation local: changing
     * libc would also change existing kernel/test consumers of raw status. */
    return status;
#else
    return WIFEXITED(status)     ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : -1;
#endif
}

int at_command_reap(AtCommand *head, int64_t *last_status)
{
    int error = 0;
    for (AtCommand *stage = head; stage; stage = stage->next) {
        if (stage->started && !stage->waited) {
            int status;
            pid_t result;
            do {
                result = waitpid(stage->pid, &status, 0);
            } while (result < 0 && errno == EINTR);
            if (result != stage->pid) {
                if (!error) {
                    error = at_command_io_error();
                }
                stage->status = -1;
            } else {
                stage->status = exit_status(status);
            }
            stage->waited = 1;
        }
        if (last_status) {
            *last_status = stage->status;
        }
    }
    return error;
}

void at_command_abort(AtCommand *head)
{
    for (AtCommand *stage = head; stage; stage = stage->next) {
        if (stage->started && !stage->waited && stage->pid > 0) {
            kill(stage->pid, SIGKILL);
        }
    }
    at_command_reap(head, NULL);
    head->failed = 1;
}

static void free_arguments(char **arguments)
{
    if (!arguments) {
        return;
    }
    for (int i = 0; arguments[i]; i++) {
        free(arguments[i]);
    }
    free(arguments);
}

static int copy_arguments(char ***out, AtList *source)
{
    *out = NULL;
    if (source->count < 1 || source->count > AT_COMMAND_ARGUMENTS) {
        return AT_E_VALUE;
    }
    char **arguments = calloc((size_t)source->count + 1, sizeof *arguments);
    if (!arguments) {
        return AT_E_MEMORY;
    }
    int status = 0;
    for (int64_t i = 0; i < source->count; i++) {
        AtNativeText text = ((AtNativeText *)source->data)[i];
        if (text.length < 0 || text.length > AT_BUFFER_LIMIT ||
            memchr(text.data, 0, (size_t)text.length)) {
            status = AT_E_VALUE;
            break;
        }
        arguments[i] = malloc((size_t)text.length + 1);
        if (!arguments[i]) {
            status = AT_E_MEMORY;
            break;
        }
        memcpy(arguments[i], text.data, (size_t)text.length);
        arguments[i][text.length] = 0;
    }
    if (status) {
        free_arguments(arguments);
    } else {
        *out = arguments;
    }
    return status;
}

int at_command_launch(AtCommand *head, int output_descriptor)
{
    if (!at_caps_have(AS_CAP_PROC)) {
        return AT_E_PERMISSION;
    }
    if (head->linked || head->started || head->failed) {
        return AT_E_VALUE;
    }
    char **arguments[AT_COMMAND_STAGES] = {0};
    int count = 0, status = 0;
    AtCommand *tail = head;
    for (AtCommand *stage = head; stage; stage = stage->next) {
        if (count == AT_COMMAND_STAGES || stage->started) {
            status = AT_E_VALUE;
            break;
        }
        status = copy_arguments(&arguments[count], stage->arguments);
        if (status) {
            break;
        }
        count++;
        tail = stage;
    }

    /* Check both grants before opening/truncating either path, and acquire
     * redirected files in the parent using the same scoped helper as open(). */
    int input = -1, output = -1, previous = -1;
    uint32_t permissions = (head->input_path.data ? AS_CAP_FS_READ : 0) |
                           (tail->output_path.data ? AS_CAP_FS_WRITE : 0);
    if (!status && !at_caps_have(permissions)) {
        status = AT_E_PERMISSION;
    }
    if (!status && head->input_path.data) {
        status = at_file_open(&input, head->input_path.data, head->input_path.length, O_RDONLY,
                              AS_CAP_FS_READ);
        if (!status) {
            status = at_command_lift_descriptor(&input);
        }
    }
    if (!status && tail->output_path.data) {
        status = at_file_open(&output, tail->output_path.data, tail->output_path.length,
                              O_WRONLY | O_CREAT | O_TRUNC, AS_CAP_FS_WRITE);
        if (!status) {
            status = at_command_lift_descriptor(&output);
        }
    }
    /* Do not use the old hardcoded 4096 sweep: an inherited higher fd must
     * not leak into an external command. Query before fork; the child only
     * performs descriptor/signal operations and exec after argv preparation. */
    long descriptor_limit = sysconf(_SC_OPEN_MAX);
    if (!status && (descriptor_limit < 3 || descriptor_limit > INT_MAX)) {
        status = AT_E_IO;
    }
    int index = 0;
    for (AtCommand *stage = head; !status && stage; stage = stage->next, index++) {
        int ends[2] = {-1, -1};
        if (stage->next) {
            if (pipe(ends) < 0) {
                status = at_command_io_error();
            } else {
                status = at_command_lift_descriptor(&ends[0]);
                if (!status) {
                    status = at_command_lift_descriptor(&ends[1]);
                }
            }
        }
        pid_t child = status ? -1 : fork();
        if (child < 0 && !status) {
            status = at_command_io_error();
        }
        if (child == 0) {
            int read_end = stage == head ? input : previous;
            int write_end = stage->next              ? ends[1]
                            : output_descriptor >= 0 ? output_descriptor
                                                     : output;
            if ((read_end >= 0 && dup2(read_end, STDIN_FILENO) < 0) ||
                (write_end >= 0 && dup2(write_end, STDOUT_FILENO) < 0) ||
                signal(SIGPIPE, SIG_DFL) == SIG_ERR) {
                _exit(127);
            }
            for (int descriptor = 3; descriptor < descriptor_limit; descriptor++) {
                close(descriptor);
            }
            execvp(arguments[index][0], arguments[index]);
            _exit(127);
        }
        if (ends[1] >= 0) {
            close(ends[1]);
        }
        if (previous >= 0) {
            close(previous);
        }
        previous = ends[0];
        if (!status) {
            stage->pid = child;
            stage->started = 1;
        }
    }
    if (previous >= 0) {
        close(previous);
    }
    if (input >= 0) {
        close(input);
    }
    if (output >= 0) {
        close(output);
    }
    for (int i = 0; i < count; i++) {
        free_arguments(arguments[i]);
    }
    if (status) {
        at_command_abort(head);
    }
    return status;
}
