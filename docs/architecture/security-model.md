# Security model

LogitOS implements CPU privilege separation, process address spaces, checked user-memory transfers, credentials, file permissions, and signature verification. These mechanisms govern different decisions: memory access, authorization for selected operations, and verification of signed payloads. This page describes their enforcement points and current limits. `SECURITY.md` defines the operational security policy, and `TRANSPARENCY.md` governs security claims.

## Privilege levels and address spaces

Applications execute at ring 3 and kernel code executes at ring 0. `c/kernel/cpu/gdt.h` defines the segment selectors. `c/kernel/cpu/smp/percpu.c` constructs each CPU's descriptors and task state segment, which supplies the kernel stack for entry from user space. `c/boot/enter_user.asm` enters user mode through an interrupt-return frame.

System calls enter through `int 0x80`. `c/kernel/cpu/irq/idt.c` installs this gate with descriptor privilege level 3, allowing applications to invoke it. The dispatcher in `c/kernel/exec/syscall.c` applies capability checks and routes requests to subsystem handlers.

`vmm_new_space()` in `c/kernel/mm/virt/vmm.c` creates private page-table roots for user mappings and retains shared supervisor mappings for the kernel. The address predicates in `c/kernel/mm/mm.h` require each nonempty user range to fit within one of two user address windows. Checking the whole range prevents acceptance of a buffer that spans the kernel mappings between those windows.

The generic exception handler in `c/kernel/cpu/irq/interrupts.c` first offers page faults to memory management. An unresolved exception from ring 3 can become a process signal. When a fault has no applicable signal handler, this path invokes core dump collection and terminates the process.

Machine checks use a dedicated gate. `c/kernel/cpu/power/mce.c` reports valid error banks through a bounded, polled serial sink in `c/kernel/cpu/power/hardware.c`. This permits diagnostics when the interrupted code holds a console lock. Recovery requires a resumable instruction pointer and bank status that passes the handler's checks; other cases panic.

## Memory permissions and user copies

The ELF loader derives page permissions from segment flags. It rejects user load segments requesting both write and execute permission and rejects executable stack requests. These checks live in `c/kernel/exec/load/elf.c`. Initial application stacks receive their mappings through `c/kernel/exec/load/exec.c` and `c/kernel/gui/wm.c`.

CPU setup enables NX and SMEP when supported, in `c/boot/long.asm` and `c/boot/ap_trampoline.asm`. NX marks nonexecutable user pages, including initial stacks. SMEP prevents supervisor execution of user pages. `cpu_prot_report()` in `c/kernel/cpu/prot.c` reports the enabled state by reading processor registers.

`user_copy_from()` and `user_copy_to()` in `c/kernel/exec/usercopy.c` transfer bytes through kernel mappings of physical RAM. For each page, the helper holds an address-space guard, acquires a reference and pin, copies the bytes, then releases the pin and reference. Writes resolve copy-on-write before reaching the destination frame.

The reference keeps the frame allocated during the transfer, and the pin protects it from reclaim. The guard coordinates the copy with mapping changes by sibling threads. Address-space guards can sleep because mapping operations may allocate memory or wait for I/O; `c/kernel/mm/virt/mmguard.h` explains how holding a spinlock across these operations could deadlock reclaim.

`user_range_ok()` faults in and checks a range. Use the copy helpers for the subsequent transfer so that page ownership spans the access. `user_copy_string()` performs bounded copying and returns failure for inaccessible memory or a missing terminator within the capacity.

## Credentials and authentication

Credentials live in a table keyed by PID in `c/fs/vfs/vfs_cred.c`. Lookup checks the process's entry, follows its parent chain, and falls back to the login session. Processes marked as system services receive root credentials through that lookup. Kernel work without a current process also receives root credentials, supporting filesystem setup during boot.

The identity syscalls expose a UID, primary GID, and supplementary groups. Changing them requires UID 0. Effective UID and GID queries return the corresponding UID and GID values.

`SYS_SETSESSION` checks UID 0 and changes the session identity under the credential lock. It records the same UID and GID for the caller when an entry is available. Combining these updates lets authentication code establish the session and drop its caller credential in one operation.

Console login and the graphical greeter share account parsing and password verification through `c/apps/coreutils/accounts.h`. Password records contain a salt, iteration count, and derived key for PBKDF2-HMAC-SHA256. `c/crypto/kdf/pbkdf2.c` generates and verifies these records. Successful enrollment in `c/apps/coreutils/login.c` writes the guest account store, `/etc/passwd`, and applies root ownership and mode `0600`.

