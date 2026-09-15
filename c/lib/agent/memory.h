/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_MEMORY_H
#define LOGIT_AGENT_MEMORY_H
#include "task.h"
#define AG_MEMORY_VERSION 1u
#define AG_MEMORY_MAX 65536u

/* The broker supplies an existing, validated private directory and the
 * authenticated stable AppID. It serializes calls for that directory; this
 * module provides no cross-process lock or authority to choose directories.
 * Memory is explicit UTF-8 text, never a model KV cache or inferred preference.
 * An absent checkpoint loads as empty text at revision 1 without creating a
 * file. Corrupt/incompatible checkpoints are not reset on load or commit.
 * Initialize out to {0}; ag_document_free releases returned text. */
int ag_memory_load(const char *directory,const char *app_id,struct ag_document *out);
/* Reloads durable state before CAS. Returns 0 after a new durable effect,
 * 1 for a matching recorded operation, or an AG_E_* error. Successful calls
 * return the current revision; 64 receipts are retained without compaction.
 * A failed save may have reached storage: retry the SAME operation/payload
 * after loading, so its durable receipt resolves uncertain completion. */
int ag_memory_commit(const char *directory,const char *app_id,uint64_t operation,
                      uint64_t expected_revision,const char *utf8,uint32_t length,
                      uint64_t *revision);
#endif
