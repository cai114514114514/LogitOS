/* SPDX-License-Identifier: MIT */
#include "internal.h"

uint64_t st_now(StEngine *e)
{
    return e->host.now(e->host.context);
}

StDocument *st_current(StEngine *e)
{
    return e->state.count ? &e->state.docs[e->state.active] : NULL;
}

const StState *st_engine_state(const StEngine *e)
{
    return &e->state;
}

const StDocument *st_engine_document(const StEngine *e)
{
    return e->state.count ? &e->state.docs[e->state.active] : NULL;
}

void st_engine_notice(StEngine *e, const char *text)
{
    snprintf(e->state.notice, sizeof e->state.notice, "%s", text);
}

StEngine *st_engine_create(const StHost *host)
{
    if (!host || !host->now) {
        return NULL;
    }
    const char *compiler = host->compiler ? host->compiler : "/bin/as",
               *modules = host->modules ? host->modules : "/usr/as/lib";
    if (strlen(compiler) >= 256 || strlen(modules) >= 256) {
        return NULL;
    }
    StEngine *e = calloc(1, sizeof *e);
    if (!e) {
        return NULL;
    }
    e->host = *host;
    strcpy(e->compiler, compiler);
    strcpy(e->modules, modules);
    e->state.problem_tab = -1;
    strcpy(e->state.project, "/docs");
    st_engine_notice(e, "Ready");
    st_runner_init(&e->state.runner);
    st_runner_init(&e->completion_runner);
    return e;
}

void st_engine_destroy(StEngine *e)
{
    if (!e) {
        return;
    }
    st_runner_dispose(&e->state.runner);
    st_runner_dispose(&e->completion_runner);
    for (int i = 0; i < e->state.count; i++) {
        st_dispose(&e->state.docs[i]);
    }
    free(e);
}

void st_changed(StEngine *e)
{
    uint64_t now = st_now(e);
    e->checkpoint_due = now + 500;
    e->check_due = now + 850;
    e->check_pending = 1;
    st_engine_dismiss_completion(e);
    st_engine_notice(e, "Unsaved changes");
}

void st_engine_activate(StEngine *e, int index)
{
    if (index < 0 || index >= e->state.count) {
        return;
    }
    e->state.active = index;
    st_engine_dismiss_completion(e);
    st_session_save(e);
}

int st_engine_close(StEngine *e, int index)
{
    if (index < 0 || index >= e->state.count) {
        return -1;
    }
    if (st_runner_busy(&e->state.runner)) {
        st_engine_notice(e, "Stop the running process before closing a tab.");
        return -1;
    }
    StDocument *d = &e->state.docs[index];
    d->revision++;
    /* Clean tabs only need a cursor bookmark. A removed/read-only parent must
     * not trap them open; dirty source still requires a successful checkpoint. */
    if (st_checkpoint(d) < 0 && st_dirty(d)) {
        st_engine_notice(e, "Draft could not be saved. Tab remains open.");
        return -1;
    }
    st_dispose(d);
    memmove(d, d + 1, (size_t)(e->state.count - index - 1) * sizeof *d);
    e->state.count--;
    memset(&e->state.docs[e->state.count], 0, sizeof *d);
    if (e->state.active > index) {
        e->state.active--;
    }
    if (e->state.active >= e->state.count) {
        e->state.active = e->state.count ? e->state.count - 1 : 0;
    }
    e->state.problem_tab = -1;
    st_engine_dismiss_completion(e);
    st_session_save(e);
    return 0;
}

int st_engine_shutdown(StEngine *e)
{
    int ok = 1;
    for (int i = 0; i < e->state.count; i++) {
        StDocument *d = &e->state.docs[i];
        d->revision++;
        if (st_checkpoint(d) < 0 && st_dirty(d)) {
            ok = 0;
        }
    }
    if (!ok) {
        st_engine_notice(e, "Could not preserve all drafts. Window remains open.");
        return -1;
    }
    st_session_save(e);
    st_engine_stop(e);
    return 0;
}

int st_engine_tick(StEngine *e)
{
    int changed = st_runner_poll(&e->state.runner) | st_completion_tick(e);
    if (!e->runner_seen && e->state.runner.finished) {
        st_finished_job(e);
        changed = 1;
    }
    uint64_t now = st_now(e);
    if (e->checkpoint_due && now >= e->checkpoint_due) {
        e->checkpoint_due = 0;
        for (int i = 0; i < e->state.count; i++) {
            if (e->state.docs[i].revision != e->state.docs[i].checkpoint &&
                st_checkpoint(&e->state.docs[i]) < 0) {
                st_engine_notice(e, "Draft checkpoint failed. Keep this window open.");
                changed = 1;
            }
        }
    }
    if (e->check_pending && now >= e->check_due && !st_runner_busy(&e->state.runner)) {
        st_engine_start(e, 1);
        changed = 1;
    }
    return changed;
}

int st_engine_wait_ms(const StEngine *e)
{
    return st_runner_busy(&e->state.runner) || st_runner_busy(&e->completion_runner) ||
                   e->completion_pending || e->checkpoint_due || e->check_pending
               ? 50
               : 0;
}

void st_engine_stop(StEngine *e)
{
    e->check_pending = 0;
    st_runner_stop(&e->state.runner);
}
