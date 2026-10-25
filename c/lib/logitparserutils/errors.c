/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <parserutils/errors.h>
#include <string.h>

static const char *const names[] = {
    "OK", "NOMEM", "BADPARM", "INVALID", "FILENOTFOUND", "NEEDDATA", "BADENCODING", "EOF"
};

static const char *const descriptions[] = {
    "No error", "Insufficient memory", "Bad parameter", "Invalid input",
    "File not found", "Insufficient data", "Unsupported encoding", "EOF"
};

const char *parserutils_error_to_string(parserutils_error error)
{
    return (unsigned)error < sizeof(descriptions) / sizeof(*descriptions) ? descriptions[error] : NULL;
}

parserutils_error parserutils_error_from_string(const char *name, size_t length)
{
    if (!name) return PARSERUTILS_BADPARM;
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
        if ((strlen(names[i]) == length && memcmp(name, names[i], length) == 0) ||
            (strlen(descriptions[i]) == length && memcmp(name, descriptions[i], length) == 0))
            return (parserutils_error)i;
    return PARSERUTILS_BADPARM;
}
