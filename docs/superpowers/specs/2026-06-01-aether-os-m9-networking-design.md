# Aether OS — M9 Networking (design)

**Date:** 2026-06-01
**Status:** design approved; implementing

## Goal

Give the from-scratch OS a real network stack: a true PCI NIC driver (Intel
e1000) plus Ethernet/ARP/IPv4/ICMP/UDP, so the machine can answer and send
pings and do a UDP round-trip — surfaced through a ring-3 **Network** app.
TCP is explicitly out of scope (a possible later M10).

## Locked decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| NIC | Intel **e1000** (QEMU 82540EM, vendor 0x8086 / device 0x100E) | a "real" server NIC; MMIO BAR + RX/TX descriptor rings |
| RX path | **polling** | simplest, no PCI IRQ routing; driven from the WM loop, which already polls |
| Protocols | Ethernet, ARP, IPv4, ICMP, UDP | a complete "can actually use the net" milestone without TCP |
| IP config | **static** 10.0.2.15 / 255.255.255.0 / gw 10.0.2.2 | QEMU SLIRP defaults; `net_config` struct leaves room for DHCP later |
| UDP demo | **DNS** A-query to SLIRP's 10.0.2.3 | zero-setup, always-available real UDP round-trip (SLIRP has no echo server) |
| App | ring-3 **Network** app | show IP/MAC/gw; interactive ping (RTT); DNS lookup |
| Host net | QEMU **SLIRP** (`-netdev user`) | no host config; provides gw 10.0.2.2 (answers ICMP) and DNS 10.0.2.3 |

## Architecture (bottom-up; each layer a focused file)

```
PCI scan        kernel/pci.c     0xCF8/0xCFC config space; find e1000, BAR0, enable bus-master+MMIO
e1000 driver    drivers/e1000.c  reset; read MAC; RX/TX descriptor rings; e1000_tx(); e1000_rx_poll()
Ethernet        net/eth.c        frame build/parse; dispatch by ethertype (0x0806 ARP, 0x0800 IPv4)
ARP             net/arp.c        reply to requests; resolve IP->MAC (cache); request on miss
IPv4            net/ip.c         header build/parse + checksum; next-hop = dst if on-subnet else gw
ICMP            net/icmp.c       echo reply (answer ping); echo request (ping out) + reply matching
UDP             net/udp.c        datagram send/recv; small port->pending-recv table
DNS (in app/    net/dns.c        build A-query, parse first A answer (UDP client)
  or net)
net core        net/net.c        net_config {mac,ip,mask,gw}; net_init(); net_poll() pump
syscalls        kernel/wm.c      SYS_NET_* routed via wm_gui_syscall
Network app     user/netapp.c    GUI: IP/MAC/gw; ping <ip> (RTT); dns <name>
```

**RX data flow:** `wm_run` loop → `net_poll()` → `e1000_rx_poll()` → `eth_input()`
→ `arp_input` / `ip_input` → `icmp_input` / `udp_input`.
**TX data flow:** app syscall → `udp_send`/`icmp_echo_send` → `ip_send` (resolve
next-hop MAC via `arp_resolve`) → `eth_send` → `e1000_tx()`.

The whole stack runs in the WM's **kernel address space** (shared CR3), so its
DMA rings and buffers are always reachable regardless of which app is current.

## Component notes

### PCI (`kernel/pci.c`)
- Config access: write a 32-bit address (enable bit | bus<<16 | dev<<11 |
  func<<8 | offset) to port 0xCF8, read/write data at 0xCFC.
- Scan bus 0, dev 0..31, func 0. Match vendor 0x8086 / device 0x100E.
- Read BAR0 (offset 0x10) → MMIO base (mask low 4 bits). Set command register
  (offset 0x04) bits: I/O+MEM space (0x1|0x2) and **bus master** (0x4).
- Expose `pci_find(vendor, device) -> {bar0, irq_line}` (irq unused — polling).

### e1000 (`drivers/e1000.c`)
- Map BAR0 (128 KiB) with `vmm_map_range(bar0, bar0, 0x20000, VMM_WRITABLE|VMM_NOCACHE)`.
- Register read/write via `volatile uint32_t *` at `bar0 + off`.
- **Reset:** set CTRL.RST (bit 26); wait; set CTRL.ASDE|SLU (link up).
- **MAC:** read RAL0 (0x5400) / RAH0 (0x5404) — QEMU pre-fills the address.
- **RX ring:** N=32 legacy descriptors (16 B: addr,len,csum,status,errors,special)
  + N×2048-byte buffers, all from `pmm_alloc_contig` (identity-mapped). Program
  RDBAL/RDBAH, RDLEN=N×16, RDH=0, RDT=N-1. RCTL = EN|BAM|BSIZE2048|SECRC.
- **TX ring:** N=8 descriptors + buffers. TDBAL/TDBAH, TDLEN, TDH=TDT=0.
  TCTL = EN|PSP|(CT/COLD defaults); set TIPG.
- `e1000_tx(buf,len)`: copy into next TX buffer, desc.cmd = EOP|IFCS|RS, bump
  TDT; (optionally spin on desc.status.DD for completion).
- `e1000_rx_poll(cb)`: software tail index; while desc[i].status & DD: deliver
  buffer to `cb`; clear status; set RDT=i; advance i. Disable e1000 interrupts
  (IMC=all) since we poll.

