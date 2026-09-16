/* SPDX-License-Identifier: MIT */
#include "include/completion.h"
#include "common/numeric.h"
#include "sema/internal.h"
#include "completion_internal.h"
#include <string.h>

static int identifier_byte(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

static int same_name(Token token, const char *name)
{
    return token.len == (int)strlen(name) && !memcmp(token.start, name, (size_t)token.len);
}

/* The parser retains field nodes while recovering from `module.`. Matching
 * the original dot token avoids treating dots inside strings/comments as
 * code, and carries the owning function without a second indentation parser. */
static AtNode *receiver_node(AsTypedProject *project, size_t start)
{
    AtModule *module = &project->modules[0];
    size_t dot = start;
    while (dot && (module->source[dot - 1] == ' ' || module->source[dot - 1] == '\t')) {
        dot--;
    }
    if (!dot || module->source[dot - 1] != '.') {
        return NULL;
    }
    dot--;
    for (int index = 0; index < project->nnodes; index++) {
        AtNode *node = project->nodes[index];
        /* The original dot survives resolution to functions, globals and
         * bound methods. It also identifies chained/call receivers exactly. */
        if (node->module == 0 && node->a && node->member_separator.start == module->source + dot) {
            return node;
        }
    }
    return NULL;
}

static int module_target(AsTypedProject *project, AtNode *field)
{
    if (field->a->kind != AN_NAME || field->function < 0 ||
        field->function >= project->nfunctions) {
        return -1;
    }
    AtFunction *function = &project->functions[field->function];
    /* Syntax recovery may prevent full checking. The compiler's binding pass
     * still establishes function-wide shadowing, including assignments after
     * the caret. Never offer module members through a shadowing local. */
    for (AtFunction *scope = function; scope;) {
        Checker checker = {.p = project, .f = scope};
        if (!scope->checked) {
            at_scope_collect_bindings(&checker);
        }
        if (at_scope_local(scope, field->a->token) >= 0) {
            return -1;
        }
        scope = scope->lexical_parent ? &project->functions[scope->lexical_parent - 1] : NULL;
    }
    Checker checker = {.p = project, .f = function};
    return at_scope_import_module(&checker, field->a->token);
}

void at_completion_item(AsTypedProject *project, FILE *out, const char *name, const char *kind,
                        int type, int module, Token declaration, const char *prefix, size_t bytes,
                        int *count)
{
    if (!declaration.start || strlen(name) < bytes || memcmp(name, prefix, bytes)) {
        return;
    }
    int position = (*count)++;
    if (position >= 64) {
        return;
    }
    if (position) {
        fputc(',', out);
    }
    fputs("{\"name\":", out);
    at_quote(out, name);
    fputs(",\"kind\":", out);
    at_quote(out, kind);
    fputs(",\"type\":", out);
    at_quote(out, type > 0 && type < project->ntypes ? project->types[type].name : "<unknown>");
    fputs(",\"path\":", out);
    at_quote(out, project->modules[module].path);
    size_t start = (size_t)(declaration.start - project->modules[module].source);
    fprintf(out, ",\"start\":%zu,\"end\":%zu}", start, start + declaration.len);
}

static void item(AsTypedProject *project, FILE *out, const char *name, const char *kind, int type,
                 int module, Token declaration, const char *prefix, size_t bytes, int *count)
{
    if (name[0] == '_' || strlen(name) < bytes || memcmp(name, prefix, bytes)) {
        return;
    }
    at_completion_item(project, out, name, kind, type, module, declaration, prefix, bytes, count);
}

int as_typed_complete_report(AsTypedProject *project, size_t caret, FILE *out)
{
    if (!project || project->oom || !project->nmodules || !project->modules[0].source) {
        return -1;
    }
    AtModule *entry = &project->modules[0];
    if (as_source_version(entry->source) != AS_LANGUAGE_NATIVE) {
        return -1;
    }
    size_t length = entry->diagnostics.bytes;
    if (caret > length ||
        (caret < length && ((unsigned char)entry->source[caret] & 0xc0) == 0x80)) {
        return -1;
    }
    size_t start = caret;
    while (start && identifier_byte((unsigned char)entry->source[start - 1])) {
        start--;
    }
    AtNode *field = receiver_node(project, start);
    int target = field ? module_target(project, field) : -1;
    /* Recognize a module import even if it failed to load or was shadowed:
     * an empty authoritative result must not fall back to guessed exports. */
    int handled = field == NULL;
    if (field && (field->a->kind == AN_NAME || field->a->kind == AN_GLOBAL ||
                   field->a->kind == AN_FUNCTION)) {
        for (int index = 0; index < entry->nimports; index++) {
            AtImport *import = &entry->imports[index];
            if (!import->member[0] && same_name(field->a->token, import->name)) {
                handled = 1;
            }
        }
    }
    int object = target < 0 && field ? at_completion_object_type(project, field) : 0;
    if (object) {
        handled = 1;
    }
    if (project->oom) {
        return -1;
    }
    fputs("{\"version\":1,\"language\":3,\"file\":", out);
    at_quote(out, entry->path);
    fprintf(out,
            ",\"source_bytes\":%zu,\"source_checksum\":%u,\"caret\":%zu,"
            "\"replace_start\":%zu,\"handled\":%s,\"items\":[",
            length, entry->diagnostics.checksum, caret, start, handled ? "true" : "false");
    int count = 0;
    if (target >= 0) {
        const char *prefix = entry->source + start;
        for (int index = 0; index < project->nfunctions; index++) {
            AtFunction *function = &project->functions[index];
            if (function->module == target && !function->module_initializer &&
                !function->lexical_parent && !function->method_owner && function->template_id < 0) {
                item(project, out, function->name, "function", function->result, target,
                     function->token, prefix, caret - start, &count);
            }
        }
        for (int index = 0; index < project->nglobals; index++) {
            AtGlobal *global = &project->globals[index];
            if (global->module == target) {
                item(project, out, global->name, "variable", global->type, target, global->token,
                     prefix, caret - start, &count);
            }
        }
        for (int index = AT_BUILTIN_LAST + 1; index < project->ntypes; index++) {
            AtType *type = &project->types[index];
            if (type->module == target && type->declaration.start &&
                (type->kind == AT_STRUCT || type->kind == AT_CLASS || type->kind == AT_LAYOUT)) {
                item(project, out, type->name, "type", index, target, type->declaration, prefix,
                     caret - start, &count);
            }
        }
    }
    if (object) {
        at_completion_object_items(project, field, object, out, entry->source + start,
                                   caret - start, &count);
    }
    fprintf(out, "],\"truncated\":%s,\"sources\":[", count > 64 ? "true" : "false");
    for (int index = 0; index < project->nmodules; index++) {
        AtModule *module = &project->modules[index];
        if (index) {
            fputc(',', out);
        }
        fputs("{\"path\":", out);
        at_quote(out, module->path);
        fprintf(out, ",\"bytes\":%zu,\"checksum\":%u}", module->diagnostics.bytes,
                module->diagnostics.checksum);
    }
    fputs("]}\n", out);
    return 0;
}
