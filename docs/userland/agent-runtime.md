# Agent runtime

LogitOS runs application agents through `agentd`, a userland broker that manages application context, source grants, model requests, document revisions, and persistent tasks. Applications publish context and provide worker processes; the broker captures source material, schedules work, and publishes results. The implementation lives in `c/apps/agent/` and `c/lib/agent/`, with shipped model settings in `fsroot/etc/agent.conf`.

## Service lifetime and identity

The window manager supervises the broker through `wm_agent_service()` in `c/kernel/gui/wm.c`. After reaping processes, it periodically checks the tracked service process and starts the installed `/bin/agentd` when needed.

The broker runs with root credentials and tracks the session UID and GID for user file operations. It listens on the Unix socket defined by `AG_SOCKET`, `/run/logit-agent`, in `c/lib/agent/sdk.h`. When its event loop observes a different session UID, it closes existing connections, clears contexts and task slots, and loads the new user's stored tasks. This lifecycle is implemented in `c/apps/agent/agentd.c`.

Authentication combines kernel process identity with installed package metadata. `ag_registry()` in `c/lib/agent/registry.c` reads an AEX v3 package's AppID and agent manifest and computes SHA-256 over the package bytes. `ag_peer()` obtains the connected process's identity from the kernel. The broker matches the client's UID, AppID, and image digest against its registry. SDK clients also compare the broker's identity with the installed broker package before publishing context.

The identity ABI is defined in `include/abi/aex_agent.h`. `proc_agent_bind_image()` in `c/kernel/exec/load/exec.c` installs the image identity and assigns a generation when binding it to a process.

## Application context and activation

The context protocol supports three forms of input:

| Context | Producer and contents |
|---|---|
| Selection | Finder publishes selected paths, or the current directory when its selection is empty. |
| Document | TextEdit publishes the current text and revision, including edits held in memory. |
| State | Applications publish text describing their application state through shared activation or GUI helpers. |

The structures and SDK entry points are in `c/lib/agent/sdk.h` and `c/lib/agent/sdk.c`. Finder's integration is in `c/apps/gui/files/files.c` and `c/apps/gui/files/files_project.inc`; TextEdit's integration is in `c/apps/gui/textedit/textedit.c` and `c/apps/gui/textedit/textedit_work.inc`.

Catalogued applications enter through `_agent_start` in `c/apps/crt0_agent.asm`. It calls `ag_activation()` and restores the application's original entry stack when activation returns control to the original startup code. Worker dispatch follows the activation mode supplied by the kernel.

`c/lib/agent/activation.c` also handles these application options:

| Option | Behavior |
|---|---|
| `--aex-capabilities` | Prints application identity, versions, and domain information. |
| `--aex-memory` | Reads the application's memory namespace. |
| `--ask` | Creates a task from a goal and supplied context text. |
| `--task-from` | Finder entry for creating a task from source paths and an output directory. |

## Worker execution

The broker creates workers through `SYS_AGENT_SPAWN`. In `c/kernel/exec/load/exec.c`, `proc_agent_spawn()` reads the requested AEX package into a memory snapshot, checks its AppID and image digest, and loads the worker from that snapshot. This binds execution to the captured bytes checked during launch.

The child receives the authenticated channel at descriptor 3 and starts with zero capability bits. Worker syscalls pass through `aex_agent_syscall_allowed()` in `include/abi/agent_policy.h`. The policy permits channel operations on descriptor 3 and selected memory, polling, synchronization, timing, identity, and resource-accounting calls. Enforcement appears in `c/kernel/exec/syscall.c`, with the signal-return path checked in `c/kernel/cpu/irq/interrupts.c`.

Before receiving work, `ag_worker()` in `c/lib/agent/worker.c` checks its activation mode, channel descriptor, and broker parent identity. The broker assigns the connection to a task slot and role, then checks incoming worker messages against that assignment.

## Task execution

Task creation starts with a published context, a goal, and an output directory. The broker validates these inputs, captures source material, attaches project bindings where supported, and saves the initial task before scheduling it. The sequence is implemented in `c/apps/agent/agentd.c`.

The document workflow has two stages:

1. A Finder worker reads captured sources and checkpoints research notes.
2. A TextEdit worker receives the notes, captured sources, and current document, then produces a candidate.

State contexts from other applications select a specialist worker through the application's catalog entry. The broker uses its domain description as part of the model instruction. Finder and TextEdit retain their research and editor roles when receiving text through the shared activation options.

`c/lib/agent/worker.c` builds model input from the goal, application memory, source chunks, and material required by the worker's role. Each source chunk carries an object identifier and byte range, such as `[S1:0-100]`. `ag_citations_valid()` in `c/lib/agent/task.c` checks citation syntax, object existence, and range bounds, including grouped ranges. Accepted model output for a task with nonempty source objects must contain a valid citation.

