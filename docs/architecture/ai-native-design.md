# AI-native design

LogitOS represents agent work as persistent tasks tied to application identities, source snapshots, and document revisions. A userland broker launches application workers, performs model requests, and publishes results. TextEdit provides a review workflow that binds each decision to the current document revision and the displayed proposal. The main implementation is in `c/apps/agent/agentd.c` and `c/lib/agent/`.

## Execution model

The window manager supervises the broker through `wm_agent_service()` in `c/kernel/gui/wm.c`. The broker processes client messages and model responses incrementally in an event loop, allowing it to handle task controls and document edits while inference is pending.

Applications enter their worker role through `c/apps/crt0_agent.asm` and `c/lib/agent/activation.c`. Activation uses the application identity and mode supplied by the kernel. `c/apps/agent/catalog.json` supplies application identities and domain descriptions; `tools/agent_catalog.py` generates the catalog used by the runtime.

Document work proceeds through two roles. Finder reads granted materials and produces research notes. After the broker saves those notes, TextEdit receives them alongside the current document and prepares a candidate. State contexts from other catalog applications select that application's specialist role. `launch_work()` in `c/apps/agent/agentd.c` chooses the application and workflow stage.

Each worker runs in a new user address space. `proc_agent_spawn()` in `c/kernel/exec/load/exec.c` reads an executable snapshot, checks its AEX application identity and image digest, and loads that snapshot. The child receives zero capability bits and one authenticated channel at descriptor 3. Verifying and loading the same bytes binds the worker identity to the executable content.

Workers assemble model input and return research notes or candidate text. The broker manages source grants, credentials, model requests, task commits, and publication.

## Policy and authority

The policy model combines application identity, broker authorization, and a kernel syscall allowlist.

| Layer | Information used | Enforcement |
| --- | --- | --- |
| Application identity | Application ID, image digest, activation mode, UID, GID, and execution generation | Identifies applications and channel endpoints |
| Broker authorization | Authenticated caller, task, worker role, source objects, rights, and revisions | Controls context publication, source reads, task operations, and commits |
| Worker syscall policy | Syscall number and descriptor argument | Restricts kernel operations available to workers |

`include/abi/aex_agent.h` defines manifests, identities, activation modes, and the spawn ABI. Manifests declare supported objects, actions, contexts, and state versions. The broker creates task grants from accepted context.

Before publishing context, the SDK checks the broker's peer identity against the installed broker image. The broker checks incoming clients against registered application images and the current session UID. Authenticated Unix sockets check live credentials and execution generation. These checks are implemented in `c/lib/agent/sdk.c`, `c/lib/agent/registry.c`, `c/apps/agent/agentd.c`, and `c/net/core/unix.c`.

The allowlist in `include/abi/agent_policy.h` permits memory management, polling, futex synchronization, identity queries, resource accounting, exit, and yield. Reads, writes, closes, nonblocking setup, and peer identity queries require descriptor 3. `c/kernel/exec/syscall.c` applies the allowlist before syscall dispatch. For example, a worker call to `SYS_SCHED_OBSERVE` returns an error before reaching the scheduler observation handler. The signal-return entry in `c/kernel/cpu/irq/interrupts.c` applies the same policy.

The model receives one supported tool, `complete_work`, carrying a `content` string. `c/lib/agent/model_json.c` accepts either that tool result or completion text with the `stop` finish reason. Both paths yield text for broker validation.

## Context and source access

The SDK publishes file selections, document snapshots, or application state. The broker accepts selections from Finder, document contexts from TextEdit, and state contexts from registered applications. Task creation checks that the caller owns the context or has controller authority. These paths are in `c/lib/agent/sdk.c` and `c/apps/agent/agentd.c`.

Task creation copies source bytes into task storage. Each source record contains an object ID, revision, rights, byte count, checksum, and source path. Workers request slices of these saved objects through their broker channel. The broker calls `ag_task_authorize()` in `c/lib/agent/task.c`, checks the requested revision, and verifies the saved object's size and checksum before returning a slice.

Source filesystem access and artifact creation use the helper in `c/apps/agent/userfs.c`. Its child closes inherited service handles, clears supplementary groups, and adopts the session user's UID and GID before performing the requested operation. This applies the user's filesystem credentials while the broker retains its service credentials.

`c/lib/agent/worker.c` assembles the user goal, application memory, and source slices. The editor role adds research notes and the current document. Source labels identify an object and byte range, such as `[S1:0-128]`. The broker sends the assembled input to the configured model endpoint.

The model instructions classify source material and research notes as data. Broker authorization follows the saved grants and authenticated worker role. `ag_citations_valid()` in `c/lib/agent/task.c` checks source IDs and byte bounds, including every range in a grouped citation. A result for a task with nonempty sources requires at least one citation. TextEdit can retrieve the saved source ranges for inspection.

