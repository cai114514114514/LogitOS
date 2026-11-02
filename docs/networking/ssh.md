# SSH

LogitOS implements an SSH-2 server for remote shells, command execution, SFTP and TCP forwarding. Protocol code in `c/net/ssh/` provides wire encoding, key exchange, packet protection and authentication messages. The userland daemon in `c/apps/services/ssh/sshd.c` connects that code to sockets, accounts and child processes. SFTP requests run in the authenticated child implemented by `c/apps/services/ssh/sftpd.c`.

## Implementation structure

The protocol layer works with byte buffers and I/O callbacks, allowing host tests and the guest daemon to use the same implementation. The callback contract in `c/net/ssh/ssh_packet.h` requires complete transfers. Socket adapters in `c/apps/services/ssh/sshd.c` accumulate partial reads and writes and retry interrupted operations.

Each connection exchanges identification strings, establishes transport keys, accepts the `ssh-userauth` service and authenticates an account. During authenticated operation, the connection reader handles transport messages and channel requests, while a pump thread moves data between channels and child descriptors. Cleanup closes forwarding listeners and joins the pump; the accept loop joins the connection worker before reusing its stack and slot.

Channel state and resource budgets live in `c/apps/services/ssh/sshd_channel_state.h`. Execution, PTYs, forwarding and data relay are implemented in `c/apps/services/ssh/sshd_channels.h`.

## Key exchange and packet protection

The server identifies itself as `SSH-2.0-LogitOS_1.0`. Its transport offers are defined in `c/net/ssh/ssh_kex.h`:

| Purpose | Advertised algorithms |
|---|---|
| Key exchange | `curve25519-sha256`, `curve25519-sha256@libssh.org` |
| Host signature | `ssh-ed25519` |
| Encryption, both directions | `aes128-ctr` |
| MAC, both directions | `hmac-sha2-256` |
| Compression | `none` |

For key exchange, host signatures, ciphers and MACs, negotiation chooses the first shared algorithm in the client’s preference order. `c/net/ssh/ssh_wire.c` implements selection, and `c/net/ssh/ssh_kex.c` applies it to the six negotiated lists. The design rationale in `c/net/ssh/ssh.h` connects these choices to existing cryptographic primitives and the transport’s cipher-plus-MAC packet format.

The server generates an ephemeral X25519 key pair and computes a shared secret from the client’s public value. An all-zero result fails the exchange. The SHA-256 exchange hash covers both identification strings, both complete key-exchange proposals, the host public-key blob, both ephemeral public values and the shared secret. The server signs this hash with its Ed25519 host key.

Key derivation produces an IV, encryption key and MAC key for each direction. The first exchange hash becomes the session identifier used by subsequent exchanges and public-key authentication. Outgoing protection switches after the server sends `SSH_MSG_NEWKEYS`; incoming protection switches after it receives the client’s `SSH_MSG_NEWKEYS`.

In `c/net/ssh/ssh_packet.c`, HMAC covers the sequence number followed by the plaintext packet. AES-CTR encrypts the packet from its length field through its padding, and the MAC follows the ciphertext. Each direction maintains its own sequence number and running AES counter. Key activation replaces keys and the counter while preserving the sequence number. Receive processing checks packet framing, verifies the MAC and validates padding before returning a payload.

## Host identity

The guest stores its host identity at `/etc/ssh_host_ed25519_key`. This is a 68-byte project record containing the four-byte magic value `LKH1`, a 32-byte private seed and a 32-byte public key. Encoding and SHA-256 fingerprint formatting are implemented in `c/net/ssh/ssh_hostkey.c`.

Startup requires strong session entropy even when a host identity already exists, because each key exchange generates fresh ephemeral key material. For an existing identity, `c/apps/services/ssh/sshd.c` checks the record length and magic, requires a regular file owned by root, and requires group and other permission bits to be clear. It derives the public key from the seed and compares it with the stored value.

For a new identity, the daemon applies creation mask `0077`, writes the record, and sets ownership to root and mode to `0600`. Startup prints the host public-key fingerprint for administrators to compare with the identity presented to clients.

