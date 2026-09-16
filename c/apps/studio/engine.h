/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_ENGINE_H
#define STUDIO_ENGINE_H
#include "document.h"
#include "runner.h"
#include "../as/editor/completion.h"

#define ST_TABS ST_RUNNER_SOURCES
#define ST_FILES 192
typedef struct StEngine StEngine;

typedef struct {
    char path[128], name[64];
    int depth, directory, expanded;
} StFile;

typedef struct {
    StFile files[ST_FILES];
    int count, limited;
    char path[128];
} StListing;

typedef struct {
    int start, end, line, column;
    char message[256], code[24], path[128];
    uint32_t source_checksum;
    int source_length;
} StProblem;

/* Borrowed, read-only view, valid until engine destruction. Commands may
 * replace its buffers. A renderer must not retain text pointers across calls. */
typedef struct {
    StDocument docs[ST_TABS];
    int count, active;
    StRunner runner;
    char project[128], notice[256];
    StFile files[ST_FILES];
    int tree_count, tree_limited;
    StProblem problems[32];
    int problem_count, problem_tab;
    uint64_t problem_revision;
    Completion completions[64];
    int completion_count, completion_start;
    int completion_loading;
} StState;

/* Platform glue is injected, never linked back to a GUI. Missing settings
 * callbacks disable session persistence; clock is mandatory. Calls are
 * serialized on the owning event loop (the language service uses scratch RAM). */
typedef struct {
    void *context;
    uint64_t (*now)(void *);
    int (*get)(void *, const char *, char *, int);
    void (*set)(void *, const char *, const char *);
    void (*commit)(void *);
    const char *compiler;
    const char *modules;
} StHost;

StEngine *st_engine_create(const StHost *host);
void st_engine_destroy(StEngine *e);
const StState *st_engine_state(const StEngine *e);
const StDocument *st_engine_document(const StEngine *e);
void st_engine_notice(StEngine *e, const char *message);
int st_engine_project(StEngine *e, const char *path);
int st_engine_open(StEngine *e, const char *path);
void st_engine_restore(StEngine *e, const char *launch_path);
void st_engine_activate(StEngine *e, int index);
int st_engine_close(StEngine *e, int index);
int st_engine_shutdown(StEngine *e);
int st_engine_save(StEngine *e);
void st_engine_refresh(StEngine *e);
int st_list_directory(const char *path, StListing *out);
int st_engine_toggle_folder(StEngine *e, int index);
int st_engine_create_entry(StEngine *e, const char *parent, const char *name, int directory);
/* Explicitly confirmed deletion; directories must be empty. Discards the
 * file's recovery slots and open buffer, so it cannot resurrect on restart. */
int st_engine_remove_entry(StEngine *e, const char *path);
void st_engine_start(StEngine *e, int check);
void st_engine_stop(StEngine *e);
int st_engine_tick(StEngine *e);
int st_engine_wait_ms(const StEngine *e);
int st_engine_insert(StEngine *e, const char *text, int bytes);
/* Typing is distinct from paste: pairs/wrapping are one undoable edit. */
int st_engine_type(StEngine *e, unsigned codepoint);
int st_engine_backspace(StEngine *e);
int st_engine_undo(StEngine *e, int redo);
int st_engine_delete(StEngine *e, int forward);
int st_engine_newline(StEngine *e);
int st_engine_find(StEngine *e, const char *query);
int st_engine_replace_found(StEngine *e, const char *query, const char *replacement);
int st_engine_select(StEngine *e, int caret, int anchor);
void st_engine_viewport(StEngine *e, int top, int left);

enum StMove {
    ST_LEFT,
    ST_RIGHT,
    ST_HOME,
    ST_END,
    ST_UP,
    ST_DOWN,
    ST_FIRST,
    ST_LAST
};

void st_engine_move(StEngine *e, enum StMove move, int rows, int extend);
void st_engine_complete(StEngine *e);
int st_engine_completion_busy(const StEngine *e);
void st_engine_dismiss_completion(StEngine *e);
int st_engine_accept_completion(StEngine *e, int index);
int st_engine_select_problem(StEngine *e, int index);
int st_line_start(const StDocument *d, int line);
int st_line_end(const StDocument *d, int offset);
int st_caret_line(const StDocument *d);
#endif
