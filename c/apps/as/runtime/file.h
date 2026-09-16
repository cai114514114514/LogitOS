/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_FILE_H
#define AS_NATIVE_FILE_H
#include "buffer.h"

/* Whole-file operations own their descriptor until they return. Status zero
 * publishes the output; errors are AtExceptionCode values. The compiler adds
 * the call's source span before propagating them to a language handler. */
int at_file_read(AtBytes **out, const char *path, int64_t length);
int at_file_write(int64_t *out, const char *path, int64_t length, const AtBytes *data);
/* Acquisition shared by whole-file helpers and scoped ports. Callers supply
 * both OS flags and the exact capability bits those flags require. */
int at_file_open(int *out, const char *path, int64_t length, int flags, uint32_t permission);
#endif