For builds without `HWHUD=1`, a nonempty account store makes `c/kernel/gui/wm.c` launch the greeter and wait for a nonzero session UID before starting the desktop. The greeter verifies the password before requesting that transition. Console login can establish the same session.

## Developer SSH provisioning

`HWHUD=1` enables developer SSH provisioning through `LOGIT_HWHUD`. `Makefile` selects the kernel definition, and `tests/uefi.mk` passes it to the UEFI loader build.

In this configuration, `c/boot/efi/loader.c` can read an optional public-key file from the boot volume and deliver it as the `devssh-authorized-keys` boot module. After mounting the root filesystem, `c/kernel/init/kmain.c` calls the provisioner in `c/kernel/init/devssh.c`.

The provisioner validates an Ed25519 public-key list of up to 4096 bytes. It creates a `dev` account with a nonzero UID and a locked password field, `!`, which password verification rejects. It installs the guest file `/home/dev/.ssh/authorized_keys` with account ownership and mode `0600`, and prepares the home and SSH directories with mode `0700`. The root-owned `/etc/sshd.enabled` flag is prepared after the account and keys.

Service startup in `c/apps/coreutils/boot_services.h` requires an enable flag that is a regular file, owned by root, with group and other write bits clear. `c/apps/services/ssh/sshd.c` checks authorized-key file type, ownership, and write permissions, matches the offered key, and verifies the client's authentication signature.

## File permissions and privileged operations

`vmeta_permission()` in `c/fs/vfs/vfs_meta.c` selects owner, group, or other permission bits. Group selection includes supplementary membership. UID 0 passes read and write checks; execute requests require an execute bit in the mode.

The VFS applies these operation checks in `c/fs/vfs/vfs.c`:

| Operation | Required permission or credential |
|---|---|
| Read file contents or list a directory | Read permission on the target |
| Rewrite an existing file | Write permission on the target |
| Create or remove a name | Write and search permission on the parent directory |
| Rename | Write and search permission on both parent directories |
| Change mode | File owner or UID 0 |
| Change owner or group | UID 0 |

Execution paths in `c/kernel/exec/load/exec.c` request execute permission through `vfs_access()`. The interpreter loader in `c/kernel/exec/load/interp.c` requests read and execute permission.

For operation checks, attribute lookup uses a filesystem's paired attribute callbacks when available, with the VFS metadata store as fallback. LogitFS persists mode, UID, and GID through its metadata transaction in `c/fs/logitfs/logitfs.c`.

Earlier comments in `c/fs/vfs/vfs_meta.h` and `c/crypto/trust/pkgsig.h` describe mode and ownership as volatile. That description applies to the VFS RAM store; current LogitFS code stores these fields on disk.

Credentials authorize other privileged operations:

| Operation | Authorization |
|---|---|
| Ptrace attachment | UID 0 or a UID matching the target, in `c/kernel/exec/ptrace.c` |
| Module loading and unloading | UID 0, in `c/kernel/module/modload.c` |
| Start or stop scheduler observation | UID 0 |
| Read scheduler observation status or records | UID 0 or the UID/PID pair recorded at the start |

Scheduler observation checks live in `c/kernel/sched/observe/observe_control.c`. Requests use size and version checks against `include/abi/logit_sched_observe.h`. User requests and exported records pass through the copy helpers in `c/kernel/sched/observe/observe_platform.c`.

## Capability checks and agent workers

The dispatcher classifies selected syscalls as `CAP_FS` or `CAP_NET`. A classified call requires the corresponding bit in the process's grant. Classification and enforcement live in `c/kernel/exec/syscall.c`.

Fork copies the grant, and execution retains it. `SYS_CAP_SPAWN` checks that the child's requested bits form a subset of the parent's grant. It also compares filesystem prefixes at a path component boundary, preventing a request for a similarly named sibling directory from passing the grant comparison. `c/kernel/exec/proc.c` implements this comparison; `c/kernel/exec/load/exec.c` creates the child.

Agent workers receive an additional syscall allowlist from `include/abi/agent_policy.h`. Worker creation reads an image snapshot, compares its application identity and image hash with the request, and loads that snapshot. This binds the comparison to the bytes supplied to the loader during launch.

