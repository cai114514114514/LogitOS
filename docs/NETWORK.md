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

The current runtime configuration is static and matches QEMU SLIRP:
`10.0.2.15/24`, gateway `10.0.2.2`, DNS server `10.0.2.3`. There is no DHCP
client yet.

## Implemented behavior

### Ethernet and ARP

- e1000 transmit/receive with interrupt-driven RX and a polling backstop.
- ARP request/reply and a small neighbor cache.
- Gateway next-hop selection for off-subnet IPv4 destinations.

### IPv4

- Header construction, Internet checksum generation and validation.
- Header-length and total-length validation, including IP options in the checksum.
- Destination filtering for the configured local address.
- TTL and supported-source validation.
- DF is used on transmit. Incoming fragmented datagrams are explicitly dropped
  because fragment reassembly is not implemented.
- No forwarding or routing role.

### ICMP

- Echo request and echo reply.
- Checksum validation before processing.
- Ping replies are matched by source, identifier, and sequence number.

### UDP and DNS

- UDP transmit and one-shot client receive slot.
- Pseudo-header checksum generation on every transmitted datagram.
- Validation of nonzero incoming checksums; a zero checksum remains accepted for
  IPv4 compatibility.
- UDP length validation and source IP/port reporting.
- DNS client sends one A query, randomizes the transaction ID, and accepts a
  response only from the configured DNS server and port.

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

- IPv6, DHCP, IP fragmentation/reassembly, multicast, forwarding, and a general
  routing table.
- Passive TCP open (`listen`/`accept`) and server sockets.
- RFC congestion control, RTT-based retransmission timeout estimation, fast
  retransmit/recovery, and path-MTU discovery.
- TCP window scaling, SACK, timestamps, ECN, urgent data, keepalives, and a full
  close/TIME-WAIT state machine.
- General UDP sockets, concurrent UDP receive queues, and ICMP error delivery to
  transports.
- General DNS record types, TCP fallback, DNSSEC, caching, and configurable
  resolvers.

The single-outstanding-segment TCP sender is conservative, but that is not a
substitute for standards-compliant congestion control. The fixed retransmission
timer is adequate for the tested QEMU/SLIRP path, not a claim about arbitrary
networks.

## Verification

Run:

```sh
make test-net
```

This executes host-side white-box tests for TCP checksum and reassembly, MSS and
window negotiation, zero-window persistence, reset handling, delayed close, IPv4
validation, UDP checksums, fragment rejection, and ICMP echo matching. QEMU boot
and browser tests remain necessary integration evidence; host protocol tests alone
do not prove driver timing or Internet interoperability.

Run the local, deterministic end-to-end data-path test with:

```sh
make test-net-os
```

It boots Aether under QEMU, serves a 32 KiB fixture from the host, and checks that
the guest `net get` command receives the complete HTTP body through e1000, IPv4,
TCP, HTTP, the syscall boundary, and a ring-3 process. It does not require a public
Internet server.

Normative references used for this implementation include
[RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html) for TCP,
[RFC 768](https://www.rfc-editor.org/rfc/rfc768.html) for UDP, and
[RFC 1122](https://www.rfc-editor.org/rfc/rfc1122.html) for IPv4 host behavior.
