/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AGENT_SDK_H
#define LOGIT_AGENT_SDK_H
#include "task.h"
#include "memory.h"
#include "files.h"
#define AG_SOCKET "/run/logit-agent"
#define AG_WIRE_MAGIC 0x33475041u
#define AG_PAYLOAD_MAX (8u*1024u*1024u)
#define AG_SOURCE_MAX (1024u*1024u)
#define AG_MODEL_INPUT_MAX (3u*1024u*1024u+128u*1024u)
#define AG_TASKS 8
enum ag_message_type {AG_CONTEXT=1,AG_CREATE,AG_STATUS,AG_CONTROL,AG_REVISE,
    AG_DOCUMENT,AG_EDIT,AG_ACCEPT_CANDIDATE,AG_SAVE,AG_MEMORY_GET,AG_MEMORY_SET,
    AG_WORK_VIEW,AG_WORK_DECIDE,AG_WORK_SOURCE,AG_PROJECT_VIEW,AG_TASK_BEGIN=20,AG_SOURCE,AG_MODEL,
    AG_CHECKPOINT,AG_RESULT,AG_PROGRESS,AG_METRICS,AG_REPLY=100};
struct ag_message {
    uint32_t magic,version,type,bytes;
    int32_t status;uint32_t role;
    uint64_t task,operation,object,revision;
};
struct ag_context {
    uint32_t kind,count,document_bytes,reserved;
    uint64_t revision;
    char output[AG_PATH];
    char paths[AG_OBJECTS][AG_PATH];
};
struct ag_create {uint64_t context;char goal[AG_GOAL],output[AG_PATH];};
struct ag_control {uint32_t phase,extend;};
struct ag_project_view {struct logit_file_id id;uint64_t revision;
    uint32_t count,reserved;struct ag_task tasks[AG_TASKS];};
struct ag_slice {uint32_t offset,length;};
/* Additive messages leave existing AEX v3 task/snapshot layouts unchanged.
 * WORK_VIEW object=1 opts into review; CREATE object=1 seeds a reviewed task
 * from TextEdit's authenticated document context. Decisions bind both the
 * current document revision and the exact proposal generation displayed. */
struct ag_work_view {struct ag_task task;uint64_t proposal,base_revision;
    uint32_t review_enabled,candidate_bytes;};
struct ag_work {struct ag_task task;uint32_t memory_bytes,reserved;};
struct ag_worker_metrics {uint64_t rss_frames,cpu_ns;};
struct ag_memory_request {char app_id[AEX_AGENT_ID_MAX];};
struct ag_status {uint32_t count,reserved;uint64_t context,idle_wakes,service_ms;char selection[AG_PATH];struct ag_task tasks[AG_TASKS];
    uint64_t service_cpu_ns,poll_returns,poll_errors;};

/* No keys or raw process pointers occur in this protocol. Large documents
 * are framed, length-checked and transferred with short-I/O handling. */
int ag_connect(void);
int ag_exchange(int fd,struct ag_message *,const void *,void **);
int ag_call(struct ag_message *,const void *,void **);
int ag_send(int fd,const struct ag_message *,const void *);
int ag_receive(int fd,struct ag_message *,void **);
int ag_self(struct aex_agent_identity *);
int ag_peer(int fd,struct aex_agent_identity *);
int ag_worker(unsigned role);
int ag_publish_state(const char *,const char *,unsigned,uint64_t,uint64_t *);
int ag_publish_selection(const char *const *,unsigned,const char *,uint64_t *);
int ag_publish_document(const char *,const char *,unsigned,uint64_t,uint64_t *);
void ag_show_assistant(uint64_t context);
int ag_registry(const char *,struct aex_agent_identity *,struct aex_agent_manifest *);
int ag_spawn_worker(const char *,const struct aex_agent_identity *,int channel);
/* Only the authenticated app's namespace is accessible to ordinary apps.
 * Tasks/agentctl can inspect or explicitly edit a selected app's memory. */
int ag_memory_read(const char *app_id,char **text,uint32_t *bytes,uint64_t *revision);
int ag_memory_write(const char *app_id,const char *text,uint32_t bytes,uint64_t *revision,uint64_t operation);
#endif
