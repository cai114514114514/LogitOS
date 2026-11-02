# System services

LogitOS provides HTTP and HTTPS serving, SSH access, SFTP file transfer and local logging through user processes in `c/apps/services/`. Console login launches enabled network services before authentication. SSH runs shells, commands and SFTP with the authenticated account’s credentials. Syslogd collects application messages through a named local datagram socket.

## Programs and startup

| Program | Purpose | Startup |
|---|---|---|
| `/bin/httpd` | Static files and socket statistics over HTTP | Shell command or boot enable file |
| `/bin/httpsd` | HTTP serving through TLS | Shell command or boot enable file |
| `/bin/sshd` | Remote authentication, shells, commands and TCP forwarding | Shell command or boot enable file |
| `/bin/sftpd` | SFTP operations through standard input and output | SSH subsystem request |
| `/bin/syslogd` | Local log collection | Shell command |

`Makefile`, `tests/ssh.mk` and `tests/servers.mk` build and install these programs. HTTPS compiles `c/apps/services/http/httpd.c` with `HTTPD_TLS` and links the TLS engine through the process descriptor adapters in `c/apps/services/http/https_platform.c`.

The window manager launches `/bin/login` on the serial console in `c/kernel/gui/wm.c`. When invoked without arguments, login calls `boot_services()` before reading the account store or authenticating the console user. The launcher in `c/apps/coreutils/boot_services.h` checks these guest filesystem paths:

| Enable file | Program | Standard output and error |
|---|---|---|
| `/etc/httpd.enabled` | `/bin/httpd` | `/var/log/httpd.log` |
| `/etc/httpsd.enabled` | `/bin/httpsd` | `/var/log/httpsd.log` |
| `/etc/sshd.enabled` | `/bin/sshd` | `/var/log/sshd.log` |

The launcher requires root credentials and a regular enable file owned by root, with group and other write permissions cleared. File presence and metadata select the service. From a root guest shell, create the desired markers:

```sh
touch /etc/httpd.enabled
touch /etc/httpsd.enabled
touch /etc/sshd.enabled
```

At the next boot, login forks each selected service. Each child establishes root credentials, connects standard input to a pipe at EOF, and opens its log with truncation. This keeps service input away from the login prompt and establishes credentials before console authentication changes the desktop session. The launcher passes the executable name as its sole argument, selecting the defaults.

For immediate startup, `c/apps/coreutils/sh.c` supports background commands:

```sh
/bin/httpd &
/bin/httpsd &
/bin/sshd &
/bin/syslogd &
```

SSHD requires root privileges at startup.

## HTTP and HTTPS

HTTP accepts these positional arguments in `c/apps/services/http/httpd.c`:

| Argument | Default | Meaning |
|---|---|---|
| `port` | `8080` | IPv4 listening port |
| `docroot` | `/www` | Absolute document root |
| `max_requests` | `0` | Connections dispatched to workers before exit; zero serves continuously |

The listener binds to all local IPv4 addresses and requests a backlog of eight. The parent polls the listener, forks workers and reaps completed children. Forking gives each worker its own request buffers and HTTPS session state.

GET and HEAD requests map to regular files beneath the document root. Path handling in `c/apps/services/http/httpd_protocol.h` decodes percent escapes and rejects parent-directory components. Root requests and paths ending in a slash receive `index.html`. The server checks every filesystem path component for symbolic links before opening the file.

Files stream through an 8 KiB buffer. Responses include a content length and a MIME type selected from the filename extension. GET supports single byte ranges, including suffix ranges. The `/_stat` endpoint reports kernel socket counters, including backlog and connection-slot refusals. Request diagnostics go to standard error.

HTTPS uses these positional arguments:

| Argument | Default | Meaning |
|---|---|---|
| Port | `8443` | IPv4 listening port |
| Document root | `/www` | Absolute document root |
| Certificate name | `localhost` | Name used when generating a certificate |
| Identity prefix | `/etc/httpsd` | Prefix for identity and certificate-chain files |

`c/apps/services/http/https_transport.h` loads a raw 32-byte P-256 private key from the prefix plus `.key` and a DER certificate from the prefix plus `.der`. It derives the public key and checks it against the certificate. When both files are absent, it generates and persists a self-signed identity. Clients need to trust that certificate to authenticate the server.

Generated keys receive mode `0600`. Loading requires a regular key file owned by the service user with all group and other permissions cleared. Optional intermediates use the prefix plus `.chain1.der` and `.chain2.der`. Identity setup requires strong randomness; generating a certificate also requires a positive wall-clock time.

## SSH access and configuration

`c/apps/services/ssh/sshd.c` owns the listener, host identity, authentication and connection threads. Its positional arguments are:

| Argument | Default | Meaning |
|---|---|---|
| `port` | `22` | IPv4 listening port |
| `rekey_bytes` | `1073741824` | Byte-accounting threshold for server key exchange |
| `rekey_seconds` | `3600` | Elapsed time threshold for server key exchange |

The daemon binds to all local IPv4 addresses. Byte accounting adds the payload length plus 64 for each transmitted or received transport packet. A zero threshold disables its trigger. The daemon also handles key exchanges initiated by clients.

