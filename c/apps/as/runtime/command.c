/* SPDX-License-Identifier: MIT */
#include "command_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static void scan_text(void *slot)
{
    at_gc_mark((void *)((AtNativeText *)slot)->data);
}

static void scan_command(void *pointer)
{
    AtCommand *command = pointer;
    at_gc_mark(command->arguments);
    at_gc_mark(command->next);
    at_gc_mark((void *)command->input_path.data);
    at_gc_mark((void *)command->output_path.data);
}

static void scan_command_pointer(void *slot)
{
    at_gc_mark(*(AtCommand **)slot);
}

int at_command_new(AtCommand **out, const AtNativeText *arguments, int64_t count)
{
    *out = NULL;
    if (!at_caps_have(AS_CAP_PROC)) {
        return AT_E_PERMISSION;
    }
    if (count < 1 || count > AT_COMMAND_ARGUMENTS) {
        return AT_E_VALUE;
    }
    for (int64_t i = 0; i < count; i++) {
        if (arguments[i].length < 0 || arguments[i].length > AT_BUFFER_LIMIT ||
            memchr(arguments[i].data, 0, (size_t)arguments[i].length)) {
            return AT_E_VALUE;
        }
    }
    void *frame = at_gc_frame();
    AtCommand *command = at_gc_allocate(sizeof *command, scan_command);
    if (!command) {
        return AT_E_MEMORY;
    }
    command->pid = -1;
    command->status = -1;
    AtRoot root;
    at_gc_root(&root, &command, scan_command_pointer);
    command->arguments = at_list_new(sizeof(AtNativeText), scan_text);
    int status = command->arguments ? 0 : AT_E_MEMORY;
    for (int64_t i = 0; !status && i < count; i++) {
        if (!at_list_append(command->arguments, &arguments[i])) {
            status = AT_E_MEMORY;
        }
    }
    if (!status) {
        *out = command;
    }
    at_gc_restore(frame);
    return status;
}

int at_command_new_list(AtCommand **out, const AtList *arguments)
{
    return at_command_new(out, (const AtNativeText *)arguments->data, arguments->count);
}

int at_command_pipe(AtCommand *head, AtCommand *tail)
{
    if (head == tail || head->linked || tail->linked || head->started || tail->started ||
        tail->next || tail->input_path.data) {
        return AT_E_VALUE;
    }
    int count = 1;
    AtCommand *last = head;
    while (last->next) {
        if (last->next == tail) {
            return AT_E_VALUE;
        }
        last = last->next;
        count++;
    }
    if (count >= AT_COMMAND_STAGES || last->output_path.data) {
        return AT_E_VALUE;
    }
    last->next = tail;
    tail->linked = 1;
    return 0;
}

int at_command_redirect(AtCommand *head, const char *path, int64_t length, int outward)
{
    if (head->started || head->linked || length <= 0 || length > AT_CAP_PATH_LIMIT ||
        memchr(path, 0, (size_t)length)) {
        return AT_E_VALUE;
    }
    AtCommand *stage = head;
    if (outward) {
        while (stage->next) {
            stage = stage->next;
        }
    }
    AtNativeText *destination = outward ? &stage->output_path : &stage->input_path;
    if (destination->data) {
        return AT_E_VALUE;
    }
    /* The generated expression root holds the source until this assignment;
     * the command scanner then retains even an interior string view. */
    *destination = (AtNativeText){path, length};
    return 0;
}

int at_command_acquire(AtCommand *command)
{
    if (command->owned || command->linked || command->failed) {
        return AT_E_VALUE;
    }
    int status = command->started ? 0 : at_command_launch(command, -1);
    if (!status) {
        command->owned = 1;
    }
    return status;
}

int at_command_release(AtCommand *command)
{
    if (!command || !command->owned) {
        return 0;
    }
    command->owned = 0;
    return at_command_reap(command, NULL);
}

int at_command_wait(int64_t *out, AtCommand *command)
{
    *out = 0;
    if (command->linked || command->failed) {
        return AT_E_VALUE;
    }
    int status = command->started ? 0 : at_command_launch(command, -1);
    return status ? status : at_command_reap(command, out);
}

int at_command_out(AtNativeText *out, AtCommand *command)
{
    *out = (AtNativeText){0};
    if (command->started || command->linked || command->failed) {
        return AT_E_VALUE;
    }
    AtCommand *tail = command;
    while (tail->next) {
        tail = tail->next;
    }
    if (tail->output_path.data) {
        return AT_E_VALUE;
    }
    if (!at_caps_have(AS_CAP_PROC)) {
        return AT_E_PERMISSION;
    }
    int descriptors[2];
    if (pipe(descriptors) < 0) {
        return at_command_io_error();
    }
    int status = at_command_lift_descriptor(&descriptors[0]);
    if (!status) {
        status = at_command_lift_descriptor(&descriptors[1]);
    }
    if (!status) {
        status = at_command_launch(command, descriptors[1]);
    }
    if (descriptors[1] >= 0) {
        close(descriptors[1]);
    }
    unsigned char *data = NULL;
    size_t used = 0, capacity = 0;
    while (!status) {
        unsigned char chunk[4096];
        ssize_t count = read(descriptors[0], chunk, sizeof chunk);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            status = at_command_io_error();
            break;
        }
        if (!count) {
            break;
        }
        if ((size_t)count > (size_t)AT_BUFFER_LIMIT - used) {
            status = AT_E_VALUE;
            break;
        }
        size_t needed = used + (size_t)count;
        if (needed > capacity) {
            size_t next = capacity ? capacity * 2 : sizeof chunk;
            if (next < needed) {
                next = needed;
            }
            void *grown = realloc(data, next);
            if (!grown) {
                status = AT_E_MEMORY;
                break;
            }
            data = grown;
            capacity = next;
        }
        memcpy(data + used, chunk, (size_t)count);
        used = needed;
    }
    if (descriptors[0] >= 0) {
        close(descriptors[0]);
    }
    if (status) {
        /* A failed reader must not wait forever for a producer blocked on
         * output. Terminate and reap only children owned by this command. */
        at_command_abort(command);
    } else {
        status = at_command_reap(command, NULL);
    }
    if (!status) {
        AtBytes *bytes = at_bytes_copy(data, (int64_t)used);
        status = bytes ? at_bytes_decode(out, bytes) : AT_E_MEMORY;
    }
    free(data);
    return status;
}

int64_t at_command_pid(const AtCommand *command)
{
    return command->pid;
}

int64_t at_command_status(const AtCommand *command)
{
    return command->status;
}

AtList *at_command_argv(AtCommand *command)
{
    return command->arguments;
}
