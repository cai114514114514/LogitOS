# TCP and UDP

LogitOS implements TCP streams and IPv4 UDP datagrams in `c/net/transport/`. TCP provides active and passive opens, receive reassembly, flow control, Reno congestion control, NewReno recovery, selective acknowledgements, and transport timers. UDP provides port bindings, datagram queues, checksums, and ICMP error reporting. Kernel clients and socket layers use these transports.

## Position in the stack

IPv4 dispatches TCP payloads through `tcp_input_v4()` and UDP payloads through `udp_input()` in `c/net/ip/ip.c`. IPv6 passes TCP payloads to `tcp_input_af()` in `c/net/ip/ip6.c`. TCP carries addresses in `struct tcp_addr`; its checksum, source selection, and output helpers select the IP family.

The descriptor layer in `c/net/core/lsock.c` maps stream operations to TCP and datagram operations to UDP. The asynchronous layer in `c/net/core/sock.c` advances connection setup and queued writes through `sock_pump()`. DNS uses UDP with TCP fallback in `c/net/dns/dns.c`; DHCP uses UDP in `c/net/core/dhcp.c`.

Earlier descriptions need a correction beside them: `TRANSPARENCY.md` describes a TCP sender with a “single outstanding segment,” and the opening comment in `c/net/transport/tcp.h` says “No listen/accept.” The current implementation has a windowed sender and passive opens. The same header declares the listener APIs and describes server support farther down.

## Connection storage and admission

`struct tcp_conn` in `c/net/transport/tcp.c` holds addresses, sequence positions, negotiated options, congestion state, timers, and fixed send and receive rings.

| Resource | Capacity |
|---|---:|
| Connection table, `NCONN` | 32 connections |
| Receive ring, `RXBUF` | 128 KiB per connection |
| Send ring, `SNDBUF` | 32 KiB per connection |
| Receive reassembly set, `NOOO` | 16 intervals per connection |
| Sender SACK set, `NSACK` | 8 intervals per connection |
| Listener table, `NLISTEN` | 4 listeners |
| Accept queue, `TCP_BACKLOG` | Up to 8 queued connections per listener |
| Client reserve, `CLIENT_RESERVE` | 8 free slots protected from passive allocation |

Static rings reserve connection storage before a handshake begins. The comment beside `NCONN` explains the choice: allocating rings during connection setup would introduce heap allocation failures into that path. The receive ring also supplies capacity beyond the 65,535-byte window field, making window scaling useful.

A listening port occupies a smaller `struct tcp_listener`, containing its binding, process owner, accept queue, and wait queue. On an incoming SYN, TCP checks the listener backlog and requires more than `CLIENT_RESERVE` free connection slots before allocating a connection. It checks queue capacity again when enqueueing the handshake result. Capacity refusals send resets and increment counters exposed through `tcp_server_stats()`.

## TCP state machine

The connection states in `c/net/transport/tcp.c` are `CLOSED`, `SYN_SENT`, `SYN_RCVD`, `ESTABLISHED`, `FIN_WAIT`, `CLOSE_WAIT`, `LAST_ACK`, `CLOSING`, and `TIME_WAIT`. Listening ports occupy the listener table.

| State or operation | Event | Result |
|---|---|---|
| Active open | Allocate a connection and send SYN | `SYN_SENT` |
| `SYN_SENT` | Accept SYN-ACK acknowledging the local SYN | Negotiate options, send ACK, enter `ESTABLISHED` |
| Listener | Admit an incoming SYN | Allocate `SYN_RCVD` connection and send SYN-ACK |
| `SYN_RCVD` | Accept the handshake ACK and enqueue successfully | `ESTABLISHED`, available to accept |
| `ESTABLISHED` | Send local FIN after queued data | `FIN_WAIT` |
| `ESTABLISHED` | Receive an in-order peer FIN | Send ACK and enter `CLOSE_WAIT` |
| `CLOSE_WAIT` | Send local FIN | `LAST_ACK` |
| `FIN_WAIT` | Receive peer FIN while local FIN remains unacknowledged | `CLOSING` |
| `FIN_WAIT` | Receive peer FIN after local FIN acknowledgement | `TIME_WAIT` |
| `CLOSING` | Receive acknowledgement of local FIN | `TIME_WAIT` |
| `LAST_ACK` | Receive acknowledgement of local FIN | `CLOSED` |
| `TIME_WAIT` | Retention timer expires | `CLOSED` |

