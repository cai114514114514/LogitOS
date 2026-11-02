# Sockets

LogitOS provides an asynchronous client broker for DNS, TCP and optional TLS, alongside descriptor sockets for TCP, UDP, UNIX communication and raw ICMP. The broker in `c/net/core/sock.c` advances connections through a pump. The descriptor layer in `c/net/core/lsock.c` connects socket backends to file operations, descriptor inheritance and polling.

## Interfaces and address families

| Interface | Identifier | Operations |
|---|---|---|
| Client broker | Handle owned by a process | Open by hostname, inspect progress, send, receive, obtain ALPN, close |
| Descriptor sockets | Descriptor referencing an `F_SOCK` file description | Create, bind, listen, accept, connect, read, write, shutdown, exchange datagrams |

The broker lets an application queue output while connection setup progresses. Descriptor sockets integrate with programs that duplicate descriptors, inherit them across a fork or pass them to code using file operations. The rationale and backend interface are described in `c/net/core/sock.h` and `c/net/core/lsock.h`.

`lsock_create()` accepts these native combinations:

| Family | Type | Protocol argument |
|---|---|---|
| `LOGIT_AF_UNIX` | `LOGIT_SOCK_STREAM`, `LOGIT_SOCK_DGRAM`, `LOGIT_SOCK_SEQPACKET` | Zero |
| `LOGIT_AF_INET` | `LOGIT_SOCK_STREAM`, `LOGIT_SOCK_DGRAM` | Zero |
| `LOGIT_AF_INET` | `LOGIT_SOCK_RAW` | `LOGIT_IPPROTO_ICMP` |

UNIX creation reaches its backend before the network availability check, allowing local communication on machines without a network interface. IPv4 creation requires `net_up()`, which records successful NIC initialisation in `c/net/core/net.c`.

NIC initialisation and address acquisition are different stages. The NIC becomes available before `c/net/core/netcfg.c` applies static settings or starts DHCP. The boot DHCP attempt shares a four-second deadline between waiting for link and exchanging messages. Acquisition continues through `dhcp_poll()` after boot and restarts when link returns. If the boot attempt ends without a lease, the hypervisor path applies the QEMU SLIRP fallback configuration; the bare metal path keeps the initial IPv4 address at zero.

## Addresses and syscall copying

The native address structures live in `include/abi/logit_abi.h`:

| Structure | Representation |
|---|---|
| `struct logit_sockaddr` | Eight bytes containing family, port and IPv4 address; port and address use host byte order |
| `struct logit_sockaddr_un` | Family followed by a 108-byte pathname field |
| `struct logit_dgram` | Buffer pointer, length, flags and IPv4 address fields for native datagram operations |

Bind and connect handlers in `c/kernel/exec/syscall.c` copy the family first, then copy the corresponding address structure. Sizing the copy this way permits an eight-byte IPv4 address at the end of a mapped page. UNIX paths receive a terminator and are resolved against the caller’s working directory before reaching the socket registry.

Payload operations use private kernel buffers. Sends copy user data into those buffers before backend I/O; receives copy results back after the backend returns. This keeps the memory used by a sleeping backend stable while another thread changes the process’s mappings.

The C wrappers in `c/apps/libc/src/socket.c` convert between the network byte order of `struct sockaddr_in` and the native ABI’s host byte order. They create IPv4 streams and all three UNIX socket types. Socket control errors pass through a shared errno mapping.

## Descriptor lifetime

`SYS_SOCKET` creates an `F_SOCK` file description and installs it in the caller’s descriptor table. Its `backing` pointer refers to a `struct lsock`, which holds the socket kind, backend identifier and control state. The descriptor backing table has 40 slots shared by the socket kinds (`c/net/core/lsock.c`).

Duplicated and inherited descriptors share the file description and its reference count. `proc_fd_acquire()` takes a reference while holding the descriptor table lock, keeping the backing alive through a concurrent close.

At the final reference, `file_close()` saves the backing pointer and detaches the file slot under its lock. It then passes the saved pointer to `lsock_file_release_backing()`, which closes the backend and returns the socket slot to the table. Using the saved pointer protects teardown from reuse of the file slot by another allocation (`c/kernel/exec/fd/file.c`).

