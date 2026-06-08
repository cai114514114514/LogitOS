# M26 — TCP robustness (out-of-order reassembly + send segmentation)

## Why

The M10 client TCP (`src/net/transport/tcp.c`) was "enough for HTTP GET": a single
outstanding segment, **no out-of-order reassembly**. `tcp_input` accepts a data
segment only when `seg_seq == rcv_nxt`; anything else is dropped (it only emits a
dup-ack). On a clean SLIRP link a small GET works, but the moment a segment is
reordered or lost mid-flight, every later segment is discarded too, and recovery
falls back to the sender's go-back-N. For a large TLS handshake flight (e.g.
Sectigo: 4 certs incl. a 4096-bit CA) this stalls past the timeout → `TLS_E_PROTO`.
That is the "TCP robustness gap, not a crypto one" noted in CLAUDE.md.

A second latent bug: `tcp_send` **silently truncates** any payload > MSS (1460)
(`if (len > TXBUF) len = TXBUF;`). Fine for tiny GETs; wrong for large requests.

This is the first of the "fix the historical limitations" line, TCP first.

## Scope

IN (client correctness/robustness):
- **Out-of-order reassembly** on receive: buffer segments ahead of `rcv_nxt`,
  coalesce when the gap fills, advance `rcv_nxt` over contiguous data.
- **Send segmentation**: a payload > MSS is sent as multiple segments (single
  outstanding, wait per chunk) instead of truncated.
- Window advertisement that accounts for out-of-order bytes already buffered.
- FIN accepted only once it is in-order (its seq == rcv_nxt after reassembly).

OUT (perf, not correctness — deferred, stated so we don't pretend otherwise):
- Congestion control / slow start (we are receive-bound; sender does its own).
- Window scaling (the 64 KiB window already holds a full flight/body).
- SACK (cumulative dup-ack + reassembly already recovers a single loss; SACK
  only speeds multi-loss recovery).
- listen/accept (server side) — the browser is a pure active-open client.

## Design (reassembly)

The receive ring `rx[RXBUF]` (RXBUF = 65536, power of two) is indexed by
**sequence number**: byte with seq `s` lives at ring index
`(rx_head + (s - read_seq)) & (RXBUF-1)`, where `read_seq` is the seq of the
first unread byte (= `rx_head`). Because a drain advances `rx_head` and
`read_seq` together, `(rx_head - read_seq)` is invariant, so a given seq maps to
a fixed ring slot for its whole lifetime — out-of-order bytes written early are
read back correctly once the gap fills.

State added to `struct tcp_conn`:
- `uint32_t read_seq;` — seq of `rx_head`. Contiguous readable = `rcv_nxt - read_seq` = `rx_len`.
- `struct ooo_seg { uint32_t seq, end; } ooo[NOOO]; int n_ooo;` — sorted, merged,
  non-overlapping list of received ranges strictly beyond `rcv_nxt` (NOOO=16; on
  overflow the excess OOO segment is dropped and the sender resends).
- `rx_tail` is removed (positions derive from `read_seq`).

On a data segment `[seg_seq, seg_seq+dlen)`:
1. Clip to the window `[rcv_nxt, read_seq+RXBUF)` (drop already-received prefix
   and anything beyond the buffer). Wraparound-safe seq compares via `(int32_t)(a-b)`.
2. `ring_write` the clipped bytes at their seq offset.
3. Merge the range into the reassembly set; if it touches the contiguous front,
   advance `rcv_nxt` and absorb now-contiguous `ooo` intervals (single forward
   pass — the list is sorted with real gaps between intervals).
4. Send a cumulative ACK of `rcv_nxt` (a dup-ack when a gap remains → nudges the
   sender to fast-retransmit the hole).

Window = `RXBUF - (furthest - read_seq)`, `furthest = ooo[n-1].end or rcv_nxt`.

Send segmentation: `tcp_send` loops MSS-sized chunks; for each non-final chunk it
sends + arms retransmit, then pumps `net_poll` until that chunk is ACKed (single
outstanding) before the next. The ≤MSS fast path is unchanged (send one, return).

## Verify

1. **Host unit test** `tools/t/tcp_test.c` (`make test-tcp-host`): stub
   `ip_send`/`timer_ticks`/`net_cfg`, drive `tcp_input` with crafted segments —
   in-order, full reverse order, single gap then fill, overlapping/duplicate,
   beyond-window — and assert the drained stream equals the original and the
   emitted ACKs advance correctly. Deterministic, no QEMU.
2. **QEMU integration** `make test-tcp`: a host `python3 -m http.server` serving a
   large known file; Aether fetches it over SLIRP and asserts the full byte count
   (exercises the real NIC + reassembly end to end).
3. No regression: `make test`, `make test-shell` stay green; the Browser still
   opens https sites (and now larger-flight ones).