## Authentication

Password authentication reads the guest account store at `/etc/passwd`. The shared parser in `c/apps/coreutils/accounts.h` supplies the account name, password hash, UID, GID, home directory and shell. Verification uses the PBKDF2-HMAC-SHA256 record implementation in `c/crypto/kdf/pbkdf2.c`; each stored record carries its iteration count.

Public-key authentication reads `.ssh/authorized_keys` beneath the account’s home directory. The daemon requires a regular file owned by root or the account, with group and other write permissions clear. `c/net/ssh/ssh_auth.c` compares the offered key blob with decoded authorization entries. Its parser accepts leading whitespace, spaces or tabs between fields, trailing comments and CRLF line endings.

Client signature algorithms are defined in `c/net/ssh/ssh_pubkey.h`:

| Key family | Signature algorithms |
|---|---|
| Ed25519 | `ssh-ed25519` |
| NIST ECDSA | `ecdsa-sha2-nistp256`, `ecdsa-sha2-nistp384`, `ecdsa-sha2-nistp521` |
| RSA | `rsa-sha2-256`, `rsa-sha2-512` |

RSA uses the `ssh-rsa` public-key blob encoding with SHA-2 signatures. The parser accepts moduli from 2048 through 4096 bits and public exponents up to 64 bits. Parsing and verification are implemented in `c/net/ssh/ssh_pubkey.c`.

An authorized unsigned key probe receives an acknowledgment. Authentication completes after verification of a signature covering the session identifier and authentication request fields, including the username, requested service, method, algorithm and key blob. When the client offers `ext-info-c`, the server sends `server-sig-algs` after the initial key exchange.

Failed authentication requests count toward six attempts per connection, including the `none` discovery method. While attempts remain, the daemon sends a failure response and waits 300 ms. On the sixth failure it disconnects. A watchdog checks once per second for unauthenticated connections older than 30 seconds and shuts down their reads so their handlers can clean up.

## Channels and command execution

Each channel has a 64 KiB input queue and flow-control state. The reader queues incoming data, and the pump writes it through nonblocking descriptors. This keeps a child with full input from blocking the reader’s handling of other channels or key exchanges.

Receive-window credit returns as queued bytes reach the child or forwarding destination. Output respects the peer’s window and maximum packet size. For sessions using pipes, stdout travels as channel data and stderr as extended channel data; both consume the same window. The advertised data maximum reserves 13 bytes beneath the transport payload limit, accommodating extended-data framing.

Session requests are handled in `c/apps/services/ssh/sshd_channels.h`:

| Request | Behavior |
|---|---|
| `shell` | Starts the account’s shell. |
| `exec` | Invokes the account’s shell with `-c` and the supplied command. |
| `pty-req` | Allocates a kernel PTY and applies supported terminal modes and dimensions. |
| `window-change` | Updates dimensions on a running PTY channel. |
| `subsystem` with `sftp` | Starts `/bin/sftpd` over pipes on a channel without a PTY. |

The child connects its standard descriptors and closes inherited descriptors above them. Before execution, it clears supplementary groups, sets the account’s GID and UID, and changes to the account’s home directory. Its environment contains `HOME`, `USER`, `PATH=/bin` and, for a PTY, `TERM`.

PTY sessions connect all three standard streams to the slave terminal. The pump consumes terminal signal events and forwards them to the immediate child PID. On completion, it drains output, reaps the child and sends exit status, EOF and channel close. Increasing local channel identifiers protect reused slots from late messages addressed to retired channels.

## Rekeying and forwarding

The server accepts client-initiated rekeying and checks automatic triggers during authenticated channel processing. Defaults are 1 GiB of counted transport traffic or one hour. Traffic accounting charges each received or sent payload plus 64 bytes. The invocation syntax in `c/apps/services/ssh/sshd.c` is:

```text
sshd [port] [rekey_bytes] [rekey_seconds]
```

A zero byte or time argument disables that automatic trigger.