Task phases cover queuing, execution, waiting for a model, pausing, cancellation, completion, document conflict, reconciliation, and budget exhaustion. `ag_task_init()` assigns a budget of 32 model calls. Before starting a provider request, the broker loads configuration and credentials, reserves a call, and persists that reservation.

Pause and cancellation save the requested state before terminating associated workers and closing their channels. An interrupted running step returns to the queue; the third worker interruption pauses the task. Resumption follows the saved workflow stage. These transitions are implemented in `c/lib/agent/task.c` and `c/apps/agent/agentd.c`.

## File access under the user's credentials

`c/apps/agent/userfs.c` provides the broker's adapter to the existing file system. For each operation, `ag_userfs()` forks a child, closes inherited descriptors in its cleanup range except the reply pipe, clears supplementary groups, and adopts the session GID and UID. The child performs the requested operation and returns status, metadata, or data through the pipe. The user's VFS credentials then govern granted file access.

The adapter supports metadata lookup, file reads, directory listing, output-directory checks, file creation, file-reference lookup, and identity-to-path resolution. Path checks use `lstat()` on existing components, with parent-path checks for creation and file-reference requests. File reads validate the opened descriptor with `fstat()`, check its type and length, and require the read to reach the expected end of file.

The broker copies captured text into task storage. Each granted object records an identifier, revision, rights, byte count, checksum, and source path. Worker source requests name an object, revision, and slice. The broker checks the grant and validates the stored copy's length and checksum before returning a slice. These checks are in `c/apps/agent/agentd.c`.

Project bindings add persistent file identity. `c/lib/agent/files.c` obtains volume identifiers, object identifiers, and revisions. `c/apps/agent/project_bindings.inc` uses them to resolve renamed projects and documents. Before scheduling or publication, the broker checks project membership and the recorded artifact revision. A failed binding check moves the task into reconciliation.

## Persistence and publication

The broker stores each user's tasks beneath `/state/agents/`, using a directory named from the session UID and numbered task directories. Application memory uses directories named from catalog application names within the same user state directory. Directory creation requests mode `0700`; snapshot and blob creation requests mode `0600`. The layout is defined in `c/apps/agent/agentd.c`.

`ag_store_save()` in `c/lib/agent/store.c` writes task state and document bytes into alternating snapshot files. Each snapshot contains a generation and checksums for its header, state, and document. The in-memory generation advances after successful writes, `fsync()`, and close.

Loading validates the snapshot format, state, document, and checksums. An incompatible version produces a version error; among compatible valid snapshots, loading selects the greatest generation. An interrupted running task returns to the queue, or enters reconciliation when a publication operation is pending.

Operation receipts record an operation identifier, resulting revision, byte count, and checksum. `ag_task_commit()` in `c/lib/agent/task.c` recognizes a retry when the operation identifier, length, and checksum match a recorded receipt. New commits check the expected document revision before advancing it. A reused identifier with a changed payload produces a conflict.

Publication proceeds through a saved intent:

1. Commit the intended document and receipt, record the destination and pending operation, and save the task.
2. Create the public artifact.
3. Record its file binding and save the completed publication state.

Creation uses `SYS_CREATE_FILE`, which reaches `vfs_create_file()` in `c/fs/vfs/vfs.c`. That function holds the mount transaction while checking destination absence and creating the file.

During recovery, the broker compares an existing artifact with the saved byte count and checksum. It can create an absent artifact from the saved document; a mismatch moves the task into reconciliation. This recovery path is in `c/apps/agent/agentd.c`.

## Document review

TextEdit can enable a review journal for a task. `c/lib/agent/review.c` saves a proposal before the broker exposes it for review. Decisions carry both the current task document revision and the proposal generation displayed to the user. The broker validates both before applying the decision.

The protocol provides retrieval messages for the current document, agent candidate, saved local draft, and source slices. TextEdit's work interface uses these messages to display proposals and source material. Its integration is in `c/apps/gui/textedit/textedit_work.inc`.

The Tasks application in `c/apps/gui/assistant/assistant.c` provides task creation, inspection, pause, resume, cancellation, budget extension, and candidate review. Both interfaces submit decisions to the broker's task and publication paths.

## Application memory

Application memory stores UTF-8 facts and preferences supplied through the runtime's memory interfaces. Workers request the memory associated with their application and include it in model input. The broker selects the namespace from the authenticated application or worker role.

`c/lib/agent/memory.c` reuses the task snapshot and receipt machinery. Each write reloads stored state, checks the expected revision, and commits using an operation identifier. A namespace with absent checkpoints starts with empty text at revision 1.

