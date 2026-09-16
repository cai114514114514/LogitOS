/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_COMMAND_H
#define AS_NATIVE_COMMAND_H
#include "native.h"

typedef struct AtCommand AtCommand;

enum AtCommandMethod {
    AT_COMMAND_START,
    AT_COMMAND_WAIT,
    AT_COMMAND_OUT,
    AT_COMMAND_PID,
    AT_COMMAND_STATUS,
    AT_COMMAND_ARGV
};

/* Commands are managed descriptions. OS process ownership begins only in a
 * synchronous operation or a compiler-generated Process resource scope. */
int at_command_new(AtCommand **out, const AtNativeText *arguments, int64_t count);
int at_command_new_list(AtCommand **out, const AtList *arguments);
int at_command_pipe(AtCommand *head, AtCommand *tail);
int at_command_redirect(AtCommand *head, const char *path, int64_t length, int outward);
int at_command_acquire(AtCommand *command);
int at_command_release(AtCommand *command);
int at_command_wait(int64_t *out, AtCommand *command);
int at_command_out(AtNativeText *out, AtCommand *command);
int64_t at_command_pid(const AtCommand *command);
int64_t at_command_status(const AtCommand *command);
AtList *at_command_argv(AtCommand *command);
#endif
