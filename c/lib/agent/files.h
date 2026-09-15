/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_FILES_H
#define LOGIT_AGENT_FILES_H
#include "task.h"
#include "../../../include/abi/fs_ref.h"
enum { AG_BIND_PROJECT,AG_BIND_ORIGIN,AG_BIND_OUTPUT,AG_BIND_ARTIFACT,AG_BIND_COUNT };
struct ag_file_info {struct logit_file_id id;uint64_t revision;};
struct ag_binding {struct ag_task journal;struct logit_file_id refs[AG_BIND_COUNT];uint64_t revisions[AG_BIND_COUNT];};
int ag_file_ref(const char *,struct logit_file_id *,uint64_t *revision);
int ag_file_path(const struct logit_file_id *,char *,unsigned);
int ag_file_same(const struct logit_file_id *,const struct logit_file_id *);
int ag_parent_path(const char *,char *,unsigned);
int ag_binding_load(const char *,struct ag_binding *);
int ag_binding_save(const char *,uint64_t task,struct ag_binding *);
#endif
