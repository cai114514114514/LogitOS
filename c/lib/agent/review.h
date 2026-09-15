/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_REVIEW_H
#define LOGIT_AGENT_REVIEW_H
#include "task.h"
/* A separate two-slot journal reuses the checked task/document serializer.
 * It is never scheduled. Its revision is the proposal's base document, its
 * next_operation identifies the worker result, and workflow binds the goal.
 * The old task ABI and all pre-review snapshots therefore remain readable. */
struct ag_review {int enabled;struct ag_task record;struct ag_document candidate;};
void ag_review_free(struct ag_review *);
int ag_review_load(const char *dir,struct ag_review *);
int ag_review_enable(const char *dir,const struct ag_task *,struct ag_review *,const char *binding);
int ag_review_propose(const char *dir,const struct ag_task *,struct ag_review *,
    uint64_t operation,uint64_t revision,const char *,uint32_t bytes);
int ag_review_pending(const struct ag_task *,const struct ag_review *);
int ag_review_validate(const struct ag_task *,const struct ag_review *,uint64_t revision,uint64_t proposal);
#endif