During an exchange, application sends wait for the new keys. A server proposal can cross channel packets already in flight. While waiting for the client’s proposal, the reader dispatches those packets into channel state and queues replies until the exchange completes. The original session identifier continues to participate in key derivation.

Forwarding requires the guest flag `/etc/sshd.forward.enabled` to be a regular file owned by root with group and other write permissions clear. Direct forwarding opens an IPv4 connection to the requested destination. Remote forwarding binds a guest loopback listener and opens `forwarded-tcpip` channels for accepted connections. Cancellation closes the listener matching the requested address and port.

Forwarding uses the same channel queues and flow-control machinery as command sessions. On incoming channel EOF, the relay shuts down the destination socket’s write direction and continues reading its response.

## SFTP server

`c/apps/services/ssh/sftpd.c` implements SFTP version 3 over standard input and output. The SSH daemon launches it with the authenticated account’s credentials and home directory. File operations use that account’s VFS permissions. Relative paths resolve from the initial working directory; absolute paths access the guest filesystem under the same credentials.

The dispatcher supports file creation, offset reads and writes, directory listing, metadata queries, truncation, pathname ownership and permission changes, directory creation and removal, file removal and symbolic links. Offset-based transfers support resuming a partial download.

The server advertises these OpenSSH extensions:

| Extension | Operation |
|---|---|
| `posix-rename@openssh.com` | Rename with replacement through the filesystem rename operation. |
| `fsync@openssh.com` | Flush an open file. |
| `hardlink@openssh.com` | Create a hard link. |
| `limits@openssh.com` | Report packet, transfer and handle limits. |

Requests execute in arrival order, and replies preserve request identifiers, allowing clients to pipeline requests. File and directory handles use generated four-byte tokens. `REALPATH` walks path components and symbolic links with a bounded restart count. Symbolic-link creation preserves relative target text so the filesystem resolves it from the link’s parent directory.

## Starting the services

Packaging rules in `Makefile`, `tests/ssh.mk` and `tests/servers.mk` install `/bin/sshd` and `/bin/sftpd` in the guest image. The SSH daemon requires root credentials at startup, defaults to port 22 and binds the IPv4 wildcard address.

For startup through console login, `/etc/sshd.enabled` must be a regular file owned by root with group and other write permissions clear. The no-argument login path in `c/apps/coreutils/login.c` invokes the service launcher before console authentication. The launcher in `c/apps/coreutils/boot_services.h` supplies an EOF input stream and redirects daemon output to `/var/log/sshd.log`.

## Test coverage

The following targets define protocol and interoperability checks. They were inspected for this review and were not run.

| Target | Checks defined in the tree |
|---|---|
| `test-ssh` | Wire encoding, packet protection, exchange vectors and session-identifier key derivation. |
| `test-ssh-pubkey` | Client signature algorithms, with a prerequisite control that removes the additional key families. |
| `test-sftpd` | OpenSSH transfers, metadata, links, resume and advertised limits through host descriptors; prerequisite controls disable writes and links. |
| `test-servers-os` | Guest authentication, command streams, PTYs, multiplexing, SFTP, forwarding, rekeying and persistence across rebuild and reboot. |
| `test-ssh-attack-host` | Authorization parsing, hostile exchange vectors and parser fuzzing, including a prerequisite sanitizer control. |
| `test-ssh-tamper-os` | OpenSSH connections through a proxy that alters exchange or encrypted packets, injects a transport message or drops the key transition. |

Guest assertions are implemented in `tests/boot/run-servers.py`, `tests/boot/ssh_keys.py` and `tests/boot/ssh_extended.py`. The multiplexing control builds a daemon with one channel and requires the overlapping-command assertion to fail. Its target is a prerequisite of `test-servers-os`.

## Limitations