Applications can read and write their own namespace. Tasks and `agentctl` can select another application's namespace for inspection or editing. The broker enforces these access rules in `c/apps/agent/agentd.c`; SDK callers use `ag_memory_read()` and `ag_memory_write()` from `c/lib/agent/sdk.c`.

## Catalog and package integration

`c/apps/agent/catalog.json` supplies application names, stable AppIDs, guest executable paths, source paths, GUI or CLI classification, domain descriptions, and action names. `tools/agent_catalog.py` checks unique names and IDs and verifies source-file existence. It generates the application names used by the build and the runtime table consumed by `c/lib/agent/catalog.c`.

The earlier description grouped object, action, and context masks as values chosen by application name. The generator assigns object mask `7` to every entry; application name selects the action and context masks. The JSON action names describe application behavior, while the generator supplies the manifest masks.

Catalog order is part of the broker's current integration. Finder, TextEdit, Tasks, and agentd occupy the first four positions. The broker uses indices 0 and 1 for research and editor workers and index 2 for the Tasks controller. Startup requires successful registration of the first four packages.

`tests/agent.mk` augments catalogued application links with the activation entry, SDK archive, and C library archive. `tools/agent_link.py` retains the application's link inputs, selects `_agent_start`, and writes metadata containing the AppID, manifest, and ELF digest. `tools/mkaex.py` checks the metadata's ELF digest before using it to build an AEX v3 package.

Before launching a worker, the broker rereads its installed package and checks its image digest, state version, and capability version. Changes to catalog identity, activation, or package metadata should be reviewed across these build and runtime paths.

## Model configuration

The shipped settings in `fsroot/etc/agent.conf` are:

```ini
host=api.deepseek.com
port=443
tls=1
path=/chat/completions
model=deepseek-flash
thinking=0
max_tokens=8192
key_file=/etc/agent.key
```

The broker loads the guest configuration from `/etc/agent.conf` for model requests. `c/lib/agent/model_config.c` parses bounded `key=value` text, validates field syntax and numeric ranges, and checks duplicate and unknown fields. It accepts empty lines and comment lines beginning with `#`. Names and values should be unpadded because parsing preserves spaces.

Before reading credential bytes, the loader requires a successful request to set the configured key file's mode to `0600`. It trims trailing carriage returns and newlines and validates the remaining characters. Configuration or credential failure puts model work into a waiting state. After setup is corrected, resuming the task causes a later request to reload the settings.

`c/lib/agent/model.c` sends an HTTP POST with bearer authentication to the configured endpoint. `c/lib/agent/model_json.c` requests a response with streaming disabled and decodes either completed message text or one `complete_work` function result containing text.

## QEMU host gateway

`tools/agent_gateway.py` provides a host gateway for model requests. It reads `deepseek_api` or `deepseek_api_key` from the selected environment file, retains the provider credential on the host, and creates a temporary guest token. Its generated guest configuration points to QEMU's host address, `10.0.2.2`.

`tools/agent_session.py` installs the generated configuration, token, broker, and supplied application packages into the selected disk before launching QEMU. Its installation path copies the existing guest tree and metadata before replacing selected entries.

The `run` recipe in `Makefile` invokes the session launcher with `--optional-env`. An absent environment file or missing provider credential produces an offline diagnostic and continues to desktop boot. The `run-agent` target in `tests/agent.mk` invokes the launcher with the environment file selected by `AGENT_ENV`.

## Inspection and validation entry points

`c/apps/coreutils/agentctl.c` implements command-line inspection and control. Its status output includes task phases, revisions, model-call counts, source-object counts, output paths, and service counters. Other operations retrieve documents, candidates, local drafts, source slices, project information, and application memory. The verify operation compares the current task document byte by byte with its published artifact.

`tests/agent.mk` defines these validation entry points:

| Target | Coverage |
|---|---|
| `test-agent` | Task rules, snapshot I/O, configuration, memory, review, authenticated channels, and session installation. |
| `test-agent-catalog-build` | Package metadata, linked activation symbols, and executable entry wiring. |
| `test-agent-startup` | Broker startup and missing-credential scenarios, preceded by its negative control. |
| `test-agent-cli` | Legacy command behavior through packaged applications with agent activation. |
| `test-agent-acceptance` | Guest task scenarios, provider integration, and publication fault recovery. |
| `test-textedit-work` | TextEdit work and review scenarios, with its negative control and document prerequisites. |

The previous description called `test-agent-cli` a task-creation check. Its runner, `tests/boot/run-agent-cli.py`, checks command arguments, pipes, file operations, directory listings, exit status, and byte counts.

These targets describe available checks. Execution results apply to the revision and environment recorded by a run.

## Limitations

