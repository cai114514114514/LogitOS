/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_MODEL_H
#define LOGIT_AGENT_MODEL_H
#include "task.h"
#include "../../net/http/http1.h"
struct ag_model_config {char host[128],path[192],model[96],key[256];unsigned port,tls,thinking,max_tokens;};
struct ag_model {struct h1_response response;char *request;size_t sent,length;int socket,active;
    uint64_t started;int status;};
/* Strict key=value text, with DeepSeek defaults. Parse does no I/O and returns
 * the separately validated key-file path; only load reads the credential.
 * Both publish an empty output on failure, never partially accepted fields. */
int ag_model_config_parse(struct ag_model_config *,char *key_file,size_t key_capacity,
                          const char *text,size_t length);
int ag_model_config_load(struct ag_model_config *,const char *);
char *ag_model_body(const struct ag_model_config *,const char *instructions,const char *input);
int ag_model_decode(const char *json,size_t length,char **content);
int ag_model_start(struct ag_model *,const struct ag_model_config *,const char *,const char *);
int ag_model_pump(struct ag_model *,char **result);
void ag_model_close(struct ag_model *);
#endif