`FIN_WAIT` covers both the period before the local FIN is acknowledged and the period awaiting the peer’s FIN. A single incoming segment can acknowledge the local FIN and carry the peer’s FIN.

The passive handshake path continues into established data processing after enqueueing the connection, allowing the final handshake ACK to carry the first request. `tcp_accept()` takes the oldest queued connection immediately. `tcp_accept_wait()` parks on the listener wait queue until a connection arrives, the listener closes, or the deadline expires.

`tcp_shutdown_write()` queues a FIN while preserving application ownership, allowing the caller to receive the peer’s remaining data. `tcp_close()` releases application ownership. In `ESTABLISHED` or `CLOSE_WAIT`, it queues the FIN behind pending data; in other states, it releases the slot.

Accepted connections and active opens created through `tcp_connect_owned()` or `tcp_connect_owned_addr()` retain their slots after protocol closure until the application releases them. This ownership keeps an integer connection ID valid through socket cleanup.

For established traffic, `tcp_input_af()` validates header length, checksum, options, and receive sequence acceptability. A reset at the next expected receive sequence closes the connection. Another reset inside the receive window receives a challenge ACK.

## Sequence space and buffering

TCP tracks four send positions in `struct tcp_conn`:

| Field | Meaning |
|---|---|
| `snd_una` | Oldest unacknowledged sequence |
| `snd_nxt` | Next sequence to transmit |
| `snd_max` | Highest transmitted sequence plus one |
| `snd_end` | One past the application data queued in the send ring |

Retransmission timeout handling rewinds `snd_nxt` to `snd_una`, while `snd_max` preserves the acknowledgement ceiling. A zero window probe records its transmitted byte in `snd_max`, allowing an acknowledgement of that byte to advance the connection.

Application data occupies the send ring until cumulative acknowledgement advances `snd_una`. `tcp_send_nb()` copies as much as fits and attempts output. Its return value counts bytes accepted into the ring.

Receive bytes occupy ring positions derived from their sequence numbers. `rx_data()` trims duplicate prefixes and clips data to the receive window. `reassemble()` merges intervals and advances the contiguous receive position when a hole fills. Bytes stored beyond that hole then become readable in place.

The receive window follows available ring space and the previously advertised right edge. Receive window avoidance suppresses small openings while preserving that edge. After an application read, `tcp_recv()` sends a window update in `ESTABLISHED` or `FIN_WAIT` when the current window exceeds the last advertised window by at least one quarter of the ring. The `FIN_WAIT` case lets responses continue after a local half-close.

For data received in `ESTABLISHED` or `FIN_WAIT`, TCP sends an immediate ACK when reassembly still contains a hole, two segments carrying data have arrived since the previous ACK, or the receive window reaches zero. Otherwise, it sets a delayed ACK deadline 200 ms ahead. Sending a segment carrying an ACK clears the pending acknowledgement.

## Options and output policy

Active opens offer maximum segment size (MSS), window scaling, selective acknowledgement (SACK) permission, and timestamps. Passive opens advertise MSS and offer scaling, SACK permission, and timestamps according to the incoming SYN. SYN and SYN-ACK window fields carry unscaled values; subsequent segments apply the negotiated shifts.

`eff_mss()` starts with the peer MSS, clamps it to the local 1,460-byte ceiling and path estimate, then subtracts outgoing option bytes. That subtraction includes timestamps and SACK blocks carried with data. A peer that omits MSS gets a default peer MSS of 536 bytes.

The receiver constructs SACK blocks from its reassembly intervals, reporting the most recently touched interval first. The sender stores peer SACK intervals and uses the nearest interval starting above `snd_una` to bound retransmission of the first hole.

Negotiated timestamps supply round-trip time (RTT) samples from echoed timestamps on acknowledgements that advance the cumulative ACK. Protection Against Wrapped Sequences (PAWS) compares incoming timestamps with the stored recent timestamp and acknowledges segments rejected as old. The fallback RTT path times one transmission and cancels the sample when retransmission makes its origin ambiguous.

`output_one()` admits data through the smaller of the congestion window and peer window. Nagle holds a short segment while transmitted data remains unacknowledged. A full segment, `tcp_set_nodelay()`, or the final data preceding a queued FIN can release that restriction. Sender window avoidance can still hold a short segment when a small peer window constrains it.

## Congestion control and loss recovery

