/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_MIBENUM_H
#define LOGIT_PARSER_MIBENUM_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif
uint16_t parserutils_charset_mibenum_from_name(const char *label, size_t length);
const char *parserutils_charset_mibenum_to_name(uint16_t mib);
bool parserutils_charset_mibenum_is_unicode(uint16_t mib);
#ifdef __cplusplus
}
#endif
#endif
