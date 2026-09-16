/* SPDX-License-Identifier: MIT */
#ifndef STUDIO_INTERNAL_H
#define STUDIO_INTERNAL_H
#include "engine.h"
#include "storage.h"
#include "../../lib/agent/json.h"
#include <dirent.h>

struct StEngine {
    StState state;
    StHost host;
    char compiler[256], modules[256];
    uint64_t checkpoint_due, check_due;
    int check_pending, runner_seen;
    /* Completion has its own worker: a slow run/check must not block editor
     * requests. A generation invalidates late results after Esc or a click. */
    StRunner completion_runner;
    uint64_t completion_generation, completion_request;
    int completion_pending;
    char expanded[ST_FILES][128];
    int expanded_count;
};

StDocument *st_current(StEngine *e);
void st_changed(StEngine *e);
void st_session_save(StEngine *e);
void st_finished_job(StEngine *e);
uint64_t st_now(StEngine *e);
int st_completion_tick(StEngine *engine);
int st_start_completion(StEngine *engine);
void st_complete_fallback(StEngine *engine);
int st_json_unsigned(const struct ag_json *json, int object, const char *name, uint32_t *value);
int st_json_int(const struct ag_json *json, int object, const char *name, int *value);
int st_project_sources_current(StEngine *engine, const struct ag_json *json);
#endif
