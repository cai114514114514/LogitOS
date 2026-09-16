/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_path(const char *path)
{
    AtBytes *result = NULL;
    int status = at_file_read(&result, path, (int64_t)strlen(path));
    if (status) {
        assert(result == NULL);
    } else {
        assert(result->length == 3 && !memcmp(result->data, "yes", 3));
    }
    return status;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[4096];
    at_caps_set(AS_CAP_FS_READ | AS_CAP_FS_WRITE, argv[1]);
    assert(chdir(argv[1]) == 0);
    assert(read_path("child/data") == 0);
    assert(read_path("./child/../child/data") == 0);
    snprintf(path, sizeof path, "%s-outside/data", argv[1]);
    assert(read_path(path) == AT_E_PERMISSION);
    assert(read_path("../state-outside/data") == AT_E_PERMISSION);
    assert(read_path("link") != 0);
    assert(read_path("directory-link/data") != 0);
    AtBytes *bytes = at_bytes_copy("changed", 7);
    int64_t count;
    assert(at_file_write(&count, "link", 4, bytes) != 0 && count == 0);

    /* Missing authority must prevent acquisition, independently of whether
     * the path is inside the allowed directory. */
    at_caps_set(AS_CAP_FS_READ, argv[1]);
    assert(at_file_write(&count, "data", 4, bytes) == AT_E_PERMISSION);
    at_caps_set(AS_CAP_FS_WRITE, argv[1]);
    assert(read_path("child/data") == AT_E_PERMISSION);
    assert(at_file_write(&count, "new", 3, bytes) == 0 && count == 7);
    at_caps_set(0, NULL);
    assert(read_path("child/data") == AT_E_PERMISSION);
    at_gc_collect();
    assert(at_gc_live_bytes() == 0);
    return 0;
}
