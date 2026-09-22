/* SPDX-License-Identifier: MIT */
#include "internal.h"

int st_engine_completion_busy(const StEngine *engine)
{
    return engine->completion_pending || st_runner_busy(&engine->completion_runner);
}

int st_start_completion(StEngine *engine)
{
    if (!engine->completion_pending || st_runner_busy(&engine->completion_runner)) {
        return 0;
    }
    engine->completion_pending = 0;
    if (st_runner_start(&engine->completion_runner, engine->state.docs, engine->state.count,
                        engine->state.active, ST_COMPLETE_PROJECT, engine->compiler,
                        engine->state.project) < 0) {
        engine->state.completion_loading = 0;
        return -1;
    }
    engine->completion_request = engine->completion_generation;
    return 0;
}

static int read_items(StEngine *engine, const struct ag_json *json)
{
    StRunner *runner = &engine->completion_runner;
    uint32_t checksum;
    int version, language, length, caret, start;
    char *path = ag_json_string(json, ag_json_get(json, 0, "file"));
    int valid = path && !strcmp(path, engine->state.docs[runner->tab].path) &&
                st_json_int(json, 0, "version", &version) && version == 1 &&
                st_json_int(json, 0, "language", &language) && language == 3 &&
                st_json_int(json, 0, "source_bytes", &length) && length == runner->source_length &&
                st_json_unsigned(json, 0, "source_checksum", &checksum) &&
                checksum == st_hash(runner->source, (size_t)runner->source_length) &&
                st_json_int(json, 0, "caret", &caret) && caret == runner->caret &&
                st_json_int(json, 0, "replace_start", &start) && start <= caret &&
                st_boundary(runner->source, runner->source_length, start) &&
                st_project_sources_current(engine, json);
    free(path);
    int handled = ag_json_get(json, 0, "handled");
    int items = ag_json_get(json, 0, "items");
    if (!valid || handled < 0 || items < 0 || json->tokens[items].type != '[') {
        return 0;
    }
    if (json->tokens[handled].type == 'f') {
        /* Builtin receiver types still awaiting typed queries retain their
         * existing UI. Handled-but-empty module/class queries never take this
         * path, so inaccessible members cannot reappear via old guesses. */
        st_complete_fallback(engine);
        return 1;
    }
    if (json->tokens[handled].type != 't') {
        return 0;
    }
    Completion candidates[64];
    int count = 0;
    for (unsigned index = 0; count < 64; index++) {
        int item = ag_json_at(json, items, index);
        if (item < 0) {
            break;
        }
        char *name = ag_json_string(json, ag_json_get(json, item, "name"));
        char *kind = ag_json_string(json, ag_json_get(json, item, "kind"));
        if (!name || !kind || !*name || strlen(name) >= sizeof candidates[0].label) {
            free(name);
            free(kind);
            continue;
        }
        Completion *candidate = &candidates[count++];
        memset(candidate, 0, sizeof *candidate);
        strcpy(candidate->label, name);
        strcpy(candidate->insert, name);
        candidate->kind = !strcmp(kind, "function") ? CMP_FUNC
                          : !strcmp(kind, "type")   ? CMP_CLASS
                          : !strcmp(kind, "method") ? CMP_METHOD
                          : !strcmp(kind, "field")  ? CMP_FIELD
                                                    : CMP_GLOBAL;
        candidate->score = 100;
        free(name);
        free(kind);
    }
    memcpy(engine->state.completions, candidates, (size_t)count * sizeof *candidates);
    engine->state.completion_count = count;
    engine->state.completion_start = start;
    return 1;
}

int st_completion_tick(StEngine *engine)
{
    StRunner *runner = &engine->completion_runner;
    st_runner_poll(runner);
    int changed = 0;
    if (runner->finished && engine->completion_request) {
        uint64_t request = engine->completion_request;
        engine->completion_request = 0;
        if (request == engine->completion_generation) {
            engine->state.completion_loading = 0;
            const StDocument *document = st_current(engine);
            int current =
                document && st_runner_current(runner, document, engine->state.active) &&
                document->caret == runner->caret &&
                st_runner_imports_current(runner, engine->state.docs, engine->state.count);
            if (current && !runner->io_error && !runner->cancelled && !runner->truncated &&
                WIFEXITED(runner->status) && WEXITSTATUS(runner->status) == 0) {
                struct ag_json json;
                if (ag_json_parse(&json, runner->text, (size_t)runner->length) == 0) {
                    changed = read_items(engine, &json);
                    ag_json_free(&json);
                }
            }
        }
    }
    /* Coalesce requests while one child is running. Reap it before replacing
     * the runner, and launch only the latest caret/source state. */
    st_start_completion(engine);
    return changed;
}
