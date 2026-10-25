/* SPDX-License-Identifier: GPL-3.0-or-later
 * Stream state is deliberately opaque. Consumers cannot couple token lifetime
 * to a particular buffering representation. Offsets and advances are UTF-8 bytes.
 * A peek pointer stays valid until advance passes that character, including
 * across refill, insertion and compaction. Destroy ends every such lifetime.
 */
#ifndef LOGIT_PARSER_INPUTSTREAM_H
#define LOGIT_PARSER_INPUTSTREAM_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parserutils_inputstream parserutils_inputstream;
typedef parserutils_error (*parserutils_charset_detect_func)(const uint8_t *, size_t, uint16_t *, uint32_t *);

parserutils_error parserutils_inputstream_create(const char *label, uint32_t origin,
        parserutils_charset_detect_func detect, parserutils_inputstream **result);
parserutils_error parserutils_inputstream_destroy(parserutils_inputstream *s);
parserutils_error parserutils_inputstream_append(parserutils_inputstream *s,
                                               const uint8_t *bytes, size_t length);
parserutils_error parserutils_inputstream_insert(parserutils_inputstream *s,
                                               const uint8_t *bytes, size_t length);
parserutils_error parserutils_inputstream_peek(parserutils_inputstream *s, size_t offset,
                                             const uint8_t **bytes, size_t *length);
parserutils_error parserutils_inputstream_peek_slow(parserutils_inputstream *s, size_t offset,
                                                  const uint8_t **bytes, size_t *length);
void parserutils_inputstream_advance(parserutils_inputstream *s, size_t count);
const char *parserutils_inputstream_read_charset(parserutils_inputstream *s, uint32_t *origin);
parserutils_error parserutils_inputstream_change_charset(parserutils_inputstream *s,
                                                       const char *label, uint32_t origin);
#ifdef __cplusplus
}
#endif
#endif