## Review before apply

TextEdit enables review when creating a document task and when attaching its workspace to an existing task. The interface and requests are implemented in `c/apps/gui/textedit/textedit_work.inc` and `c/apps/gui/textedit/textedit.c`.

The review sequence is:

1. The broker saves the candidate in a review journal, then moves the task to `AG_CONFLICT`.
2. TextEdit retrieves the proposal using its journal generation and lets the user compare current and proposed text, with changed ranges highlighted.
3. The decision carries the current document revision and the displayed proposal generation.
4. The broker validates both values. Approval enters publication; declining completes the task with the current document.

`c/lib/agent/review.c` records the proposal's base revision, workflow, operation, and journal generation. Matching the displayed generation prevents a decision about an earlier proposal from approving a replacement.

When pending local edits exist, TextEdit synchronizes them and refreshes the comparison before accepting a further review decision. Broker-accepted edits advance the document revision. In a task with review enabled, an edit submitted against an older revision is saved as a local draft and places the task in conflict.

Approval commits the candidate as the task document and publishes a report artifact in the selected output directory. The broker saves the intended bytes, operation receipt, and publication intent before creating the public file. Subsequent agent publications use revision suffixes.

Recovery compares an existing artifact with the saved byte count and checksum. If the artifact is absent, recovery creates it from the saved document. A mismatch moves the task into reconciliation. These paths are in `c/apps/agent/agentd.c`.

`tests/unit/agent_review_test.c` contains cases for stale document and proposal decisions, recovery from a torn proposal snapshot, cancellation, workflow changes, and receipt checks against replay.

## Persistence and Project bindings

`c/lib/agent/store.c` stores task state and document bytes together in alternating slots. Checksums cover the header, task state, and document. A successful save advances the generation after synchronization and close. For snapshots with supported layouts, loading selects the newest valid generation.

The saved task includes its goal, source grants, document revision, workflow stage, model call budget, receipts, and pending publication state. The broker also saves model responses with their operation, input checksum, and document revision. A matching request can reuse the saved response.

The broker reserves and persists a model call before submitting the request. Pause and cancellation are persisted before workers are stopped and their channels dropped. Configuration failures, provider failures, exhausted budgets, and storage uncertainty become task states with reasons exposed through the task interface. Session changes reload storage for the new session user. These mechanisms are in `c/apps/agent/agentd.c`.

A Project corresponds to the task's output directory. `c/apps/agent/project_bindings.inc` records filesystem identities for that directory and, as they become available, the originating document, output, and published artifact. Before scheduling and publication, binding refresh resolves current paths and checks Project membership and the published artifact's revision. This lets tasks follow Project renames and routes conflicting filesystem changes into reconciliation.

## Remote model requests

The shipped configuration in `fsroot/etc/agent.conf` selects DeepSeek's chat completions endpoint over TLS, the `deepseek-flash` model, disabled thinking, and an output limit of 8,192 tokens.

`c/lib/agent/model_config.c` validates configuration fields, duplicate fields, numeric bounds, host syntax, request paths, and credential bytes. Credential loading requires a successful permission change to mode `0600` before reading the key contents.

`c/lib/agent/model.c` sends HTTP requests with bearer authentication and pumps transport progress incrementally. Requests use buffered completions with streaming disabled; `c/lib/agent/model_json.c` decodes the completed response.

For QEMU, `tools/agent_gateway.py` provides an optional host gateway. It keeps the provider credential on the host and generates a bearer token for the guest. The gateway binds to host loopback, checks the requested model and output limit, and applies call and concurrency budgets. Its metrics record request counts, HTTP status, and elapsed time.

## Local inference

The local inference application in `c/apps/lm/lm.c` loads LOGITLM model files. It attempts a mapping with read access and uses a copy path when mapping fails or the user requests copying.

`c/lib/nn/model.c` validates model geometry, flags, header fields, and expected file size before constructing tensor views. It supports floating point, 8-bit, and 4-bit weights, using `c/lib/nn/quant4.c` for the 4-bit layout. Tensor views reference the supplied model bytes, allowing the loader to use mapped storage.

The decoder in `c/lib/nn/infer.c` implements causal attention, rotary position encoding, RMS normalization, and SwiGLU layers. Its inference state holds activation buffers and the key/value cache. `lm_state_bytes()` calculates the allocation required for that state before inference begins.

The application accepts byte prompts and supplied token IDs, with greedy or temperature and nucleus sampling. The byte prompt path treats each byte as a token. Token ID input supports tokenization on the host.

## Application memory

Application memory is UTF-8 text associated with an application identity in the session user's task storage. Applications can read and write their own namespace. The assistant and task controller can select another application's namespace; workers receive read access to memory for their assigned application. The broker enforces these choices in `c/apps/agent/agentd.c`.

