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
    char compiler[256],modules[256];
    uint64_t checkpoint_due,check_due;
    int check_pending,runner_seen;
    char expanded[ST_FILES][128];
    int expanded_count;
};
StDocument *st_current(StEngine *e);
void st_changed(StEngine *e);
void st_session_save(StEngine *e);
void st_finished_job(StEngine *e);
uint64_t st_now(StEngine *e);
#endif
