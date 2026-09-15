/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_JSON_H
#define LOGIT_AGENT_JSON_H
#include <stddef.h>
struct ag_jtoken {int start,end,next,child;char type;};
struct ag_json {const char *text;size_t length;struct ag_jtoken *tokens;int count,cap;};
int ag_json_parse(struct ag_json *,const char *,size_t);
void ag_json_free(struct ag_json *);
int ag_json_get(const struct ag_json *,int,const char *);
int ag_json_at(const struct ag_json *,int,unsigned);
char *ag_json_string(const struct ag_json *,int);
char *ag_json_quote(const char *);
#endif
