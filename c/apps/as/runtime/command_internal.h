/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_COMMAND_INTERNAL_H
#define AS_NATIVE_COMMAND_INTERNAL_H
#include "command.h"
#include <sys/types.h>

enum {
    AT_COMMAND_ARGUMENTS = 256,
    AT_COMMAND_STAGES = 32
};

struct AtCommand {
    AtList *arguments;
    struct AtCommand *next;
    AtNativeText input_path, output_path;
    pid_t pid;
    int status;
    int linked, started, waited, owned, failed;
};

int at_command_launch(AtCommand *head, int output_descriptor);
int at_command_reap(AtCommand *head, int64_t *last_status);
void at_command_abort(AtCommand *head);
int at_command_io_error(void);
/* Internal plumbing must never occupy a previously closed standard stream.
 * Otherwise later dup2/close operations can silently close a newly wired fd. */
int at_command_lift_descriptor(int *descriptor);
#endif