- The broker schedules one worker and one provider request at a time. It has eight task slots and twelve connection slots. See `c/apps/agent/agentd.c` and `c/lib/agent/sdk.h`.
- Filesystem source capture accepts UTF-8 files with the `.txt` or `.md` extension. Tasks support 32 source objects and 1 MiB of combined source text; traversal rejects depths greater than eight. Documents are bounded at 1 MiB. See `c/apps/agent/agentd.c`, `c/lib/agent/sdk.h`, and `include/abi/aex_agent.h`.
- Application memory is bounded at 64 KiB. Receipt storage holds 64 operations, including memory writes, with no receipt compaction. See `c/lib/agent/memory.h` and `c/lib/agent/task.h`.
- Citation validation checks syntax, object identifiers, and byte ranges. It provides no semantic check that a cited passage supports a generated statement. Model decoding supports text results through the forms described above; general command execution and external actions are outside that decoder. See `c/lib/agent/task.c` and `c/lib/agent/model_json.c`.
- Plain HTTP configuration is accepted for the exact hosts `127.0.0.1` and `10.0.2.2`. Task bindings fall back to paths on volumes that lack persistent file identity. See `c/lib/agent/model_config.c` and `c/apps/agent/project_bindings.inc`.
- Authentication, worker restrictions, and recovery operate within an experimental OS. `TRANSPARENCY.md` records the project's security and validation limits, including the experimental standing of production security and the documented scope of LogitFS crash consistency.

## Source map

| File | What it contains |
|---|---|
| `c/apps/agent/agentd.c` | Broker authentication, scheduling, source capture, publication, and recovery. |
| `c/apps/agent/userfs.h`, `c/apps/agent/userfs.c` | File operations under the session user's credentials. |
| `c/apps/agent/project_bindings.inc` | Project membership, persistent identity, revision checks, and binding recovery. |
| `c/apps/agent/catalog.json` | Application identities, paths, domains, and action descriptions. |
| `c/lib/agent/sdk.h`, `c/lib/agent/sdk.c` | IPC structures, framing, broker authentication, and client helpers. |
| `c/lib/agent/registry.c` | Installed package metadata and digest extraction. |
| `c/apps/crt0_agent.asm`, `c/lib/agent/activation.c`, `c/lib/agent/gui.c` | Application entry, activation options, and GUI context publication. |
| `c/lib/agent/worker.c` | Worker authentication, model input construction, and result submission. |
| `c/lib/agent/task.h`, `c/lib/agent/task.c` | Task phases, grants, budgets, citations, revisions, and receipts. |
| `c/lib/agent/store.c` | Alternating snapshots and recovery validation. |
| `c/lib/agent/files.c` | File references and binding journal serialization. |
| `c/lib/agent/review.c` | Proposal storage and review version checks. |
| `c/lib/agent/memory.h`, `c/lib/agent/memory.c` | Application memory persistence and limits. |
| `c/lib/agent/catalog.c` | Generated runtime catalog and AppID lookup. |
| `c/lib/agent/model_config.c`, `c/lib/agent/model.c`, `c/lib/agent/model_json.c` | Configuration, credential loading, HTTP transport, and response decoding. |
| `include/abi/aex_agent.h`, `include/abi/agent_policy.h` | Identity, activation, and worker syscall policy. |
| `c/kernel/exec/load/exec.c`, `c/kernel/exec/syscall.c`, `c/kernel/cpu/irq/interrupts.c` | Kernel identity binding, worker creation, and policy enforcement. |
| `c/kernel/gui/wm.c` | Broker supervision. |
| `c/fs/vfs/vfs.c` | File creation under the mount transaction. |
| `c/apps/coreutils/agentctl.c`, `c/apps/gui/assistant/assistant.c` | Command-line control and Tasks interface. |
| `c/apps/gui/files/files.c`, `c/apps/gui/files/files_project.inc` | Finder context publication and project tasks. |
| `c/apps/gui/textedit/textedit.c`, `c/apps/gui/textedit/textedit_work.inc` | Document synchronization and proposal review. |
| `tools/agent_catalog.py`, `tools/agent_link.py`, `tools/mkaex.py` | Catalog generation, link integration, and AEX packaging. |
| `fsroot/etc/agent.conf` | Shipped model settings. |
| `tools/agent_session.py`, `tools/agent_gateway.py` | QEMU session installation and host model gateway. |
| `Makefile`, `tests/agent.mk` | Build integration, launch recipes, and validation targets. |
| `tests/boot/run-agent-cli.py` | Legacy CLI behavior checks through packaged applications. |
| `TRANSPARENCY.md` | Project security and validation limits. |

## See also

- [AI-native design](../architecture/ai-native-design.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [System services](services.md)
- [Security model](../architecture/security-model.md)
- [Testing](../architecture/testing.md)
