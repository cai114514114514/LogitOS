/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <limits.h>

int st_json_unsigned(const struct ag_json *json, int object, const char *name, uint32_t *value)
{
    int token = ag_json_get(json, object, name);
    if (token < 0) {
        return 0;
    }
    const struct ag_jtoken *item = &json->tokens[token];
    uint64_t number = 0;
    if (item->end <= item->start || item->end - item->start > 10) {
        return 0;
    }
    for (int index = item->start; index < item->end; index++) {
        char digit = json->text[index];
        if (digit < '0' || digit > '9') {
            return 0;
        }
        number = number * 10 + (unsigned)(digit - '0');
        if (number > UINT32_MAX) {
            return 0;
        }
    }
    *value = (uint32_t)number;
    return 1;
}

int st_json_int(const struct ag_json *json, int object, const char *name, int *value)
{
    uint32_t number;
    if (!st_json_unsigned(json, object, name, &number) || number > INT_MAX) {
        return 0;
    }
    *value = (int)number;
    return 1;
}

/* Unsaved tabs are authoritative. A disk module is accepted only if it still
 * matches the compiler's byte count/checksum; otherwise its positions refer
 * to an unavailable revision. The caller owns the returned copy. */
static char *source_text(StEngine *engine, const char *path, int bytes, uint32_t checksum)
{
    char *text = NULL;
    int length = 0;
    for (int index = 0; index < engine->state.count; index++) {
        const StDocument *document = &engine->state.docs[index];
        if (!strcmp(path, document->path)) {
            length = document->length;
            text = malloc((size_t)length + 1);
            if (!text) {
                return NULL;
            }
            memcpy(text, document->text, (size_t)length + 1);
            break;
        }
    }
    if (!text) {
        int exists;
        if (st_read_file(path, &text, &length, &exists) < 0 || !exists) {
            free(text);
            return NULL;
        }
    }
    if (length != bytes || st_hash(text, (size_t)length) != checksum) {
        free(text);
        return NULL;
    }
    return text;
}

static int source_metadata(const struct ag_json *json, const char *path, int *bytes,
                           uint32_t *checksum)
{
    int array = ag_json_get(json, 0, "sources");
    if (array < 0 || json->tokens[array].type != '[') {
        return 0;
    }
    for (unsigned index = 0;; index++) {
        int item = ag_json_at(json, array, index);
        if (item < 0) {
            return 0;
        }
        char *candidate = ag_json_string(json, ag_json_get(json, item, "path"));
        int matches = candidate && !strcmp(path, candidate);
        free(candidate);
        if (matches) {
            return st_json_int(json, item, "bytes", bytes) &&
                   st_json_unsigned(json, item, "checksum", checksum);
        }
    }
}

int st_project_sources_current(StEngine *engine, const struct ag_json *json)
{
    int array = ag_json_get(json, 0, "sources");
    if (array < 0 || json->tokens[array].type != '[' || ag_json_at(json, array, 0) < 0) {
        return 0;
    }
    for (unsigned index = 0;; index++) {
        int item = ag_json_at(json, array, index);
        if (item < 0) {
            return 1;
        }
        int bytes;
        uint32_t checksum;
        char *path = ag_json_string(json, ag_json_get(json, item, "path"));
        char *text = NULL;
        if (path && st_json_int(json, item, "bytes", &bytes) &&
            st_json_unsigned(json, item, "checksum", &checksum)) {
            text = source_text(engine, path, bytes, checksum);
        }
        free(path);
        if (!text) {
            return 0;
        }
        free(text);
    }
}

static int parse_problem(StEngine *engine, const struct ag_json *json, int item, StProblem *problem)
{
    memset(problem, 0, sizeof *problem);
    char *path = ag_json_string(json, ag_json_get(json, item, "path"));
    char *message = ag_json_string(json, ag_json_get(json, item, "message"));
    char *code = ag_json_string(json, ag_json_get(json, item, "code"));
    uint32_t checksum;
    int valid = path && strlen(path) < sizeof problem->path && message && code &&
                source_metadata(json, path, &problem->source_length, &problem->source_checksum) &&
                st_json_unsigned(json, item, "source_checksum", &checksum) &&
                checksum == problem->source_checksum &&
                st_json_int(json, item, "start", &problem->start) &&
                st_json_int(json, item, "end", &problem->end) &&
                st_json_int(json, item, "line", &problem->line) &&
                st_json_int(json, item, "column", &problem->column) &&
                problem->end >= problem->start && problem->line > 0 && problem->column > 0;
    char *text = valid ? source_text(engine, path, problem->source_length, checksum) : NULL;
    valid = text && st_boundary(text, problem->source_length, problem->start) &&
            st_boundary(text, problem->source_length, problem->end);
    if (valid) {
        snprintf(problem->path, sizeof problem->path, "%s", path);
        snprintf(problem->message, sizeof problem->message, "%s", message);
        snprintf(problem->code, sizeof problem->code, "%s", code);
    }
    free(text);
    free(path);
    free(message);
    free(code);
    return valid;
}

