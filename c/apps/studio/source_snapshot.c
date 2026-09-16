/* SPDX-License-Identifier: MIT */
#include "runner.h"
#include "../as/include/snapshot.h"
#include <stdio.h>

void st_runner_release_sources(StRunner *runner)
{
    for (int index = 0; index < runner->source_count; index++) {
        free(runner->sources[index].text);
    }
    free(runner->payload);
    runner->payload = NULL;
    runner->source = NULL;
    runner->source_count = 0;
    runner->payload_length = 0;
}

int st_runner_capture(StRunner *runner, const StDocument *documents, int count, int active)
{
    if (count < 1 || count > ST_RUNNER_SOURCES || active < 0 || active >= count) {
        return -1;
    }
    size_t capacity = sizeof AS_SNAPSHOT_MAGIC + 32;
    for (int index = 0; index < count; index++) {
        const StDocument *document = &documents[index];
        StCapturedSource *source = &runner->sources[index];
        source->text = malloc((size_t)document->length + 1);
        if (!source->text) {
            st_runner_release_sources(runner);
            return -1;
        }
        runner->source_count++;
        memcpy(source->text, document->text, (size_t)document->length + 1);
        strcpy(source->path, document->path);
        source->length = document->length;
        source->revision = document->revision;
        capacity += strlen(source->path) + (size_t)source->length + 32;
    }
    runner->payload = malloc(capacity);
    if (!runner->payload) {
        st_runner_release_sources(runner);
        return -1;
    }

    /* Build in memory before forking. Disk writes would expose half a snapshot
     * to another check, and saving the user's buffers would change their work.
     * The existing nonblocking pipe sends this framing incrementally. */
    size_t used = (size_t)snprintf(runner->payload, capacity, AS_SNAPSHOT_MAGIC "%d\n", count);
    for (int index = 0; index < count; index++) {
        StCapturedSource *source = &runner->sources[index];
        size_t path_bytes = strlen(source->path);
        used += (size_t)snprintf(runner->payload + used, capacity - used, "%zu\n%d\n", path_bytes,
                                 source->length);
        memcpy(runner->payload + used, source->path, path_bytes);
        used += path_bytes;
        memcpy(runner->payload + used, source->text, (size_t)source->length);
        used += (size_t)source->length;
    }
    runner->payload_length = (int)used;
    runner->source = runner->sources[active].text;
    runner->source_length = runner->sources[active].length;
    runner->revision = runner->sources[active].revision;
    return 0;
}

const StCapturedSource *st_runner_source(const StRunner *runner, const char *path)
{
    for (int index = 0; index < runner->source_count; index++) {
        if (!strcmp(path, runner->sources[index].path)) {
            return &runner->sources[index];
        }
    }
    return NULL;
}

int st_runner_imports_current(const StRunner *runner, const StDocument *documents, int count)
{
    if (count < runner->source_count) {
        return 0;
    }
    for (int index = 0; index < runner->source_count; index++) {
        /* The entry's generation and bytes are checked by st_runner_current.
         * This companion guard extends the same rule to every other tab. */
        if (index == runner->tab) {
            continue;
        }
        const StCapturedSource *source = &runner->sources[index];
        const StDocument *document = &documents[index];
        if (strcmp(source->path, document->path) || source->revision != document->revision ||
            source->length != document->length ||
            memcmp(source->text, document->text, (size_t)source->length)) {
            return 0;
        }
    }
    return 1;
}