Process exit closes descriptors through `proc_fd_close_all()` and releases broker handles through `sock_close_owner()` in `c/kernel/exec/proc.c`.

## IPv4 streams and datagrams

A TCP listener follows create, bind, listen and accept. Stream bind records the requested local address and port. `lsock_listen()` claims the listener through `tcp_listen_addr()`. Accept constructs a new file description with blocking I/O flags.

Outgoing streams use `lsock_connect_inet()`, which starts TCP through `tcp_connect_owned()` and checks progress between sleeps. TCP records ownership before transmitting the SYN. The ownership flag keeps a failed connection’s table slot associated with its socket until close, protecting a saved transport index from reuse (`c/net/transport/tcp.c`).

Reads consume buffered data and return zero after the peer’s end of stream has drained. Writes feed TCP’s send buffer and wait for capacity as acknowledgements arrive. With `O_NONBLOCK`, an operation that would wait returns `EAGAIN_RC`; a write can return the bytes already accepted. Read shutdown makes subsequent reads report EOF and wakes readiness waiters. Write shutdown queues TCP’s FIN.

For UDP, bind obtains a receive queue through `udp_bind()`. Native `SYS_SENDTO` and `SYS_RECVFROM` use `struct logit_dgram`; receive reports the sender’s IPv4 address and port. Descriptor reads also receive datagrams and discard the sender address. Transmission uses the native datagram operation (`c/net/core/lsock.c`, `c/net/transport/udp.c`).

The descriptor layer accepts `LOGIT_SO_RCVTIMEO` in milliseconds and `LOGIT_SO_REUSEADDR`. It also accepts `LOGIT_TCP_NODELAY` on connected TCP sockets. These native options use `LOGIT_SOL_SOCKET` and a value passed in a register.

## Asynchronous client connections

`sock_open()` allocates a handle owned by the caller and starts a DNS query. `sock_pump()` advances setup through resolution, IPv4 ARP preparation, TCP establishment and optional TLS negotiation. In `net_poll()`, `dns_poll()` precedes the broker pump, allowing a lookup completed during that pass to advance its connection immediately.

The broker stores up to eight destinations covering IPv4 and IPv6. A pending connection can start a candidate from the other family after a 250 ms head start. Connection failures can advance through further destinations (`c/net/core/sock.c`).

Each of the 16 broker slots has a 4096-byte transmit ring. `sock_send()` accepts as many bytes as fit, including during setup. `sock_poll_bits()` reports connection, readability, writability, EOF and error bits. Bits 8 through 15 carry the negated terminal error code. `sock_alpn()` returns the negotiated application protocol.

Closing a ready handle with queued output marks it shut. The pump retains it until the queue drains or the drain deadline expires. The owner’s access ends when the handle is marked shut. Other close paths release the resources immediately. Failure releases transport resources while retaining an open handle’s diagnostic state; failure during close releases the handle as well.

## UNIX sockets

UNIX sockets use the pathname registry and memory channels in `c/net/core/unix.c`. Bind checks the containing directory through `vfs_may_create()`, then records the binding’s UID, GID and mode, calculated as `0777 & ~umask`. Connect checks write permission using the owner, primary group and other permission bits, with access allowed for UID zero.

Streams and sequenced packets share connection establishment. Connect allocates a connection containing two channels and queues the server endpoint at the listener. The client can write before accept. The listener backlog is clamped between one and eight.

`lsock_socketpair()` creates two connected, unnamed endpoints. Allocation failures release the endpoints already created. The syscall installs both descriptors before copying their numbers to the caller and rolls them back if that copy fails (`c/net/core/lsock.c`, `c/kernel/exec/syscall.c`).

Each connected pair has one channel per direction. A channel contains a 4096-byte byte ring and 32 record-length entries:

| Socket form | Data behavior |
|---|---|
| Stream | Reads consume available bytes across write boundaries |
| Sequenced packet | Each write queues one record; each read consumes one record |
| Datagram socket pair | Exchanges records through the pair’s channels |
| Named datagram | Bind creates an inbox; connect records the destination pathname |