void st_finished_job(StEngine *engine)
{
    StRunner *runner = &engine->state.runner;
    if (engine->runner_seen || !runner->finished) {
        return;
    }
    engine->runner_seen = 1;
    if (runner->cancelled || runner->io_error) {
        st_engine_notice(engine, runner->cancelled
                                     ? "Process stopped"
                                     : "Process I/O failed; output may be incomplete");
        return;
    }
    int exit_code = WIFEXITED(runner->status) ? WEXITSTATUS(runner->status) : -1;
    if (!runner->mode) {
        snprintf(engine->state.notice, sizeof engine->state.notice, "Process finished: %s %d%s",
                 exit_code < 0 ? "signal" : "exit",
                 exit_code < 0 ? WTERMSIG(runner->status) : exit_code,
                 runner->truncated ? "; output limit reached" : "");
        return;
    }
    if (runner->tab < 0 || runner->tab >= engine->state.count ||
        !st_runner_current(runner, &engine->state.docs[runner->tab], runner->tab) ||
        !st_runner_imports_current(runner, engine->state.docs, engine->state.count)) {
        st_engine_notice(engine, "Check finished for an older version; current edits are kept.");
        return;
    }
    if (runner->truncated) {
        st_engine_notice(engine,
                         "Compiler diagnostics exceeded the output limit; result rejected.");
        return;
    }
    struct ag_json json;
    if (ag_json_parse(&json, runner->text, (size_t)runner->length) < 0) {
        st_engine_notice(engine, "Compiler did not return structured diagnostics. Check /bin/as.");
        return;
    }
    int version, language, bytes;
    uint32_t checksum;
    char *path = ag_json_string(&json, ag_json_get(&json, 0, "file"));
    int valid = st_json_int(&json, 0, "version", &version) && version == 1 &&
                st_json_int(&json, 0, "language", &language) && language == 3 &&
                st_json_int(&json, 0, "source_bytes", &bytes) && bytes == runner->source_length &&
                st_json_unsigned(&json, 0, "source_checksum", &checksum) &&
                checksum == st_hash(runner->source, (size_t)runner->source_length) && path &&
                !strcmp(path, engine->state.docs[runner->tab].path) &&
                st_project_sources_current(engine, &json);
    free(path);
    int array = ag_json_get(&json, 0, "diagnostics");
    int truncated = ag_json_get(&json, 0, "truncated");
    valid = valid && truncated >= 0 && json.tokens[truncated].type == 'f';
    valid = valid && array >= 0 && json.tokens[array].type == '[';
    StProblem parsed[32];
    int count = 0;
    while (valid && count < 32) {
        int item = ag_json_at(&json, array, (unsigned)count);
        if (item < 0) {
            break;
        }
        valid = parse_problem(engine, &json, item, &parsed[count]);
        count++;
    }
    int ok = ag_json_get(&json, 0, "ok");
    if (ok < 0 || json.tokens[ok].type != (count ? 'f' : 't') ||
        (array >= 0 && ag_json_at(&json, array, 32) >= 0) || exit_code != (count ? 1 : 0)) {
        valid = 0;
    }
    if (valid) {
        memcpy(engine->state.problems, parsed, (size_t)count * sizeof *parsed);
        engine->state.problem_count = count;
        engine->state.problem_tab = runner->tab;
        engine->state.problem_revision = runner->revision;
        snprintf(engine->state.notice, sizeof engine->state.notice,
                 count ? "%d compiler problems" : "A3 project check passed", count);
    } else {
        st_engine_notice(engine,
                         "Invalid or outdated compiler result; diagnostics were not attached.");
    }
    ag_json_free(&json);
}