Writes carry an expected revision and operation ID. `c/lib/agent/memory.c` reloads saved state before committing and uses operation receipts to recognize repeated writes with matching content. `c/lib/agent/worker.c` includes the stored text in model input.

## Limitations

- Review is opt in. Other tasks can publish after revision, citation, and edit checks succeed. The edit preservation check in `c/lib/agent/task.c` uses a text heuristic.
- The local inference application is not connected to broker model dispatch. Broker requests use the HTTP transport in `c/lib/agent/model.c`.
- The broker permits one active model request. It has eight task slots and up to 32 source objects per task. Combined source content and document content each have a 1 MiB bound; application memory has a 64 KiB bound.
- Filesystem source files must contain UTF-8 text and have a `.txt` or `.md` extension. Application-supplied document and state contexts provide text directly.
- Model transport applies a 4 MiB response body limit and a 120-second deadline checked while pumping the request. Plain HTTP configuration is accepted for the exact numeric addresses `10.0.2.2` and `127.0.0.1`; other hosts require TLS.
- Citation validation checks object references and ranges. Assessing whether a source supports a statement remains part of review.
- Persistent Project bindings require filesystem identity support. Older volumes retain path behavior. `docs/FS_AI_NATIVE_DESIGN.md` describes a broader target protocol and identifies its implementation scope.
- The isolation and authorization mechanisms have experimental security scope. `TRANSPARENCY.md` and `SECURITY.md` describe the project's limits concerning production security and hostile workloads.

## See also

- [Agent runtime](../userland/agent-runtime.md)
- [Applications and AEX packages](applications-aex.md)
- [Security model](security-model.md)
- [Virtual file system](../filesystems/vfs.md)

## Source map

| File | What it contains |
| --- | --- |
| `include/abi/aex_agent.h` | Manifests, identities, activation modes, and spawn ABI |
| `include/abi/agent_policy.h` | Worker syscall allowlist |
| `c/kernel/exec/load/exec.c` | Worker snapshot verification, loading, and channel transfer |
| `c/kernel/exec/syscall.c` | Worker policy enforcement before dispatch |
| `c/kernel/cpu/irq/interrupts.c` | Worker policy enforcement for signal return |
| `c/net/core/unix.c` | Authenticated channel identity and lifetime checks |
| `c/kernel/gui/wm.c` | Broker supervision |
| `c/apps/crt0_agent.asm` | Application activation entry wrapper |
| `c/lib/agent/activation.c` | Worker role selection |
| `c/apps/agent/catalog.json` | Application identities and domain descriptions |
| `tools/agent_catalog.py` | Catalog and manifest generation |
| `c/apps/agent/agentd.c` | Authorization, scheduling, model dispatch, publication, and recovery |
| `c/apps/agent/userfs.c` | Filesystem operations under session user credentials |
| `c/apps/agent/project_bindings.inc` | Project and document identity bindings |
| `c/lib/agent/sdk.h` | Protocol messages and task and source bounds |
| `c/lib/agent/sdk.c` | Broker authentication and context publication |
| `c/lib/agent/registry.c` | Installed application identity and digest lookup |
| `c/lib/agent/worker.c` | Model input assembly and worker requests |
| `c/lib/agent/task.h` | Task state, grants, receipts, and workflow fields |
| `c/lib/agent/task.c` | Authorization, revision checks, receipts, and citation validation |
| `c/lib/agent/store.c` | Alternating task and document snapshots |
| `c/lib/agent/review.c` | Proposal journal and decision validation |
| `c/lib/agent/memory.h` | Application memory interface and size bound |
| `c/lib/agent/memory.c` | Memory persistence and revision checks |
| `c/apps/gui/textedit/textedit.c` | Document synchronization and workspace attachment |
| `c/apps/gui/textedit/textedit_work.inc` | Proposal comparison, source inspection, and review decisions |
| `c/lib/agent/model_config.c` | Configuration validation and credential loading |
| `c/lib/agent/model.c` | HTTP model transport |
| `c/lib/agent/model_json.c` | Request encoding and completion decoding |
| `fsroot/etc/agent.conf` | Shipped model configuration |
| `tools/agent_gateway.py` | Host gateway and provider budgets |
| `c/apps/lm/lm.c` | Local inference application |
| `c/lib/nn/model.c` | LOGITLM validation and tensor loading |
| `c/lib/nn/infer.c` | Inference state, token processing, and sampling |
| `c/lib/nn/quant4.c` | 4-bit weight layout and arithmetic |
| `tests/unit/agent_review_test.c` | Review validation and recovery test cases |
| `docs/FS_AI_NATIVE_DESIGN.md` | Project and filesystem protocol proposal |
| `TRANSPARENCY.md` | Evidence and feature claim rules |
| `SECURITY.md` | Experimental security scope |