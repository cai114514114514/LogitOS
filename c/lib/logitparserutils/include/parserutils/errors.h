/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_ERRORS_H
#define LOGIT_PARSER_ERRORS_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum parserutils_error {
    PARSERUTILS_OK = 0, PARSERUTILS_NOMEM, PARSERUTILS_BADPARM,
    PARSERUTILS_INVALID, PARSERUTILS_FILENOTFOUND, PARSERUTILS_NEEDDATA,
    PARSERUTILS_BADENCODING, PARSERUTILS_EOF
} parserutils_error;
const char *parserutils_error_to_string(parserutils_error error);
parserutils_error parserutils_error_from_string(const char *name, size_t length);
#ifdef __cplusplus
}
#endif
#endif