The congestion controller in `c/net/transport/tcp.c` measures windows in bytes. After either handshake completes, `initial_cwnd()` selects ten effective MSS units, capped at 14,600 bytes and bounded below by two effective MSS units. The initial slow start threshold is `0xffffffff`.

| Event | Congestion response |
|---|---|
| Advancing ACK during slow start | Add the smaller of newly acknowledged bytes and one effective MSS |
| Advancing ACK during congestion avoidance | Accumulate acknowledged bytes; add one effective MSS for each congestion window’s worth |
| Third counted duplicate ACK outside recovery | Set the threshold to half the flight size, with a floor of two effective MSS units |
| Entry to fast recovery | Record `snd_max` as the recovery point, set the window to the threshold plus three effective MSS units, and retransmit the first hole |
| Counted duplicate ACK beyond the third during recovery | Add one effective MSS |
| Partial ACK during recovery | Deflate for acknowledged bytes, add one effective MSS, and retransmit the new head |
| ACK reaches the recovery point | Leave recovery and set the window to the threshold |
| Retransmission timeout outside persist handling | Apply the loss threshold, reduce the window to one effective MSS, and clear recovery and SACK tracking |

The duplicate ACK counter advances when an accepted ACK carries no payload, SYN, or FIN, transmitted sequence space remains outstanding, and the cumulative ACK makes no progress. The peer window must remain equal to its previous value, or processing SACK blocks must change the number of stored SACK intervals. This describes the predicate implemented in `tcp_input_af()`.

NewReno keeps recovery active through partial acknowledgements until the cumulative ACK reaches the recorded recovery point. A partial ACK subtracts acknowledged bytes from the congestion window when possible. If those bytes consume the window, the calculation sets it to one effective MSS before adding another.

Congestion avoidance uses a byte accumulator so integer division cannot discard small increments. `cc_on_ack()` caps its resulting window at four times `SNDBUF`.

## Retransmission and timers

After network initialisation, a periodic 10 ms kernel timer in `c/net/core/net.c` marks a TCP timer pass as due and raises `SOFTIRQ_NET`. The softirq handler drains received packets before running the timer pass, allowing an arriving ACK to cancel a retransmission. `net_poll()` can also discharge a due pass.

The periodic softirq drains NIC receive queues during interrupt silence. `net_rx_poll_active()` reports polling when no NIC interrupt route exists, or when routed interrupts have been silent for more than 100 ms and at least four unannounced frames have been counted. `net_rx_schedule()` resets that count whenever a NIC interrupt arrives.

`tcp_poll()` walks the connection table for retransmission, delayed ACKs, persist probes, queued output, and close timers. TCP deadlines use `timer_ticks()`; `c/drivers/timer/pit.h` defines `TIMER_HZ` as 100.

`rtt_sample()` initialises smoothed RTT from the first sample and RTT variation to half that sample. Later samples update smoothed RTT with weight 1/8 and variation with weight 1/4. `base_rto()` computes the retransmission timeout (RTO) as smoothed RTT plus the larger of one tick and four times the variation. The initial timeout is one second; subsequent estimates are clamped between 100 ms and 60 seconds.

An ACK advancing `snd_una` restarts the connection’s retransmission timer while transmitted sequence space remains outstanding and stops it when everything transmitted is acknowledged. Timeout handling doubles the RTO up to 60 seconds. A passive handshake receives five timeout retransmissions and closes on the next expiry. Other connections receive eight retransmissions without acknowledgement progress, with persist handling exempting them from that closure rule.

When the peer window is zero and unsent data remains in `ESTABLISHED` or `CLOSE_WAIT`, persist handling sends one-byte probes. Its interval starts from the base RTO and grows with bounded exponential backoff, capped at 60 seconds. An accepted update opening the window resumes output.

IPv4 ICMP errors reach `tcp_error()` through `c/net/ip/icmp.c`. Destination-unreachable codes for network, host, and port close the matching connection. Fragmentation-needed errors lower the path MSS estimate through a plateau table and restart output from `snd_una`. The third consecutive retransmission timeout also lowers a path estimate above 508 bytes to that ceiling.

## Calling and locking rules

For a positive buffer size, `tcp_recv()` returns available contiguous bytes, zero while awaiting data, and a negative result for peer EOF or connection closure after retained buffered bytes are drained. The descriptor layer translates that negative result into a zero-length stream read.

