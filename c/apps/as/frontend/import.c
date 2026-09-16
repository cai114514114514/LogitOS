/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int source_exists(AsTypedProject *project, const char *path)
{
    for (int i = 0; i < project->noverlays; i++) {
        if (!strcmp(project->overlays[i].path, path)) {
            return 1;
        }
    }
    FILE *file = fopen(path, "rb");
    if (file) {
        fclose(file);
        return 1;
    }
    /* Unreadable local sources still shadow the library. A permission failure
     * must not silently compile a different module with the same name. */
    return errno != ENOENT;
}

int at_parse_import_path(AtParser *parser, char module[64], char path[512], Token *site)
{
    *site = at_parse_expect(parser, T_IDENT, "expected module name");
    if (site->type != T_IDENT || site->len >= 64) {
        if (site->len >= 64) {
            at_parse_error(parser, *site, "AS3300", "Module name is too long");
        }
        return 0;
    }
    at_parse_name(*site, module, 64);
    int standard = !strcmp(module, "std") && at_parse_has(parser, T_DOT);
    char relative[512] = "";
    if (!standard) {
        strcpy(relative, module);
    }
    while (at_parse_accept(parser, T_DOT)) {
        Token component = at_parse_expect(parser, T_IDENT, "expected module name after '.'");
        if (component.type != T_IDENT) {
            return 0;
        }
        if (component.len >= 64 || strlen(relative) + (size_t)component.len + 5 >= 512) {
            at_parse_error(parser, component, "AS3300", "Import path is too long");
            return 0;
        }
        at_parse_name(component, module, 64);
        if (relative[0]) {
            strcat(relative, "/");
        }
        strcat(relative, module);
    }

    char directory[512];
    snprintf(directory, sizeof directory, "%s", parser->p->modules[parser->m].path);
    char *slash = strrchr(directory, '/');
    if (slash) {
        slash[1] = 0;
    } else {
        directory[0] = 0;
    }
    int length = snprintf(path, 512, "%s%s.as", directory, relative);
    if (length < 0 || length >= 512) {
        at_parse_error(parser, *site, "AS3300", "Import path is too long");
        return 0;
    }
    /* std is an explicit root, not a local module searched ahead of the
     * library. Missing std modules must never fall back to a sibling file. */
    if (standard || (!source_exists(parser->p, path) && parser->p->library[0])) {
        if (!parser->p->library[0]) {
            at_parse_error(parser, *site, "AS3300", "std import requires a standard library root");
            return 0;
        }
        length = snprintf(path, 512, "%s/%s.as", parser->p->library, relative);
        if (length < 0 || length >= 512) {
            at_parse_error(parser, *site, "AS3300", "Standard library import path is too long");
            return 0;
        }
    }
    return 1;
}
