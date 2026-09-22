/* SPDX-License-Identifier: MIT */
#ifndef ST_HIGHLIGHT_H
#define ST_HIGHLIGHT_H
enum StInk { ST_INK_TEXT, ST_INK_KEYWORD, ST_INK_STRING, ST_INK_COMMENT,
             ST_INK_NUMBER, ST_INK_OPERATOR, ST_INK_CALL };
/* One style per UTF-8 byte; no allocation, no compiler or module I/O.
 * Unterminated strings remain coloured through EOF, matching AS multiline
 * literals. The bytes in a codepoint always receive the same style. */
void st_highlight(const char *text,int length,unsigned char *styles);
#endif
