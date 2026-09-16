/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int launch_count;
static char **launch_values;

void at_process_init(int argc, char **argv)
{
    /* Native I/O reports a broken pipe as IOError. The OS default would kill
     * the program before write returns EPIPE, bypassing every with cleanup.
     * Future process launchers must reset SIGPIPE to SIG_DFL in exec children,
     * so external commands keep their own conventional pipeline behavior. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fputs("AetherScript: cannot initialize broken-pipe exception handling\n", stderr);
        exit(1);
    }
    launch_count = argc;
    launch_values = argv;
}

static void scan_list_pointer(void *slot)
{
    at_gc_mark(*(AtList **)slot);
}

static void scan_text(void *slot)
{
    at_gc_mark((void *)((AtNativeText *)slot)->data);
}

AtList *at_process_args(void)
{
    void *frame = at_gc_frame();
    AtList *list = at_list_new(sizeof(AtNativeText), scan_text);
    if (!list) {
        return NULL;
    }
    AtRoot list_root, text_root;
    AtNativeText text = {0};
    at_gc_root(&list_root, &list, scan_list_pointer);
    at_gc_root(&text_root, &text, scan_text);

    for (int index = 0; index < launch_count; index++) {
        const char *argument = launch_values[index];
        size_t length = strlen(argument);
        /* The newly copied text must be rooted while append grows the list.
         * The list scanner cannot reach this text until append has completed. */
        if (length >= INT64_MAX || !at_text_concat(&text, argument, (int64_t)length, "", 0) ||
            !at_list_append(list, &text)) {
            at_gc_restore(frame);
            return NULL;
        }
    }
    at_gc_restore(frame);
    return list;
}
