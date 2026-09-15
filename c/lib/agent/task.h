/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_TASK_H
#define LOGIT_AGENT_TASK_H
#include <stdint.h>
#include <stddef.h>
#include "../../../include/abi/aex_agent.h"

#define AG_TASK_VERSION 1u
#define AG_OBJECTS 32
#define AG_RECEIPTS 64
#define AG_PATH 128
#define AG_GOAL 2048
#define AG_CALLS_DEFAULT 32u
#define AG_ACTIVE_MAX 2u
enum ag_result { AG_OK=0, AG_E_IO=-1, AG_E_ARGUMENT=-2, AG_E_VERSION=-3,
    AG_E_SCOPE=-4, AG_E_CONFLICT=-5, AG_E_BUDGET=-6, AG_E_STATE=-7,
    AG_E_LIMIT=-8, AG_E_MODEL=-9, AG_E_NOTFOUND=-10, AG_E_UNCERTAIN=-11 };
enum ag_phase { AG_QUEUED=1, AG_RUNNING, AG_WAIT_MODEL, AG_PAUSED,
    AG_CANCELLED, AG_DONE, AG_CONFLICT, AG_RECONCILE, AG_WAIT_BUDGET };
enum ag_role { AG_FINDER=1, AG_EDITOR=2, AG_SPECIALIST=3 };
enum ag_right { AG_READ=1, AG_RIGHT_EDIT=2 };
struct ag_object {
    uint64_t id, revision;
    uint32_t rights, bytes, checksum, kind;
    char path[AG_PATH];
};
struct ag_receipt { uint64_t operation, revision; uint32_t checksum, bytes; };
struct ag_task {
    uint64_t id, generation, revision, next_operation;
    uint32_t version, phase, calls, call_limit, active, object_count, receipt_count;
    uint32_t document_bytes, document_checksum, finder_state, editor_state;
    uint32_t pending_bytes, pending_checksum;
    uint32_t workflow, stage, restarts;
    uint32_t notes_bytes, notes_checksum, model_bytes, model_checksum;
    uint64_t model_operation,model_revision;
    uint32_t model_input_checksum,pending_phase;
    uint64_t pending_operation, pending_revision;
    uint64_t peak_worker_rss_frames,worker_cpu_ns,recovery_ms;
    uint64_t manual_base_revision;
    uint32_t manual_base_bytes,manual_base_checksum;
    char app_id[AEX_AGENT_ID_MAX];
    char goal[AG_GOAL], output[AG_PATH], artifact[AG_PATH], reason[192];
    struct ag_object objects[AG_OBJECTS];
    struct ag_receipt receipts[AG_RECEIPTS];
};
struct ag_document { char *bytes; uint32_t length; uint64_t revision; };
void ag_task_init(struct ag_task *, uint64_t id, const char *goal, const char *output);
int ag_task_valid(const struct ag_task *);
int ag_task_control(struct ag_task *, unsigned phase, unsigned extend);
int ag_task_call(struct ag_task *);
int ag_task_enter(struct ag_task *);
void ag_task_leave(struct ag_task *);
int ag_task_grant(struct ag_task *, const struct ag_object *);
int ag_task_authorize(const struct ag_task *, unsigned role, uint64_t object, unsigned rights);
int ag_task_commit(struct ag_task *, struct ag_document *, uint64_t op,
                   uint64_t expected_revision, const char *, uint32_t length);
int ag_document_set(struct ag_document *, const char *, uint32_t, uint64_t);
void ag_document_free(struct ag_document *);
int ag_utf8(const char *, uint32_t);
uint32_t ag_checksum(const void *, size_t);
const char *ag_phase_name(unsigned);
int ag_citations_valid(const struct ag_task *,const char *);
int ag_manual_preserved(const char *base,const char *edited,const char *candidate);
/* Slots contain state AND document, so a receipt and its effect share the
 * same durable commit. Generation is advanced only after fsync and close. */
int ag_store_save(const char *directory, struct ag_task *, const struct ag_document *);
int ag_store_load(const char *directory, struct ag_task *, struct ag_document *);
#endif