The worker starts with a zero category grant and a transferred communication endpoint at descriptor 3, defined by `include/abi/aex_agent.h`. Its allowlist permits selected memory, timing, polling, identity, and lifecycle operations. Reads, writes, closure, and the listed descriptor operations require descriptor 3. Unlisted syscall numbers are refused. The interrupt path checks the same policy before signal return, which is handled ahead of the main dispatcher.

## Signing and trust

The package verifier in `c/crypto/trust/pkgsig.c` checks container fields, the payload's SHA-256 digest, an Ed25519 signature, and membership in the compiled signer set. The signature covers a hash of the header with a domain prefix. That header contains the payload length, digest, name, and signer key, binding the payload through its digest while keeping the signed input small.

`c/apps/coreutils/pkgverify.c` exposes verification and optional payload extraction. Extraction follows a successful verification result. `Makefile` links the verifier into the tool; signer keys come from `c/crypto/trust/pkgroots.inc`.

AEX versions 2 and 3 require a matching CRC-32 record over the ELF payload. An optional Ed25519 record produces one of four verdicts: absent, invalid, valid from an untrusted signer, or valid from a trusted signer. Parsing and verdict reporting live in `c/kernel/exec/load/aex.c`.

`c/crypto/trust/aexsig.c` derives the signed AEX manifest from the ELF payload length and SHA-256 digest. AEX and package signatures use different domain strings and consult the same signer roots. `tests/unit/aexsig_test.c` includes altered-payload, foreign-signer, and package-signature replay cases.

## Limitations

- `SECURITY.md` provides best-effort support for experimental use and warns against valuable secrets or hostile workloads. Production multi-user security, a verified boot chain, an exploit mitigation baseline, and formal verification are outside the project's scope. `TRANSPARENCY.md` states that an external audit of the cryptography is pending.
- Directory traversal has a metadata gap: `visit()` in `c/fs/vfs/vfs.c` consults the VFS RAM store for ancestor search checks, while operation checks can use LogitFS attributes. Persisted directory modes receive incomplete traversal enforcement.
- The credential model has one seat, a session lasting until shutdown, and 32 PID entries. Session establishment can succeed when the table has no slot for the caller. An absent account file takes console login to a root shell; an absent or empty store starts the desktop unauthenticated. Serial password masking occurs after echo, exposing password characters to serial recordings (`c/fs/vfs/vfs_cred.c`, `c/apps/coreutils/login.c`, `c/kernel/exec/fd/file.c`, `c/kernel/gui/wm.c`).
- The earlier rule that every nonempty account store locks the desktop has a developer configuration exception. With `HWHUD=1`, a store containing just the locked `dev` account starts the desktop unauthenticated under the initial root session. Provisioned accounts, keys, and the SSH enable flag persist on disk after the boot module is removed (`c/kernel/init/devssh.c`, `c/kernel/init/devssh.h`, `c/kernel/gui/wm.c`).
- Category capabilities provide partial confinement. Filesystem prefixes participate in grant comparison, while VFS resolution applies its own permission checks. Capability spawn inherits descriptors, and process, GUI, clipboard, and settings operations rely on their subsystem checks (`c/kernel/exec/syscall.c`, `c/kernel/exec/proc.c`, `c/kernel/exec/load/exec.c`).
- AEX signature verdicts are informational: all four permit loading subject to container and ELF checks. The signature covers the ELF payload manifest, so AEX header metadata sits outside the signed bytes. Bare ELF and version 1 AEX loading skip the container integrity record. The compiled signer set contains a development key with its private seed published in `tools/pkgroots/DEV-SIGNING-KEY.txt`, and `pkgverify --any` accepts valid signatures from arbitrary signers.
- The kernel runs with SMAP disabled. Some syscall paths dereference user pointers after range checking, including `SYS_CAP_QUERY` in `c/kernel/exec/proc.c`; a successful check represents a moment in time. ELF placement is deterministic, and anonymous mappings accept write and execute permission together, so system-wide W^X and ASLR require mechanisms beyond the loader checks (`c/boot/long.asm`, `c/boot/ap_trampoline.asm`, `c/kernel/exec/usercopy.c`, `c/kernel/exec/load/elf.c`, `c/kernel/mm/mmsys.c`).
- Drivers, filesystem parsers, and loaded modules execute with kernel privilege. Module loading accepts unsigned modules. Unresolved kernel exceptions in the generic exception path are fatal (`SECURITY.md`, `c/kernel/module/module.h`, `c/kernel/cpu/irq/interrupts.c`).
- The browser's origin and JavaScript sandbox fall short of hostile-content isolation; AetherScript exposes raw memory and direct syscalls. The RNG can fall back to weak timing inputs. The TLS client requires the RNG's hardware entropy gate before beginning a handshake (`TRANSPARENCY.md`, `c/kernel/core/rng.c`, `c/net/tls/tls.c`).