Record writes enqueue the complete record when capacity permits. A receive copies the portion that fits and discards the remaining bytes of that record. Named datagram writes resolve the destination pathname on each attempt, protecting senders from retaining a pointer to a recycled destination slot.

Connected endpoints track read shutdown, write shutdown and closure. Readers drain queued data before reporting EOF from peer write shutdown or closure. Writing after local write shutdown, toward a closed peer or toward a peer that shut down reading posts `SIGPIPE`. State changes wake waiting peers.

Connections capture endpoint process IDs and available agent identities. For an endpoint carrying an agent identity, read, write and poll validate its process generation and credentials. Connected reads and writes repeat validation after a wait (`c/net/core/unix.c`, `c/kernel/exec/proc.c`).

## Raw ICMP sockets

Raw creation requires UID zero, checked through `vfs_cred_get()`, and allocates an ICMP endpoint immediately (`c/net/core/lsock.c`).

The sender supplies the ICMP header, payload and checksum. `raw_icmp_send()` passes that message to `ip_send()`, which constructs the IPv4 header and selects the source address through the IP output path. Successful transmission returns the message’s byte count. `c/apps/coreutils/ping.c` uses this interface and constructs its own echo requests.

On receive, `icmp_input()` validates message length and checksum, then calls `raw_icmp_deliver()` before handling the ICMP type. Delivery attempts to queue a copy for every open raw endpoint. Receivers obtain the ICMP header and payload with the source IPv4 address and perform their own request matching.

`c/net/core/raw.h` defines eight endpoints, each with four queue entries of 1500 bytes. A full queue drops the newest message. Receive removes the oldest queued message and copies up to the caller’s capacity.

## Locking and readiness

Socket services use task ownership for operations that can wait. Packet processing uses the network spinlock.

| State or operation | Protection |
|---|---|
| Broker handles, pumping and teardown | `sock_owner` |
| Descriptor backing slot allocation and release | `lsock_slots` |
| Descriptor bind, listen, connect, options and shutdown | One task owner per backing slot |
| UNIX namespace, connections and queues | `unix_owner` |
| TCP state, UDP queues and raw receive queues | `net_lock()` |
| DHCP address publication and withdrawal | `net_lock()` |

Task owners use the sleeping mutex machinery in `c/drivers/core/io_domain.h`. Recursive ownership follows the task, allowing a waiting task to migrate between CPUs. Descriptor reads and writes use backend synchronization so both directions can progress.

UNIX waits capture a change sequence, release `unix_owner`, park and reacquire ownership before inspecting state. Releasing ownership gives peers access to produce data or free capacity. Named datagram polling uses a permanent aggregate wait queue for inbox data and destination capacity, avoiding a retained pointer to a destination slot that can be reused.

`net_lock()` disables local interrupts and acquires a spinlock shared across CPUs. It tracks recursion by CPU because receive processing can generate replies that re-enter transmission paths. Its documented lock order is network lock, wait queue lock, then scheduler lock. Allocation and sleeping operations belong outside these critical sections (`c/net/core/netlock.c`).

TCP data wait predicates inspect scalar state under the wait queue lock. This preserves the lock order by keeping network lock acquisition out of those predicates. Descriptor TCP reads and writes use 200 ms waits as a retry backstop; a configured receive timeout supplies the read wait duration.

DHCP lease publication and address withdrawal hold the network lock. A NAK answering an outstanding request clears the IPv4 configuration and the interface’s routes before discovery restarts. Descriptor bind reads the current address through `net_config_snapshot()`, which takes the same lock (`c/net/core/dhcp.c`, `c/net/core/net.h`).

Descriptor polling dispatches through `lsock_file_poll()`. TCP registers its listener queue or shared transport queue and checks readiness under the network lock. UNIX registers its queue and checks state under `unix_owner`. The poll core records a wake arriving during this scan before deciding whether to sleep (`c/kernel/exec/fd/kpoll.c`).

