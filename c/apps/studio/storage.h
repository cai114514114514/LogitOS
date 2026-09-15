/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_STORAGE_H
#define STUDIO_STORAGE_H
#include "document.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/* Full byte reads/writes: a short operation is progress, not success. The
 * two checked draft slots preserve the last complete edit across a torn write.
 * They include the original bytes so recovery can distinguish a concurrent
 * external edit from our unsaved draft instead of silently overwriting it. */
int st_read_file(const char *path,char **text,int *length,int *exists);
int st_checkpoint(StDocument *d);
int st_open_document(StDocument *d,const char *path);
int st_save_document(StDocument *d);
int st_forget_drafts(const char *path);
#endif
