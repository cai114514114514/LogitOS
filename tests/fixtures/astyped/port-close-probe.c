/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Only a private runtime copy calls this hook. Real descriptors are closed;
 * the log is opened afterwards so it cannot perturb the acquisition order. */
int probe_close(int descriptor)
{
    assert(close(descriptor) == 0);
    FILE *log = fopen(getenv("AS_PORT_CLOSE_LOG"), "a");
    assert(log);
    fprintf(log, "close %d\n", descriptor);
    assert(fclose(log) == 0);
    errno = EIO;
    return -1;
}