- Configured upper bounds are four connections, four channels per connection and two remote forwarding listeners per connection. Descriptor availability can impose lower practical limits.
- Each SFTP process has eight handles, a 65,536-byte request-body limit and a 255-byte path limit. Its limits extension reports a 65,540-byte framed packet maximum, 65,520 bytes of read data and 65,511 bytes of write data.
- Strict key exchange is unadvertised. `c/net/ssh/ssh_kex.c` negotiates six algorithm lists; compression lists, language lists, the guessed-packet flag and the reserved field remain unparsed.
- Authorization entries with leading options are skipped because their restrictions are unsupported. Password-change requests are rejected. The authentication loop parses the requested service without checking it against `ssh-connection`.
- Direct forwarding accepts numeric IPv4 addresses and `localhost`. Remote listeners accept `localhost` or `127.0.0.1`, with ports of at least 1024 or zero for allocation.
- SFTP rejects timestamp setters, exclusive creation, base-protocol rename, and ownership or permission changes through a file handle. Replacement rename uses the advertised POSIX extension.
- Earlier comments in `c/apps/services/ssh/sshd.c` describe process groups as absent. The correction in `include/abi/pty.h` records session and process-group support. SSH still forwards PTY signals to its immediate child; terminal-generated group signals, background-read stops and orphan-group handling remain absent.
- After the startup entropy check, a failed random draw makes the daemon’s callback fill the requested buffer with `0xff` and return it to protocol callers.
- `TRANSPARENCY.md` classifies the network and cryptographic implementations as educational project code without certification or an independent production security audit.

## See also

- [Sockets](sockets.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [System services](../userland/services.md)
- [Security model](../architecture/security-model.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/net/ssh/ssh.h` | Protocol constants, transport state and algorithm rationale. |
| `c/net/ssh/ssh_wire.c` | Wire encoding, parsing and algorithm selection. |
| `c/net/ssh/ssh_kex.h`, `c/net/ssh/ssh_kex.c` | Transport offers, X25519 exchange, host signatures and key derivation. |
| `c/net/ssh/ssh_packet.h`, `c/net/ssh/ssh_packet.c` | I/O callback contract, framing, AES counters and HMAC verification. |
| `c/net/ssh/ssh_hostkey.c` | Host-key records and fingerprints. |
| `c/net/ssh/ssh_auth.c` | Authentication parsing, authorization matching and signed request data. |
| `c/net/ssh/ssh_pubkey.h`, `c/net/ssh/ssh_pubkey.c` | Client signature algorithms, key validation and verification. |
| `c/net/ssh/ssh_conn.c` | Channel message encoding and parsing. |
| `c/apps/services/ssh/sshd.c` | Listener, host identity, authentication, rekeying and watchdog. |
| `c/apps/services/ssh/sshd_channel_state.h` | Channel state, queues and resource budgets. |
| `c/apps/services/ssh/sshd_channels.h` | Child execution, PTYs, multiplexing and forwarding. |
| `c/apps/services/ssh/sftpd.c` | SFTP dispatcher, handles and filesystem extensions. |
| `c/apps/coreutils/accounts.h`, `c/crypto/kdf/pbkdf2.c` | Account parsing and password verification. |
| `c/apps/coreutils/login.c`, `c/apps/coreutils/boot_services.h` | Enabled-service startup and log redirection. |
| `include/abi/pty.h`, `c/kernel/exec/fd/pty.c` | PTY interface, terminal modes, signals and session controls. |
| `c/kernel/exec/fd/file.c` | Permission checks when opening filesystem descriptors. |
| `Makefile`, `tests/ssh.mk`, `tests/servers.mk` | Packaging, build rules, interoperability targets and controls. |
| `tests/ssh_attack.mk` | Adversarial test targets and control prerequisites. |
| `tests/boot/run-servers.py`, `tests/boot/ssh_keys.py`, `tests/boot/ssh_extended.py` | Guest OpenSSH and SFTP assertions. |
| `tests/unit/sftpd_test.py` | Host SFTP interoperability and advertised-limit assertions. |
| `tests/boot/run-ssh-tamper-test.sh`, `tests/boot/ssh_tamper_proxy.py` | Transport tampering scenarios and assertions. |
| `TRANSPARENCY.md` | Project security and validation claim rules. |