Receive processing runs through `net_rx_softirq()`, raised by NIC interrupts and the 10 ms network timer. `net_poll()` also drains receive rings. Packets delivered through these paths can wake TCP readers (`c/net/core/net.c`).

## Limitations

- Descriptor families are UNIX and IPv4; the broker handles IPv6 destinations. IPv4 descriptor connect rejects nonblocking operation and sockets with a prior bind.
- UDP and raw descriptors return `LPOLLNVAL` from polling. Blocking receives retry after 10 ms sleeps and apply the configured receive timeout.
- UNIX bindings live outside the VFS node namespace. Directory listings and metadata operations cannot inspect the binding, and unlink leaves it registered. Final socket release removes the name. Abstract UNIX names are rejected.
- UNIX accept ignores the native accept timeout. UNIX sockets reject socket options and native addressed datagram operations. Zero-length writes create no record; records larger than 4096 bytes are rejected.
- Raw sockets support IPv4 ICMP with the IP header supplied by the kernel. Bind and connect provide no address filtering. Receive delivery clips messages to 1500 bytes.
- The C library creates IPv4 streams and UNIX sockets. Its `getpeername()` and `getsockopt()` return errors. Addressed `sendto()` is unsupported, and `recvfrom()` reports a zero address length. Send and receive flags other than `MSG_NOSIGNAL` are rejected. For sends, that flag temporarily changes the process’s signal disposition and can race another thread.
- `LOGIT_SO_REUSEADDR` records the request without changing binding or listener conflict checks.

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_abi.h` | Native socket calls, address structures, options and result codes |
| `c/net/core/sock.h` | Broker interface and design rationale |
| `c/net/core/sock.c` | Broker ownership, setup, transmit queues and cleanup |
| `c/net/core/lsock.h` | Descriptor backend interface and design rationale |
| `c/net/core/lsock.c` | Family dispatch, descriptor state, I/O, options and polling |
| `c/net/core/unix.c` | UNIX names, permissions, channels, identity checks and waits |
| `c/net/core/raw.h` | Raw ICMP interface and queue capacities |
| `c/net/core/raw.c` | Raw endpoint allocation, transmission and receive delivery |
| `c/net/core/net.c` | NIC availability, receive processing, timers and pump ordering |
| `c/net/core/netcfg.c` | Static settings, boot acquisition and fallback configuration |
| `c/net/core/dhcp.c` | Boot deadline, background acquisition and address updates |
| `c/net/core/net.h` | Network lock interface and configuration snapshot |
| `c/net/core/netlock.c` | Recursive network spinlock and lock order |
| `c/net/transport/tcp.c` | TCP ownership, listeners, buffers, shutdown and readiness |
| `c/net/transport/udp.c` | Bound UDP ports and datagram queues |
| `c/net/ip/icmp.c` | ICMP validation and raw delivery hook |
| `c/net/ip/ip.c` | IPv4 header construction and output routing |
| `c/drivers/core/io_domain.h` | Task ownership through sleeping mutexes |
| `c/drivers/core/io_lock.h` | Short lock used for descriptor backing slot allocation |
| `c/kernel/exec/syscall.c` | Socket dispatch, address resolution and user memory copying |
| `c/kernel/exec/fd/file.c` | Socket file dispatch and final reference teardown |
| `c/kernel/exec/fd/kpoll.c` | Readiness registration and sleeping |
| `c/kernel/exec/proc.c` | Descriptor references, exit cleanup and agent identity helpers |
| `c/fs/vfs/vfs.c` | Directory permission check used by UNIX bind |
| `c/fs/vfs/vfs_cred.c` | Credential lookup |
| `c/apps/libc/src/socket.c` | C wrappers, byte order conversion and errno mapping |
| `c/apps/coreutils/ping.c` | Native raw ICMP consumer |

## See also

- [Network stack](../architecture/network-stack.md)
- [TCP and UDP](tcp-udp.md)
- [DNS](dns.md)
- [TLS and certificates](tls.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [Synchronisation](../kernel/synchronization.md)
- [The C library](../userland/libc.md)