/* SPDX-License-Identifier: MIT */
#include "ir/model.h"
#include <string.h>

int at_exception_code(Token name)
{
    static const char *const names[] = {
#define AT_NAME_ENTRY(code, name) [code] = #name,
        AT_EXCEPTION_NAMES(AT_NAME_ENTRY)
#undef AT_NAME_ENTRY
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        if (name.len == (int)strlen(names[i]) && !memcmp(name.start, names[i], (size_t)name.len)) {
            return (int)i;
        }
    }
    return -1;
}

void at_initialize_exceptions(AsTypedProject *project)
{
    /* Error is a fixed-layout value. Catch types select its code, without
     * introducing a hidden heap object or borrowing the VM's Value format. */
    project->exception_type = project->ntypes++;
    AtType *error = &project->types[project->exception_type];
    error->kind = AT_STRUCT;
    error->module = -1;
    strcpy(error->name, "Error");
    int index = 0;
#define AT_TYPE_FIELD(name, c_type, language_type)                                                 \
    strcpy(error->names[index], #name);                                                            \
    error->fields[index++] = language_type;
    AT_EXCEPTION_FIELDS(AT_TYPE_FIELD)
#undef AT_TYPE_FIELD
    error->count = index;
}
