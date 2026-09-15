/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_AEX_AGENT_ABI_H
#define LOGIT_AEX_AGENT_ABI_H
#include <stdint.h>

/* Versioned declarations, never grants. The broker binds this identity to
 * the launched image; an app_id supplied in an IPC payload is not identity. */
#define AEX_AGENT_ABI 1u
#define AEX_AGENT_ID_MAX 64
#define AEX_T_AGENT 0x544e4741u
#define AEX_AGENT_DOCUMENT_MAX (1024u * 1024u)
#define AEX_AGENT_OBJECT_FILE 1u
#define AEX_AGENT_OBJECT_DOCUMENT 2u
#define AEX_AGENT_OBJECT_STATE 4u
#define AEX_AGENT_ACTION_READ 1u
#define AEX_AGENT_ACTION_REPORT 2u
#define AEX_AGENT_ACTION_REVISE 4u
#define AEX_AGENT_ACTION_ANALYZE 8u
#define AEX_AGENT_CONTEXT_SELECTION 1u
#define AEX_AGENT_CONTEXT_DOCUMENT 2u
#define AEX_AGENT_CONTEXT_STATE 4u
struct aex_agent_manifest {
    uint32_t abi, capability_version, state_version;
    uint32_t objects, actions, contexts, document_max, reserved;
};

/* 190..192 were unassigned. A separate spawn shape avoids silently changing
 * CAP_SPAWN's legacy fd inheritance. Exactly one authenticated channel is
 * inherited at fd 3; no stdio or ambient file authority crosses this boundary. */
#define SYS_AGENT_SPAWN 190
#define SYS_AGENT_SELF 191
#define SYS_AGENT_PEER 192
/* Create a new regular file atomically; an existing name is never replaced. */
#define SYS_CREATE_FILE 193
#define AEX_AGENT_FD 3
/* Failure after transfer: the caller must not close/reuse the old fd number. */
#define AEX_SPAWN_CHANNEL_CONSUMED (-3)
#define AEX_ACT_UI 1u
#define AEX_ACT_WORKER 2u
struct aex_agent_spawn {
    uint32_t size, abi;
    int32_t channel_fd;
    uint32_t reserved;
    char app_id[AEX_AGENT_ID_MAX];
    uint8_t image_hash[32];
};
struct aex_agent_identity {
    uint32_t size, abi, mode, state_version;
    int32_t pid, parent_pid, channel_fd;
    uint32_t capability_version;
    uint32_t uid, gid;
    uint64_t generation;
    char app_id[AEX_AGENT_ID_MAX];
    uint8_t image_hash[32];
};
#endif
