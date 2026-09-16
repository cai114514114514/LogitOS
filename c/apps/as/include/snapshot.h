/* SPDX-License-Identifier: MIT */
#ifndef AETHER_SOURCE_SNAPSHOT_H
#define AETHER_SOURCE_SNAPSHOT_H
#include "project.h"

/* A length-framed pipe protocol, independent of the language version. Each
 * decimal number ends with LF. After the magic and source count, each entry
 * contains path length, source length, raw path bytes, then raw source bytes.
 * Framing avoids escaping/reinterpreting UTF-8 or allocating temporary files
 * in a user's project just to check unsaved buffers. No module is executed. */
#define AS_SNAPSHOT_MAGIC "AETHER-SNAPSHOT-1\n"
#define AS_SNAPSHOT_PATH_MAX 511
#define AS_SNAPSHOT_SOURCE_MAX (1024 * 1024)
#define AS_SNAPSHOT_FILES_MAX 64

typedef struct {
    AsSourceOverlay *sources;
    char **storage;
    int count;
} AsSourceSnapshot;

/* The snapshot owns both paths and text. Even on failure, free() below is
 * valid and releases all partially read entries. NULL means success; errors
 * are static strings suitable for an option diagnostic. */
const char *as_snapshot_read(FILE *input, AsSourceSnapshot *snapshot);
void as_snapshot_free(AsSourceSnapshot *snapshot);
#endif