SSH reads credentials and policy from these guest filesystem locations:

| Location | Role |
|---|---|
| `/etc/ssh_host_ed25519_key` | Persistent server identity |
| `/etc/passwd` | Account names, password hashes, IDs, home directories and shells |
| The account home’s `.ssh` directory, file `authorized_keys` | Public keys authorized for that account |
| `/etc/sshd.forward.enabled` | Enable TCP forwarding |

At startup, SSH checks for strong session entropy and loads or creates an Ed25519 host key. The key uses the project’s 68-byte record format defined in `c/net/ssh/ssh_hostkey.h`. Creation sets root ownership and mode `0600`. Loading checks the file type, root ownership, permissions and record contents, then derives the public key from the seed to check consistency. The daemon prints its SHA-256 host-key fingerprint.

SSH and console login share account parsing and password verification through `c/apps/coreutils/accounts.h`. Root can enroll an account with `login -a NAME`, which prompts for a password and creates its home directory. The daemon accepts password authentication and public-key authentication using Ed25519, ECDSA P-256/P-384/P-521, or RSA SHA-2 signatures. Public-key verification lives in `c/net/ssh/ssh_pubkey.c`.

Authorized-key lookup constructs the filename from the account home. The file must be regular, owned by root or the account, and protected against group and other writes. `c/net/ssh/ssh_auth.c` matches its decoded key blobs against the client’s key.

After authentication, `c/apps/services/ssh/sshd_channels.h` forks children for shells, commands and SFTP. Each child clears supplementary groups, applies the account GID and UID, changes to its home directory, and sets `HOME`, `USER` and `PATH`. Remote commands execute through the account shell with `-c`.

PTY requests allocate a kernel terminal and apply terminal settings and window dimensions. Resize requests update those dimensions. Sessions using pipes carry standard output and standard error through their corresponding SSH streams. Each channel has an input queue and flow-control window, allowing other channels to progress while a command pauses its input.

TCP forwarding requires a regular, root-owned `/etc/sshd.forward.enabled` file protected against group and other writes. Direct forwarding accepts numeric IPv4 destinations or `localhost`, with destination ports from 1 through 65535. Remote forwarding binds loopback listeners requested as `localhost` or `127.0.0.1`; its requested listening port must be at least 1024, or zero to request allocation.

## Developer SSH provisioning

Hardware bring-up builds enable developer SSH provisioning through `HWHUD=1`. `Makefile` defines `LOGIT_HWHUD` for the kernel, and `tests/uefi.mk` supplies it when building the UEFI loader.

The loader reads a public-key file named DEVSSH.PUB from the LOGIT directory of the UEFI boot volume. These are names inside the boot image. The `--file SRC:ESP_PATH` option in `tools/mkesp.py` copies supplied files into that image. `c/boot/efi/loader.c` passes the key file to the kernel as the `devssh-authorized-keys` boot module.

After a successful root filesystem mount, `c/kernel/init/kmain.c` calls `devssh_provision_boot()`. The implementation in `c/kernel/init/devssh.c` validates Ed25519 public-key lines, prepares a `dev` account with `/home/dev` and `/bin/sh`, writes `/home/dev/.ssh/authorized_keys`, and creates `/etc/sshd.enabled` last. This ordering prepares the account and keys before enabling service startup through login.

The account’s password hash field is `!`, which `c/crypto/kdf/pbkdf2.c` rejects during password verification. Its home and `.ssh` directory receive mode `0700`; its authorized-key file belongs to the account and receives mode `0600`. Provisioning compares content and metadata before updating files. A changed valid module replaces the authorized-key contents.

In these builds, `c/kernel/gui/wm.c` uses `devssh_desktop_locked()` to choose the initial desktop state. An account store containing just the locked `dev` account permits the desktop to start without authentication. Other account rows keep it behind the greeter.

## SFTP file transfer

An SSH request for the `sftp` subsystem executes `/bin/sftpd` after applying the account credentials and changing to its home directory. Standard input and output carry SFTP packets through the SSH channel. This startup path is in `c/apps/services/ssh/sshd_channels.h`.

`c/apps/services/ssh/sftpd.c` implements SFTP version 3 and processes requests in order. Operations include file reading and writing, directory listing, metadata queries, truncation, ownership and permission changes by pathname, directory creation, removal and symbolic links. Advertised OpenSSH extensions provide replacement rename, file synchronization, hard links and transfer limits.

Relative paths resolve from the home directory inherited from SSH. Filesystem operations use the authenticated account’s permissions, including access to paths outside that home. Configuration follows the account record and filesystem ownership and modes.

## Local logging

`c/apps/services/syslog/syslogd.c` creates `/var` and `/var/log` as needed, then binds an AF_UNIX datagram socket at `/var/log/sock`. The daemon and C library obtain this socket path from `include/abi/logit_abi.h`, keeping senders and receiver on the same name.

Datagrams preserve message boundaries between unrelated processes. For each received record, syslogd prefixes monotonic seconds since boot, writes to standard output and appends to `/var/log/messages`. Monotonic timestamps keep boot-log ordering stable across wall-clock adjustments.

