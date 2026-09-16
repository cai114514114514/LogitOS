/* SPDX-License-Identifier: MIT */
#include "include/snapshot.h"
#include <stdlib.h>
#include <string.h>

static int read_size(FILE *input, size_t limit, size_t *value)
{
    size_t result = 0;
    int digits = 0;
    for (;;) {
        int byte = fgetc(input);
        if (byte == '\n' && digits) {
            *value = result;
            return 1;
        }
        if (byte < '0' || byte > '9' || ++digits > 10) {
            return 0;
        }
        size_t digit = (size_t)(byte - '0');
        if (result > limit / 10 || (result == limit / 10 && digit > limit % 10)) {
            return 0;
        }
        result = result * 10 + digit;
    }
}

void as_snapshot_free(AsSourceSnapshot *snapshot)
{
    for (int i = 0; i < snapshot->count; i++) {
        free(snapshot->storage[i]);
    }
    free(snapshot->storage);
    free(snapshot->sources);
    memset(snapshot, 0, sizeof *snapshot);
}

const char *as_snapshot_read(FILE *input, AsSourceSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    char magic[sizeof AS_SNAPSHOT_MAGIC - 1];
    if (fread(magic, 1, sizeof magic, input) != sizeof magic ||
        memcmp(magic, AS_SNAPSHOT_MAGIC, sizeof magic)) {
        return "Invalid source snapshot header";
    }
    size_t count;
    if (!read_size(input, AS_SNAPSHOT_FILES_MAX, &count) || !count) {
        return "Invalid source snapshot file count";
    }
    snapshot->sources = calloc(count, sizeof *snapshot->sources);
    snapshot->storage = calloc(count, sizeof *snapshot->storage);
    if (!snapshot->sources || !snapshot->storage) {
        return "Cannot allocate source snapshot";
    }
    for (size_t index = 0; index < count; index++) {
        size_t path_bytes, source_bytes;
        if (!read_size(input, AS_SNAPSHOT_PATH_MAX, &path_bytes) || !path_bytes ||
            !read_size(input, AS_SNAPSHOT_SOURCE_MAX, &source_bytes)) {
            return "Invalid source snapshot lengths";
        }
        char *storage = malloc(path_bytes + source_bytes + 2);
        if (!storage) {
            return "Cannot allocate source snapshot entry";
        }
        snapshot->storage[snapshot->count++] = storage;
        char *source = storage + path_bytes + 1;
        if (fread(storage, 1, path_bytes, input) != path_bytes ||
            fread(source, 1, source_bytes, input) != source_bytes) {
            return "Incomplete source snapshot entry";
        }
        if (memchr(storage, 0, path_bytes) || memchr(source, 0, source_bytes)) {
            return "Source snapshot contains NUL bytes";
        }
        storage[path_bytes] = 0;
        source[source_bytes] = 0;
        for (size_t previous = 0; previous < index; previous++) {
            if (!strcmp(storage, snapshot->sources[previous].path)) {
                return "Duplicate source snapshot path";
            }
        }
        snapshot->sources[index] = (AsSourceOverlay){storage, source, source_bytes};
    }
    /* Require EOF, not merely enough bytes for the declared count. Otherwise
     * a truncated producer can silently omit a later unsaved module. */
    if (fgetc(input) != EOF || ferror(input)) {
        return "Trailing data or read failure in source snapshot";
    }
    return NULL;
}