The blocking helpers `tcp_connect()` and `tcp_send()` advance networking through `net_poll()` and idle between attempts. `tcp_connect()` applies a five-second deadline. Each wait for send ring space in `tcp_send()` has an eight-second deadline. Asynchronous opens progress through transport retransmissions and caller deadlines.

`c/net/transport/tcp.h` requires interrupts enabled for helpers that pump networking. Syscall and asynchronous write paths use `tcp_send_nb()`. Descriptor writes combine that call with `tcp_wait_writable()`; descriptor reads use `tcp_wait_readable()`.

TCP allocation, send and receive operations, and timer sweeps acquire `net_lock()`. Packet input inherits the lock from the receive path, whose contract is documented in `c/net/link/eth.h`. `c/net/core/netlock.c` implements a recursive spinlock that disables local interrupts. Recursion supports packet processing that generates output and enters lower network layers.

The documented lock order is the network lock, then a wait queue lock, then scheduler locking. Blocking helpers park after releasing the network lock and check transport state after waking.

`tcp_get_info()` exposes congestion and flow windows, negotiated options, RTT, timeout, state, and retransmission counters. `tcp_server_stats()` reports listener count, free connection slots, and admission counters.

## UDP datagrams

`c/net/transport/udp.c` provides eight bound sockets. Each has a FIFO of four datagrams, with 1,500 payload bytes per entry. `udp_bind()` assigns one socket to a local port, and `udp_send_to()` uses that port as its source.

Incoming datagrams undergo length and checksum validation before queueing. The UDP length must include its eight-byte header and fit within the received IP payload. A nonzero checksum is verified over the IPv4 pseudo-header and the declared UDP length. IPv4 datagrams carrying a zero checksum are accepted. Outgoing datagrams carry a checksum, encoding a computed zero as `0xffff`.

A full queue drops the arriving datagram. A payload exceeding one entry is also dropped. Both conditions increment the socket’s drop counter, read through `udp_drops()`. The size check protects fixed queue storage when IP delivers a large datagram.

`udp_recv()` consumes the oldest datagram and can report its source address and port. A short caller buffer receives a truncated payload, and the entry is consumed. `udp_send()` accepts up to 1,472 payload bytes, builds the UDP header, and submits the datagram to IPv4.

A valid datagram addressed to an unbound port at a destination classified as non-broadcast triggers ICMP port-unreachable handling. The limiter remembers the most recent target address and suppresses another reply to that address within 100 ticks, or one second.

Incoming ICMP errors mark the bound socket using the quoted local port. `udp_recv()` reports the pending error once, before delivering queued data, and clears it. The descriptor receive loop in `c/net/core/lsock.c` checks UDP queues between 10 ms sleeps.

## UDP clients and address acquisition

DNS sends queries through UDP and starts a TCP fallback when a validated response carries the truncation flag. `c/net/dns/dns.c` implements both paths.

DHCP binds UDP port 68 and broadcasts client messages to server port 67. `net_cfg_acquire()` in `c/net/core/netcfg.c` selects a static configuration from settings or starts DHCP. `dhcp_run()` in `c/net/core/dhcp.c` uses a four-second monotonic deadline spanning the link wait and exchange. Its caller supplies a 300-tick exchange budget, which is clipped to the time remaining after the link wait.

After the boot attempt, `dhcp_poll()` advances acquisition through `net_poll()`. DISCOVER retransmissions use base intervals of 4, 8, 16, 32, then 64 seconds, with jitter of up to one second in either direction. Link restoration starts DISCOVER or an INIT-REBOOT request for a remembered lease. A boot timeout preserves the transaction ID and a pending REQUESTING state so late replies can complete the exchange.

Lease publication holds the network lock. An ACK assigning a different address clears the previous mask, gateway, and DNS configuration before applying supplied options; a background address change triggers an ARP announcement. A NAK answering an outstanding request clears IPv4 configuration, withdraws the primary interface’s routes through `route_flush_if()` in `c/net/core/route.c`, and restarts DISCOVER with a fresh transaction ID.

When boot acquisition fails, `c/net/core/netcfg.c` applies the static SLIRP configuration under a detected hypervisor. On physical machines, the initial IPv4 address remains `0.0.0.0` while background acquisition continues.

## Limitations

