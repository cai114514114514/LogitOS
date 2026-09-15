/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_DOCUMENT_H
#define STUDIO_DOCUMENT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ST_DOCUMENT_LIMIT (1024*1024)
#define ST_UNDO_STEPS 128
#define ST_HISTORY_LIMIT (512*1024)
typedef struct {
    int at,removed,inserted,caret,anchor;
    char *old_text,*new_text;
} StEdit;
typedef struct {
    char path[128];
    char *text,*base;
    int length,capacity,base_length,exists,caret,anchor,top,left;
    uint64_t revision,checkpoint,draft_generation;
    StEdit edits[ST_UNDO_STEPS];
    int undo_count,undo_cursor,history_bytes;
} StDocument;

int st_boundary(const char *s,int n,int p);
int st_utf8(const char *s,int n);
int st_prev(const char *s,int p);
int st_next(const char *s,int p,int n);
int st_encode(unsigned cp,char s[4]);
uint32_t st_hash(const void *data,size_t n);
int st_reserve(StDocument *d,int length);
int st_init(StDocument *d,const char *path,const char *text,int n,int exists);
void st_dispose(StDocument *d);
int st_dirty(const StDocument *d);
int st_apply(StDocument *d,int lo,int hi,const char *text,int n,int remember);
int st_replace(StDocument *d,const char *s,int n);
int st_undo(StDocument *d,int redo);
int st_find(StDocument *d,const char *query,int from);
#endif
