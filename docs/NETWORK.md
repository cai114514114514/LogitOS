# Network Stack Scope

Status snapshot: 2026-08-05.

Aether's network stack is a project implementation maintained under `c/net/`; it
is not Linux networking code under different names. It is intentionally small and
client-oriented. This document records the implemented subset and the missing
pieces so that interoperability demonstrations are not mistaken for RFC-complete
support.

## Data path

```text
e1000
  -> Ethernet
     -> ARP
     -> IPv4
        -> ICMP echo
        -> UDP -> DNS A queries
        -> TCP -> HTTP -> TLS 1.3
```

The runtime configuration comes from a DHCPv4 client (RFC 2131/2132) that
negotiates at boot and renews at T1; if negotiation fails it falls back to the
static QEMU SLIRP defaults: `10.0.2.15/24`, gateway `10.0.2.2`, DNS server
`10.0.2.3`.

## Implemented behavior

### Ethernet and ARP

- e1000 transmit/receive with interrupt-driven RX and a polling backstop.
- ARP request/reply and a small neighbor cache.
- Gateway next-hop selection for off-subnet IPv4 destinations.

### IPv4

- Header construction, Internet checksum generation and validation.
- Header-length and total-length validation, including IP options in the checksum.
- Destination filtering for the configured local address; limited and
  subnet-directed broadcasts are accepted for UDP only (the DHCP receive path).
- TTL and supported-source validation.
- Fragment reassembly: four slots keyed by (src, dst, id, proto) with a 64 KiB
  cap; overlapping, conflicting, or oversized datagrams are dropped, and
  stalled slots expire. DF is used on transmit; the send path never fragments.
- No forwarding or routing role.

### ICMP

- Echo request and echo reply.
- Checksum validation before processing.
- Ping replies are matched by source, identifier, and sequence number.
- Destination-unreachable and time-exceeded errors are validated against the
  quoted IP/L4 headers and delivered to the matching UDP socket or TCP
  connection.

### UDP and DNS

- Up to eight UDP sockets (`udp_bind`/`udp_close`), each with a four-datagram
  receive queue recording source IP/port; queue overflows are counted.
- ICMP port-unreachable is generated for datagrams to unbound ports (rate
  limited, never for broadcasts), and inbound ICMP errors are delivered to the
  matching UDP socket and to TCP connections.
- Pseudo-header checksum generation on every transmitted datagram.
- Validation of nonzero incoming checksums; a zero checksum remains accepted for
  IPv4 compatibility.
- UDP length validation and source IP/port reporting.
- DNS client sends one A query from a random ephemeral port to the configured
  resolver (from DHCP or the fallback), randomizes the transaction ID, and
  accepts a response only from the resolver's address and port.

### DHCP

- Minimal DHCPv4 client (RFC 2131/2132): DISCOVER/OFFER/REQUEST/ACK with the
  broadcast flag set, xid/chaddr/magic-cookie validation, and options 1/3/6/51.
- Lease renewal (T1) via a ciaddr REQUEST; NAK or persistent silence restarts
  from DISCOVER.

### TCP

- Active open (client) and a maximum of eight simultaneous connection records.
- Mandatory TCP checksum generation and validation.
- Randomized initial sequence numbers using the kernel RNG, mixed with connection
  and timer state. The RNG's documented weak fallback still applies.
- MSS option generation and parsing; IPv4's 536-byte default is used when a peer
  omits MSS.
- Peer advertised-window tracking with ordered `SND.WL1`/`SND.WL2` updates.
- Zero-window persist probes with exponential backoff.
- One outstanding transmit segment, cumulative acknowledgments, retransmission,
  and segmentation of larger writes.
- RFC 6298 RTT estimation (srtt/rttvar in timer ticks) with Karn's algorithm:
  retransmitted segments are never sampled. RTO = srtt + max(G, 4*rttvar),
  clamped to [100 ms, 60 s], doubling on each timeout retransmission.
- Fast retransmit on the third duplicate ACK (RFC 5681 definition adapted to
  the single-outstanding model, where it is exact).
- Full close state machine: CLOSE_WAIT/LAST_ACK for the passive side, FIN_WAIT
  and CLOSING (simultaneous close) flowing into a shortened TIME_WAIT (~10 s
  instead of 2MSL — `alloc_lport` never immediately reuses a local port, and
  eight connection slots cannot afford full-length waits).
- A 64 KiB sequence-indexed receive ring with overlap handling and out-of-order
  interval reassembly.
- Receive-window sequence acceptability checks and rejection of ACKs beyond
  `SND.NXT`.
- Exact-sequence reset acceptance plus challenge ACKs for non-exact in-window
  resets.
- FIN retransmission and delayed FIN when application data is still unacknowledged.

## Deliberately missing or incomplete

The following are not implemented and must not be inferred from a successful web
request:

- IPv6, multicast, forwarding, and a general routing table.
- Passive TCP open (`listen`/`accept`) and server sockets.
- Multiple outstanding segments (a sliding send window), NewReno congestion
  control (cwnd), SACK, TCP window scaling, and path-MTU discovery. Under the
  single-outstanding model congestion control has nothing to act on; doing it
  for real means rebuilding the send buffer as a sliding window, which is a
  separate project in its own right.
- TCP timestamps, ECN, urgent data, and keepalives.
- General DNS record types, TCP fallback, DNSSEC, and caching.

The single-outstanding-segment TCP sender is conservative, but that is not a
substitute for standards-compliant congestion control. The RTT estimator and
fast retransmit make it robust on lossy paths, not faster on uncongested ones.

## Verification

Run:

```sh
make test-net
```

This executes host-side white-box tests for TCP checksum and reassembly, MSS and
window negotiation, zero-window persistence, reset handling, delayed close, RTT
estimation and RTO backoff, fast retransmit, the close state machine, IPv4
validation and fragment reassembly, UDP sockets and checksums, ICMP echo
matching and error delivery, and the DHCP client state machine. QEMU boot and
browser tests remain necessary integration evidence; host protocol tests alone
do not prove driver timing or Internet interoperability.

Run the local, deterministic end-to-end data-path test with:

```sh
make test-net-os
```

It boots Aether under QEMU, serves a 32 KiB fixture from the host, and checks that
the guest `net get` command receives the complete HTTP body through e1000, IPv4,
TCP, HTTP, the syscall boundary, and a ring-3 process. It does not require a public
Internet server. `make test-dhcp-os` additionally asserts the DHCP lease is bound
during boot before the same fetch. `make test-https-smoke` performs a live
Internet fetch (DHCP -> DNS -> TCP -> TLS 1.3) and therefore requires outbound
network access; it is not part of the deterministic offline suite.

Normative references used for this implementation include
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html) for TCP,
[RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html) for RTT estimation,
[RFC 768](https://www.rfc-editor.org/rfc/rfc768.html) for UDP,
[RFC 791](https://www.rfc-editor.org/rfc/rfc791.html) for IPv4 fragmentation,
[RFC 2131](https://www.rfc-editor.org/rfc/rfc2131.html) for DHCP, and
[RFC 1122](https://www.rfc-editor.org/rfc/rfc1122.html) for IPv4 host behavior.