- `c/net/transport/tcp.h` lists urgent data, TCP-AO/MD5, ECN, CUBIC, and the full RFC 6675 SACK pipe algorithm as unimplemented. Reno/NewReno drives congestion recovery; SACK bounds retransmission of the first hole.
- TCP uses a 100 ms minimum RTO and a ten-second `TIME_WAIT`. `FIN_WAIT` cleanup requires a stopped retransmission timer and more than two seconds since both its last start and the last inbound traffic recorded by TCP. Comments in `c/net/transport/tcp.c` connect these policies to LAN/VM use and connection slot pressure.
- Timer registration failure makes `c/net/core/net.c` log the failure and drive TCP timers through `net_poll()`.
- IPv6 UDP dispatch is absent in `c/net/ip/ip6.c`. The IPv4 UDP receive API returns zero for both an empty queue and a consumed empty datagram; the descriptor loop interprets zero as awaiting data.
- UDP computes outgoing checksums using `net_cfg.ip`, while IPv4 output can choose a source address from the route. When those addresses differ, the checksum pseudo-header differs from the emitted IP header.
- `TRANSPARENCY.md` describes networking as an experimental project implementation without production certification or an external security audit.

## Regression coverage

`tests/unit/tcp_test.c` injects segments, captures output, and advances a simulated timer. Cases cover handshakes, reassembly, sequence wraparound, options, congestion growth, recovery, retransmission, window probes, resets, and close transitions. It includes negative controls for congestion responses and connection ownership.

`tests/unit/tcp_half_window_test.c` checks receive window updates after a local half-close. `tests/unit/tcp_readiness_test.c` checks readiness and descriptor ownership through closure. UDP checksum, queue overflow, oversized payload, and ICMP cases appear in `tests/unit/net_proto_test.c`.

`tests/unit/dhcp_test.c` exercises acquisition deadlines, late replies, backoff, link changes, and NAK handling against the routing implementation. `tests/boot/run-dhcp-test.sh` checks for a SLIRP lease and the guest’s completion marker for a 32,768-byte HTTP body.

`tests/boot/tcp_timer_wedge.py` combines a link interruption with parking `net_poll()`. Its assertions require recovery with timer softirqs active and a stall after restoring the earlier timer wiring.

## Source map

| File | What it contains |
|---|---|
| `c/net/transport/tcp.c` | TCP states, buffers, reassembly, options, congestion control, recovery, timers, listeners, and APIs |
| `c/net/transport/tcp.h` | TCP interfaces, address representation, diagnostics, and feature scope |
| `c/net/transport/udp.c` | UDP bindings, queues, checksums, output, and ICMP handling |
| `c/net/transport/udp.h` | UDP interfaces and return conventions |
| `c/net/core/net.c` | Receive processing, polling diagnostics, and TCP timer scheduling |
| `c/net/core/netlock.c` | Recursive network lock and lock ordering |
| `c/net/link/eth.h` | Receive callback locking contract |
| `c/net/core/lsock.c` | Descriptor operations, transport waits, and return translation |
| `c/net/core/sock.c` | Asynchronous connection and write processing |
| `c/net/ip/ip.c` | IPv4 dispatch, source selection, and output |
| `c/net/ip/ip6.c` | IPv6 TCP dispatch and UDP scope |
| `c/net/ip/icmp.c` | ICMP validation and transport error notification |
| `c/net/dns/dns.c` | DNS UDP queries and TCP fallback |
| `c/net/core/dhcp.c` | DHCP deadlines, acquisition, link handling, and lease publication |
| `c/net/core/netcfg.c` | Settings, boot acquisition, and fallback policy |
| `c/net/core/route.c` | Interface route withdrawal |
| `c/drivers/timer/pit.h` | Tick frequency and timer interface |
| `tests/unit/tcp_test.c` | TCP protocol cases and negative controls |
| `tests/unit/tcp_half_window_test.c` | Receive window regression after half-close |
| `tests/unit/tcp_readiness_test.c` | Readiness and descriptor lifetime checks |
| `tests/unit/net_proto_test.c` | UDP, IPv4, and ICMP cases |
| `tests/unit/dhcp_test.c` | DHCP acquisition and configuration cases |
| `tests/boot/run-dhcp-test.sh` | Guest lease and HTTP transfer checks |
| `tests/boot/tcp_timer_wedge.py` | Guest timer recovery harness and control |
| `TRANSPARENCY.md` | Claim rules and historical networking descriptions |

## See also

- [Sockets](sockets.md)
- [Link layer, IPv4 and IPv6](link-and-ip.md)
- [DNS](dns.md)
- [Network stack](../architecture/network-stack.md)
- [Testing](../architecture/testing.md)