### Ethernet / ARP / IPv4 / ICMP / UDP (`net/*.c`)
- `eth_send(dst_mac, ethertype, payload, len)` prepends our MAC + dst; `eth_input`
  dispatches by ethertype.
- ARP: 16-entry IP→MAC cache; reply to requests for our IP; `arp_resolve(ip)`
  returns cached MAC or sends a request and reports "pending".
- IPv4: build/parse header, ones-complement checksum, TTL=64; `ip_send` picks
  next-hop (on-subnet dst vs gateway) and resolves its MAC.
- ICMP: answer echo requests (so the host can ping us); `icmp_echo_send` for
  outbound ping; match replies by id/seq, record RTT in PIT ticks.
- UDP: `udp_send(dst_ip,dport,sport,data,len)`; `udp_input` matches a small
  table of pending receives (keyed by local port).
- DNS: build an A-query, parse the first A answer; used over UDP to 10.0.2.3.

### net core (`net/net.c`)
- `net_config { uint8_t mac[6]; uint32_t ip, mask, gw; }` (static defaults; DHCP
  hook later). `net_init()` from kmain (after pci/e1000); `net_poll()` from the
  WM loop.

### Syscalls + app
- ABI: `SYS_NET_INFO` (fill {ip,mask,gw,mac}), `SYS_NET_PING(ip)` /
  `SYS_NET_PING_STATUS` (→ RTT ms, or -1 pending), `SYS_NET_DNS(name)` /
  `SYS_NET_DNS_STATUS` (→ resolved IP, or 0 pending). Routed via
  `wm_gui_syscall`; helpers in `user/aether.h`.
- `user/netapp.c`: shows local IP/MAC/gateway; text field to ping an address
  (shows RTT) and to resolve a hostname (shows the A record). Added to the
  Makefile APP list + Dock.

## Error handling
- No e1000 found → `[net] no NIC`, skip net init; desktop still boots.
- ARP miss → send request, drop the outbound packet; caller retries.
- Malformed/short frames, bad checksums → dropped silently.
- MMIO mapped uncached; rings physically contiguous and identity-mapped.

## Testing / verification
1. **Kernel boot self-test:** ping gateway 10.0.2.2 and DNS-query 10.0.2.3; on
   both replies print `AETHER_NET_OK` on serial (fits the existing marker test).
2. **pcap forensics:** QEMU `-object filter-dump,...,file=build/net.pcap` to
   confirm correct ARP/ICMP/DNS frames left the NIC.
3. **QMP UI smoke:** drive the Network app to ping the gateway and resolve a
   name; screenshot.
4. `make test` stays green (existing AETHER_BOOT_OK marker).

## QEMU wiring (Makefile run/debug/test)
```
QEMU_NET := -netdev user,id=n0 -device e1000,netdev=n0 \
            -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap
```
SLIRP (user networking) needs no host setup: gw 10.0.2.2 answers ICMP, 10.0.2.3
is a DNS server, guest is 10.0.2.15.

## Build order (each layer builds + boots; commit per layer)
1. **PCI + e1000 bring-up + TX:** reset, MAC, rings, `e1000_tx`; self-test sends
   one broadcast ARP; verify a frame in the pcap.
2. **RX + Ethernet + ARP:** `e1000_rx_poll`, eth dispatch, ARP reply + resolve;
   self-test resolves the gateway MAC.
3. **IPv4 + ICMP:** ping the gateway → `AETHER_NET_OK` with RTT.
4. **UDP + DNS:** resolve a name via 10.0.2.3.
5. **Syscalls + Network GUI app + docs** (CLAUDE.md + this spec's result).

## Risks
- MMIO base is high (~0xFEB80000), outside the identity map — must `vmm_map_range`
  it first (framebuffer already proves this works).
- Descriptor rings must be physically contiguous + identity-mapped → `pmm_alloc_contig`.
- e1000 register/bit details are fiddly; bring-up is staged and pcap-verified.

## Result — all five layers implemented & verified

A from-scratch network stack works end to end in QEMU:
- **L1 PCI + e1000 + TX:** NIC found (MAC 52:54:00:12:34:56, MMIO 0xfebc0000); a
  probe frame appears in the pcap byte-for-byte.
- **L2 RX + ARP:** the gateway MAC resolves (52:55:0a:00:02:02). RX bring-up
  surfaced two non-driver bugs (caught via QEMU's `e1000x_rx_can_recv_disabled`
  trace + reading QEMU source): QEMU's `set_rx_control` defers its RX-queue
  flush by ~1s, and the original self-test ran with interrupts disabled (no time
  base) — both test harness issues, not driver bugs.
- **L3 IPv4 + ICMP:** ping the gateway → reply; pcap shows ICMP type 8/0.
- **L4 UDP + DNS:** `example.com` resolves via 10.0.2.3; pcap shows 2 UDP/53
  frames.
- **L5 syscalls + app:** the ring-3 **Network** app shows IP/MAC/gw, pings the
  gateway ("reply 10 ms"), and resolves a host (example.com → an A record) — all
  over `SYS_NET_*` syscalls, the net stack pumped by the WM loop. A cold-ARP
  first-send drops, so the app re-issues until a reply arrives.

Decisions that held up: **polling** (driven by the WM loop) avoided PCI IRQ
routing entirely; **static SLIRP config** needed no host setup; **DNS** gave a
zero-setup real UDP round-trip (SLIRP has no echo server). TCP remains out of
scope (possible future M10).
