/* SPDX-License-Identifier: MIT */
#include "ir/model.h"

int at_allocate_type(AsTypedProject *project, int module, Token site)
{
    if (project->ntypes >= AT_TYPES) {
        /* The arena remains intact for editor diagnostics. Repeating this for
         * every signature would obscure the first declaration that exceeded
         * the snapshot's resource budget. Type zero is the poison type. */
        if (!project->type_limit_reported) {
            at_error(project, module, site, "AS3600", "Project type capacity exceeded");
            project->type_limit_reported = 1;
        }
        return AT_ERROR;
    }
    int id = project->ntypes++;
    project->types[id].declaration = site;
    return id;
}