`c/apps/libc/src/syslog.c` provides `openlog()`, `syslog()` and related functions. It formats identity and priority information, sends a datagram and falls back to standard error when delivery is unavailable. Applications can set a priority mask and request a copy on standard error.

## Limitations

- Boot startup attempts each enabled service once per login invocation without arguments. It provides no restart supervision or dependency management. Syslogd uses manual startup. See `c/apps/coreutils/boot_services.h`.
- HTTP and HTTPS close each connection after one request and have four worker slots. Path checks and file opening occur in distinct operations, so document-root contents must remain trusted during filesystem mutations. See `c/apps/services/http/httpd.c`.
- SSH allows at most four connections, four channels per connection and two remote forwarding listeners per connection. Its watchdog uses a 30-second authentication deadline. Six failed authentication requests, including `none` probes, end a connection. Authorized-key files are limited to 16 KiB; lines with authorization options before the key type are skipped. See `c/apps/services/ssh/sshd.c`, `c/apps/services/ssh/sshd_channel_state.h` and `c/net/ssh/ssh_auth.c`.
- Developer provisioning accepts modules of at most 4096 bytes containing Ed25519 keys. Provisioned accounts, keys and the enable file persist after removal of the boot-volume key file. A conflicting existing `dev` account stops provisioning. The source records model and QEMU evidence, with X79-VG4 hardware verification pending. See `c/kernel/init/devssh.c` and `c/kernel/init/devssh.h`.
- SFTP has eight handles, a 64 KiB request-body limit and paths shorter than 256 bytes. Timestamp updates, exclusive creation, ownership or permission changes through file handles, and base-protocol rename are unsupported. Replacement rename uses the advertised OpenSSH extension. See `c/apps/services/ssh/sftpd.c`.
- Syslogd has no configuration file, facility routing, network forwarding or rotation. Failure to open the file sink leaves standard output as the destination. Its 1024-byte receive buffer truncates larger datagrams and discards the remainder. See `c/apps/services/syslog/syslogd.c` and `c/net/core/unix.c`.
- The project’s [security policy](../../SECURITY.md) excludes production security guarantees and use with valuable secrets or hostile workloads.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Service source mappings, disk installation and hardware build flag |
| `tests/ssh.mk` | SSH daemon build and package rules |
| `tests/servers.mk` | SFTP and HTTPS build and installation rules |
| `tests/uefi.mk` | Hardware build flag for the UEFI loader |
| `tools/mkesp.py` | Boot-image creation and extra-file placement |
| `c/boot/efi/loader.c` | Developer public-key boot-module loading |
| `c/kernel/init/kmain.c` | Provisioning after root filesystem mount |
| `c/kernel/init/devssh.c` | Key validation, account provisioning and desktop lock decision |
| `c/kernel/init/devssh.h` | Provisioning paths, limits and evidence scope |
| `c/kernel/gui/wm.c` | Desktop lock selection and console login launch |
| `c/apps/coreutils/login.c` | Boot service entry and account enrollment |
| `c/apps/coreutils/boot_services.h` | Enable-file checks, credentials and log redirection |
| `c/apps/coreutils/accounts.h` | Account format, parsing and password verification |
| `c/crypto/kdf/pbkdf2.c` | Password hash creation and verification |
| `c/apps/coreutils/sh.c` | Shell background commands |
| `c/apps/coreutils/touch.c` | Enable-file creation command |
| `c/apps/services/http/httpd.c` | HTTP listener, workers and responses |
| `c/apps/services/http/httpd_protocol.h` | Request paths, MIME types and ranges |
| `c/apps/services/http/https_transport.h` | HTTPS identity files and TLS transport |
| `c/apps/services/http/https_platform.c` | TLS adapters for process descriptors |
| `c/apps/services/ssh/sshd.c` | SSH identity, authentication and connection lifecycle |
| `c/apps/services/ssh/sshd_channels.h` | Child execution, PTYs, SFTP and forwarding |
| `c/apps/services/ssh/sshd_channel_state.h` | Channel state and resource limits |
| `c/apps/services/ssh/sftpd.c` | SFTP operations and extensions |
| `c/net/ssh/ssh_hostkey.h` | Persistent SSH host-key format |
| `c/net/ssh/ssh_auth.c` | Authentication messages and authorized-key matching |
| `c/net/ssh/ssh_pubkey.c` | Client public-key verification |
| `c/apps/services/syslog/syslogd.c` | Local log receiver and output sinks |
| `c/apps/libc/src/syslog.c` | Application logging interface |
| `c/net/core/unix.c` | Local datagram delivery and truncation |
| `include/abi/logit_abi.h` | Shared logging paths |

## See also

- [SSH](../networking/ssh.md)
- [HTTP](../networking/http.md)
- [Sockets](../networking/sockets.md)
- [Security model](../architecture/security-model.md)
- [The C library](libc.md)
- [Running on real hardware](../getting-started/real-hardware.md)
- [Applications and AEX packages](../architecture/applications-aex.md)