## Source map

| File | What it contains |
|---|---|
| `SECURITY.md`, `TRANSPARENCY.md` | Operational policy and security claim limits |
| `c/kernel/cpu/gdt.h`, `c/kernel/cpu/smp/percpu.c`, `c/boot/enter_user.asm` | Privilege descriptors, kernel entry stacks, and user-mode entry |
| `c/kernel/cpu/irq/idt.c`, `c/kernel/cpu/irq/interrupts.c` | Syscall gate, exception handling, and worker signal-return checks |
| `c/kernel/cpu/power/mce.c`, `c/kernel/cpu/power/hardware.c` | Machine-check decisions and emergency serial output |
| `c/boot/long.asm`, `c/boot/ap_trampoline.asm`, `c/kernel/cpu/prot.c` | CPU protections and state reporting |
| `c/kernel/mm/mm.h`, `c/kernel/mm/virt/vmm.c` | User address windows, address spaces, and page pinning |
| `c/kernel/mm/virt/mmguard.h`, `c/kernel/exec/usercopy.c` | Address-space guards and user-memory transfers |
| `c/kernel/mm/mmsys.c` | Mapping and protection syscall handling |
| `c/fs/vfs/vfs_cred.c` | Credentials, supplementary groups, and session identity |
| `c/fs/vfs/vfs.c`, `c/fs/vfs/vfs_meta.c`, `c/fs/vfs/vfs_meta.h` | Permission checks, path traversal, and metadata fallback |
| `c/fs/logitfs/logitfs.c` | Persistent mode and ownership attributes |
| `c/apps/coreutils/accounts.h`, `c/apps/coreutils/login.c`, `c/crypto/kdf/pbkdf2.c` | Account records, enrollment, and password verification |
| `c/apps/gui/greeter/greeter.c`, `c/kernel/gui/wm.c` | Graphical authentication, desktop startup, and application stacks |
| `Makefile`, `tests/uefi.mk`, `c/boot/efi/loader.c` | Build selection, verifier linkage, and developer key delivery |
| `c/kernel/init/kmain.c`, `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h` | Developer account provisioning and desktop startup exception |
| `c/apps/coreutils/boot_services.h`, `c/apps/services/ssh/sshd.c` | Service enable checks and SSH authentication |
| `c/kernel/exec/fd/file.c` | Serial console input and echo |
| `c/kernel/exec/syscall.c`, `c/kernel/exec/proc.c` | Capability enforcement, inheritance, and grant comparison |
| `c/kernel/sched/observe/observe_control.c`, `c/kernel/sched/observe/observe_platform.c`, `include/abi/logit_sched_observe.h` | Observation authorization, user transfers, and request ABI |
| `c/kernel/exec/load/exec.c`, `c/kernel/exec/load/interp.c`, `c/kernel/exec/load/elf.c` | Execution permissions, worker creation, and ELF validation |
| `include/abi/agent_policy.h`, `include/abi/aex_agent.h` | Worker allowlist and communication descriptor ABI |
| `c/kernel/exec/ptrace.c`, `c/kernel/module/modload.c`, `c/kernel/module/module.h` | Debugger and module authorization |
| `c/kernel/exec/load/aex.c`, `c/crypto/trust/aexsig.c` | AEX integrity checks and signature verdicts |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/pkgsig.h`, `c/crypto/trust/pkgroots.inc` | Package verification and compiled signer roots |
| `c/apps/coreutils/pkgverify.c`, `tools/pkgroots/DEV-SIGNING-KEY.txt` | Verification tool and published development key |
| `tests/unit/aexsig_test.c` | Signature verdict and replay cases |
| `c/kernel/core/rng.c`, `c/net/tls/tls.c` | Entropy handling and TLS entropy gate |

## See also

- [System calls](system-calls.md)
- [Memory model](memory-model.md)
- [Applications and AEX packages](applications-aex.md)
- [AI-native design](ai-native-design.md)
- [Virtual file system](../filesystems/vfs.md)
- [Trust roots and package signing](../crypto/trust-and-signing.md)
- [SSH](../networking/ssh.md)
- [Security policy and reporting](../../SECURITY.md)
