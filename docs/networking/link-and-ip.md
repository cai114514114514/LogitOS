# Link layer, IPv4 and IPv6

LogitOS carries IPv4 and IPv6 over Ethernet. IPv4 uses ARP for neighbor resolution, a routing table for next-hop selection, and DHCP for address configuration. IPv6 uses Neighbor Discovery for neighbor resolution, router discovery, and stateless address configuration. Ethernet processing lives in `c/net/link/`, IP processing in `c/net/ip/`, and IPv4 routing and DHCP in `c/net/core/`.

## Packet flow and shared state

Received frames reach `eth_input()`, which dispatches ARP, IPv4, and IPv6 packets. NIC interrupts schedule a receive drain through a network softirq. `net_poll()` also drains receive queues. If a previous interrupt left a drain pending, `net_rx_schedule()` drains inline. These paths converge in `c/net/core/net.c`.

When armed by `net_init()`, a periodic kernel timer raises the network softirq every 10 ms. Its handler drains receive queues before advancing TCP timers, allowing newly received acknowledgments to affect retransmission decisions. `net_rx_poll_active()` reports polling when no NIC interrupt route is recorded, or when unannounced drains have found at least four frames during interrupt silence exceeding 100 ms. An interrupt reaching `net_rx_schedule()` resets that frame count.

IPv4 transmission selects a route, resolves its next hop through ARP, and passes the packet to `eth_send()`. IPv6 selects a source address and next hop, then resolves unicast neighbors through Neighbor Discovery. Both families use the Ethernet framing code.

The global `net_cfg`, a `struct net_config`, contains the MAC address and IPv4 address, subnet mask, gateway, and DNS server. IPv4 values use host byte order. IPv6 uses `ip6_addr`, containing 16 bytes in network byte order, and an address table with states and lifetimes. See `c/net/core/net.h` and `c/net/ip/ip6.h`.

The recursive `net_lock()` serializes shared packet-processing state across CPUs and masks interrupts on the current CPU. Recursion accommodates receive handlers that transmit replies while their caller holds the lock. The routing table uses a table lock, and DHCP uses a task owner for its state machine. See `c/net/core/netlock.c`, `c/net/core/route.c`, and `c/net/core/dhcp.c`.

## Ethernet framing and local delivery

`eth_input()` checks frame length and destination before dispatching by Ethernet type:

| Ethernet type | Handler | Payload |
|---|---|---|
| `0x0806` | `arp_input()` | ARP |
| `0x0800` | `ip_input()` | IPv4 |
| `0x86dd` | `ip6_input()` | IPv6 |

The destination filter accepts the configured MAC address, broadcast, and multicast. It also accepts unicast while the configured MAC is zero, covering the initialization interval before `net_init()` copies the NIC address into `net_cfg`. Multicast reception supports IPv6 Neighbor Discovery. See `c/net/link/eth.c`.

Receive processing recognizes 802.1Q and 802.1ad tags, with up to two tags per frame. It reconstructs an untagged Ethernet header before dispatch, preserving the protocol handlers’ assumption that their payload starts after a header of 14 bytes.

`eth_send()` constructs the header and pads short frames with zeros to reach 60 bytes, excluding the frame check sequence supplied by the NIC. Centralizing padding gives drivers a common frame format and prevents stack contents from appearing in transmitted padding. Frame bounds and protocol types are defined in `c/net/link/eth.h`.

Frames addressed to the machine’s MAC enter a local queue in `c/net/core/net.c`. The receive drain processes them after the sending path returns. This ordering lets TCP publish its sequence state before local delivery produces an acknowledgment.

## ARP and IPv4 neighbors

The ARP cache in `c/net/link/arp.c` has 32 entries. Each records an IPv4 address, MAC address, state, probe count, deadline, and time of last use.

| State | Behavior |
|---|---|
| Incomplete | Broadcast resolution is in progress; outgoing packets can wait in the pending queue. |
| Reachable | The cached binding has a recent confirmation. |
| Stale | The binding remains usable; using it starts a delay before probing. |
| Delay | Traffic uses the cached MAC while waiting to probe. |
| Probe | Unicast ARP requests check the cached binding. |
| Failed | Resolution failure is cached until its deadline. |

The reachable interval is 30 seconds. Requests retry at intervals of one second, and using a stale entry starts a delay of five seconds before probing. Failed resolution is cached for five seconds. Entries that remain stale expire after two minutes. After three unanswered unicast probes, resolution returns to broadcast requests.

`arp_output()` either transmits using a resolved MAC or copies the packet into a shared pending queue of eight entries. Its zero return means transmission succeeded or the queue accepted the packet. Learning a binding flushes queued packets for that address. A full queue replaces its oldest packet, and `arp_poll()` removes packets that have waited more than three seconds. This preserves initial traffic during resolution while bounding storage and waiting time.

`arp_input()` validates hardware and protocol types, address lengths, operation, and sender MAC. It also checks the sender IPv4 address. An existing sender entry can be updated; a new entry can be created when the target IPv4 address is the configured local address.

The implementation treats an ARP reply whose target MAC matches the local MAC as confirmation. Such a reply can replace an existing binding. A differing MAC received through another message causes a reachable or stale entry to probe its cached binding. This rule is implemented in `c/net/link/arp.c`.

`arp_resolve()` returns the local MAC for the configured local IPv4 address. It also implements the IPv4 multicast mapping to `01:00:5e` followed by the group address’s low 23 bits.

At the end of initial configuration, `arp_announce()` broadcasts the configured binding when the IPv4 address is nonzero. A claim for that address from another MAC increments a conflict counter, prints a diagnostic, and receives a reply advertising the local binding. An ARP probe with sender address zero receives a reply when its target is the local address.

## IPv4 transmission and reception

`ip_send()` in `c/net/ip/ip.c` constructs an IPv4 header of 20 bytes with an incrementing identification value, TTL 64, the selected protocol, and the Don’t Fragment flag. It calculates the header checksum before submitting the packet.

Limited broadcasts and broadcasts for the configured subnet use the Ethernet broadcast address. Other destinations use the routing table. A local route sends to the machine’s MAC for queued delivery; other routes pass their next hop to `arp_output()`.

On reception, `ip_input()` validates the version, header length, total length, checksum, nonzero TTL, and reserved fragment flag. The checksum covers the received header length, including options. Destination checks admit the configured IPv4 address, internally delivered loopback traffic, and UDP broadcasts. Source checks reject zero, the limited broadcast address, addresses beginning with a first nibble of `0xe` or `0xf`, and loopback sources received from a NIC.

After validation and fragment reassembly, the packet reaches ICMP, UDP, or TCP. UDP receives the original IP header alongside its payload, preserving the destination needed for checksum verification and the header used in ICMP error quotes. TCP receives both packet addresses for its checksum. See `c/net/ip/ip.c` and `c/net/transport/udp.c`.

## IPv4 fragment reassembly

`reasm_input()` in `c/net/ip/reasm.c` groups fragments by source, destination, protocol, and identification value. Four fixed slots each contain a 64 KiB payload buffer, a bitmap of received bytes, and storage for the fragment at offset zero’s IP header.

Before copying, the implementation checks buffer bounds, overlapping bytes, and inconsistent final lengths. It tracks the highest received offset, so a final fragment ending before previously received data invalidates the datagram. With overlaps excluded and the final boundary checked, the received-byte count establishes complete coverage.

Fragments can arrive in any order. Once coverage is complete, `ip_input()` dispatches the assembled payload and calls `reasm_release()`. Keeping the header from offset zero preserves the original destination for UDP checksums and the header used in error quotes.

`ip_poll()` removes slots after more than 3,000 timer ticks without a fragment, approximately 30 seconds at the stack’s 100 Hz tick rate. It then calls the IPv6 timer pump. `net_poll()` drives this work through `ip_poll()`. See `c/net/ip/reasm.c` and `c/net/core/net.c`.

## IPv4 routing

The routing table has 16 entries. A `struct route_entry` records a destination prefix and prefix length, next hop, preferred source, outgoing interface index, metric, and flags. Its definition is in `c/net/core/route.h`.

`route_lookup()` ranks matching routes by:

1. Longest prefix.
2. Lowest metric among equal prefix lengths.
3. Earliest insertion among otherwise equal entries.

A default route has prefix length zero and follows the same ranking. For a directly connected route, lookup returns the destination itself as the next hop. A lookup miss makes `ip_send()` return an error and increments the counter exposed by `ip_no_route_count()`. See `c/net/core/route.c` and `c/net/ip/ip.c`.

Before routed transmission, IPv4 refreshes routes from the configured primary interface through `route_v4_iface()`. This installs the connected prefix and a default route when a gateway is configured. The function remembers the configuration, making repeated refreshes a comparison. A changed configuration withdraws the interface’s previous routes before installing replacements.

The refresh also installs `127.0.0.0/8` as a local route with preferred source `127.0.0.1`. Internal Ethernet delivery records its provenance, allowing IPv4 reception to distinguish loopback traffic from NIC traffic. See `c/net/ip/ip.c` and `c/net/link/eth.c`.

## IPv6 addresses and routing

`ip6_poll()` initializes IPv6 once `net_up()` reports that a NIC initialized successfully. `ip6_init()` derives a link-local address from the MAC using modified EUI-64, adds it as tentative, and starts Duplicate Address Detection and Router Solicitation. See `c/net/ip/ip6.c`.

The address table holds eight `struct ip6_ifaddr` entries. Each records its prefix length, tentative/preferred/deprecated state, preferred and valid lifetimes, and DAD progress. A preferred address becomes deprecated when its preferred lifetime expires; an address is removed when its valid lifetime expires.

`ip6_select_source()` filters candidates and ranks them by address equality, scope, deprecation, policy label, and matching prefix length. It excludes tentative addresses and sources with insufficient scope. Deprecated addresses remain candidates, with deprecation considered after equality and scope. Prefix comparison is capped by each candidate’s configured prefix length. The algorithm lives in `c/net/ip/ip6_addr.c`.

Next-hop selection uses the prefix and router tables maintained by Neighbor Discovery:

- Multicast and link-local destinations are reached directly.
- Destinations covered by a valid advertised on-link prefix are reached directly.
- Other destinations use the first router table entry with a valid lifetime.

`ip6_output()` maps multicast destinations to Ethernet addresses beginning with `33:33`. For unicast, it asks `nd_resolve()` for the next-hop MAC. A cache miss starts neighbor resolution and returns an error to the sender, allowing a later retry. Generated IPv6 headers use hop limit 255. See `c/net/ip/ip6.c`.

## IPv6 reception

`ip6_input()` checks the IPv6 version and payload bounds. It rejects multicast and loopback source addresses, and loopback and unspecified destination addresses. Destinations are accepted through the local address table or multicast membership. Joined groups include all-nodes multicast and each configured address’s solicited-node group. See `c/net/ip/ip6.c`.

Receive processing skips bounded chains of hop-by-hop options, destination options, and routing headers. A routing header with a nonzero Segments Left field is rejected. TCP receives both IPv6 addresses for its pseudo-header checksum. For ICMPv6, `ip6_input()` verifies the checksum, including the IPv6 pseudo-header, before dispatch. See `c/net/ip/ip6.c` and `c/net/transport/tcp.c`.

## Neighbor Discovery and ICMPv6

Neighbor Discovery messages require hop limit 255 and code zero. Router Advertisements also require a link-local source. Option parsing checks lengths and message bounds. Router Advertisements update router lifetimes, on-link prefixes, reachability timers, and the link MTU. The implementation stores four prefixes and four routers. See `c/net/ip/nd.c`.

An eligible autonomous prefix of length 64 forms an address through Stateless Address Autoconfiguration, or SLAAC, using the interface identifier derived from the MAC. The code checks preferred and valid lifetimes and excludes link-local prefixes from this configuration path.

Duplicate Address Detection, or DAD, sends one Neighbor Solicitation from the unspecified address to the tentative address’s solicited-node multicast group. After the retransmission interval, the address becomes preferred or deprecated according to its lifetime. A competing DAD solicitation or a Neighbor Advertisement claiming the tentative address removes it. Once the link-local address passes DAD, Router Solicitation restarts if a default router is still missing.

The neighbor cache holds 16 entries and uses incomplete, reachable, stale, delay, and probe states. Defaults are a reachable interval of 30 seconds, a retransmission interval of one second, and a delay of five seconds before probing a used stale entry. Router Advertisements can update the first two timers. Neighbor Advertisements use their Solicited and Override flags to control confirmation and replacement of bindings.

Incomplete entries receive up to three multicast solicitations. Probing entries receive up to three unicast solicitations. Exhaustion removes the entry, allowing subsequent traffic to begin resolution again. These transitions run from `nd_poll()` in `c/net/ip/nd.c`.

ICMPv6 answers echo requests up to 1,400 bytes and counts echo replies. Error parsing recognizes Destination Unreachable, Packet Too Big, Time Exceeded, and Parameter Problem. For a quoted packet with a usable local source address and TCP immediately after its IPv6 header, it extracts the remote address and port pair for the optional `tcp_error_af()` hook.

## ICMP for IPv4

`icmp_input()` verifies the checksum, delivers accepted messages to raw ICMP sockets, answers echo requests, and matches echo replies against the built-in ping state. `icmp_last_rtt()` reports the matched round-trip time in timer ticks. See `c/net/ip/icmp.c`.

Destination Unreachable and Time Exceeded processing validates the quoted IPv4 header’s version and length, checks its source against the configured IPv4 address, extracts the transport ports, and calls UDP or TCP error handling. Each transport interprets the supplied error type and code.

Incoming UDP traffic to an unbound unicast port generates a port-unreachable response quoting the original IP header and UDP header. The response path suppresses another error to the most recently recorded source within one second. This rate check lives in `c/net/transport/udp.c`.

## DHCP boot configuration

`net_init()` delegates address acquisition to `net_cfg_acquire()` in `c/net/core/netcfg.c`. When automatic configuration is disabled, that function reads static settings. Otherwise, it calls `dhcp_run(300)`, requesting approximately three seconds for the exchange within a four-second deadline shared with the initial link wait.

The link wait pumps `net_poll()` so drivers can update link state and uses `time_mono_ns()` for its deadline. The exchange checks both timer ticks and monotonic time, allowing a working monotonic clock to bound it when timer interrupts stop. See `c/net/core/dhcp.c`.

`netdev_link_up()` reads the primary interface’s `NETIF_F_RUNNING` flag under the network lock. The driver in `c/drivers/net/rtl8169.c` publishes PHY link changes through `netdev_set_link()` during receive polling and link-change interrupts. DHCP uses this state during boot and subsequent acquisition. See `c/drivers/net/netdev.c`.

Earlier documentation described a static QEMU configuration after every DHCP boot failure. The current policy in `c/net/core/netcfg.c` selects that fallback when the CPU’s hypervisor feature is set:

| Setting | Value |
|---|---|
| IPv4 address | `10.0.2.15` |
| Subnet mask | `255.255.255.0` |
| Gateway | `10.0.2.2` |
| DNS server | `10.0.2.3` |

With automatic configuration and the hypervisor feature clear, the boot path leaves IPv4 at `0.0.0.0` while background DHCP continues acquisition.

## DHCP exchange and background acquisition

The DHCPv4 client binds UDP port 68 and sends to server port 67 through the limited broadcast address. DISCOVER and REQUEST messages set the BOOTP broadcast flag and request the subnet mask, router, DNS server, and lease duration. See `c/net/core/dhcp.c`.

The exchange follows DISCOVER, OFFER, REQUEST, and ACK. Reply processing checks the BOOTP operation, magic cookie, transaction identifier, and client MAC. An ACK publishes its assigned address and available configuration options into `net_cfg` under the network lock. When the assigned address changes, mask, gateway, and DNS values begin at zero before options are applied. Renewal of the same address retains values for omitted options.

After the boot attempt, `dhcp_poll()` continues acquisition through `net_poll()`. Handover preserves the transaction identifier and an outstanding requesting state, allowing late OFFERs and ACKs to complete the exchange.

Background DISCOVER retries use base delays of 4, 8, 16, 32, then 64 seconds, with jitter of up to one second in either direction. Background acquisition REQUESTs receive one retransmission before the client begins discovery with a new transaction identifier.

A link-down transition pauses scheduled transmissions and retains the lease. When the link returns, a client holding a lease sends an INIT-REBOOT REQUEST. It carries the remembered address in option 50, sets the client-address field to zero, and omits the server identifier. A client seeking its first lease sends DISCOVER. A background ACK that changes the address triggers `arp_announce()`; routed transmission refreshes routes from the new configuration.

## DHCP renewal and refusal

Renewal starts halfway through the lease duration from option 51, using a duration of one hour when the option is absent. A renewal REQUEST carries the current address in its client-address field. During the boot exchange and renewal, waiting phases retransmit at intervals of one second, allowing four retries. Renewal retry exhaustion returns the client to discovery and its background backoff. See `c/net/core/dhcp.c`.

ACK and NAK processing applies while a REQUEST is outstanding in requesting, renewing, or INIT-REBOOT state. A NAK clears the IPv4 address, mask, gateway, and DNS server under the network lock, withdraws the primary interface’s routes through `route_flush_if()`, and starts discovery with a new transaction identifier.

Route withdrawal matters at this transition because the refresh in `c/net/ip/ip.c` skips the primary interface when both its address and mask are zero. `route_flush_if()` also clears the remembered interface configuration, allowing the next lease to reinstall its routes. See `c/net/core/route.c`.

The `Makefile` target `make test-dhcp-host` exercises scenarios in `tests/unit/dhcp_test.c`, including late replies, link changes, backoff, the shared boot deadline, and NAK route withdrawal. `make test-dhcp-os` uses `tests/boot/run-dhcp-test.sh` to check the DHCP lease marker and a complete 32 KiB HTTP transfer through QEMU user networking.

## Limitations

- Both IP receive paths implement host delivery without forwarding. Transmission uses the primary NIC; the IPv4 route’s interface index is not passed through Ethernet and ARP output. See `c/net/ip/ip.c`, `c/net/ip/ip6.c`, and `c/drivers/net/netdev.h`.
- VLAN reception strips up to two tags without enforcing VLAN membership. Transmit frames are untagged. See `c/net/link/eth.c`.
- IPv4 output sets Don’t Fragment and rejects packets exceeding 1,500 bytes. IPv6 output enforces a link MTU accepted in the range 1,280–1,500 bytes. IPv6 fragment reassembly, UDP delivery, redirect processing, and loopback delivery are absent. See `c/net/ip/ip.c`, `c/net/ip/ip6.c`, and `c/net/ip/nd.c`.
- IPv6 reception accepts tentative address-table destinations and permits an unspecified source beyond the DAD path. Such packets can reach TCP dispatch. Router Advertisement processing uses the last Prefix Information option found in a message. See `c/net/ip/ip6.c` and `c/net/ip/nd.c`.
- The ICMPv6 `tcp_error_af()` hook has no production implementation. `tests/unit/nd_test.c` supplies a callback to check its arguments; kernel TCP receives no path-MTU update through this hook.
- DHCP has no T2 rebinding deadline or lease-expiry withdrawal. Discovery after renewal silence retains the existing configuration. See `c/net/core/dhcp.c`.
- Link tracking depends on driver reports. `c/drivers/net/netdev.h` records that e1000, VirtIO, RTL8139, and PCnet retain the initial running flag without reporting cable changes. The RTL8169/RTL8168 driver records its hardware behavior as unverified on X79-VG4 in `c/drivers/net/rtl8169.c`.
- Hypervisor detection selects the QEMU fallback without checking the virtual network’s addressing. See `c/net/core/netcfg.c`.

## See also

- [Network stack](../architecture/network-stack.md)
- [Ethernet drivers](../drivers/network.md)
- [TCP and UDP](tcp-udp.md)
- [DNS](dns.md)
- [Sockets](sockets.md)

## Source map

| File | What it contains |
|---|---|
| `c/net/link/eth.h` | Ethernet header layout, frame bounds, protocol types, and statistics. |
| `c/net/link/eth.c` | Destination filtering, VLAN stripping, frame padding, and local-delivery entry. |
| `c/net/link/arp.h` | ARP states, statistics, and interfaces. |
| `c/net/link/arp.c` | Neighbor resolution, pending packets, probing, announcements, and conflict handling. |
| `c/net/core/net.h` | IPv4 configuration, byte-order helpers, locking interfaces, and receive diagnostics. |
| `c/net/core/net.c` | Initialization, receive scheduling, local queue, timer integration, and polling. |
| `c/net/core/netcfg.c` | Static settings, boot DHCP acquisition, and hypervisor-based fallback policy. |
| `c/net/core/netlock.c` | Recursive network lock and interrupt-state handling. |
| `c/net/core/route.h` | Routing entries, lookup results, flags, and interfaces. |
| `c/net/core/route.c` | Route ranking, mutation, withdrawal, and interface refresh. |
| `c/net/core/dhcp.h` | DHCP entry points, boot deadline contract, and diagnostic snapshot. |
| `c/net/core/dhcp.c` | DHCP messages, acquisition, link tracking, renewal, and NAK handling. |
| `c/net/ip/ip.h` | IPv4 interfaces, protocol numbers, and broadcast classification. |
| `c/net/ip/ip.c` | IPv4 construction, validation, routing integration, and protocol dispatch. |
| `c/net/ip/reasm.c` | IPv4 fragment storage, coverage checks, completion, and expiry. |
| `c/net/ip/icmp.c` | IPv4 echo handling, raw delivery, and transport error dispatch. |
| `c/net/ip/ip6.h` | IPv6 address types, address states, and protocol interfaces. |
| `c/net/ip/ip6.c` | Address lifetimes, packet processing, next-hop selection, and timer pump. |
| `c/net/ip/ip6_addr.c` | IPv6 address operations, multicast mapping, and source selection. |
| `c/net/ip/nd.c` | Neighbor Discovery, DAD, SLAAC, router and prefix tables, and ICMPv6. |
| `c/drivers/net/netdev.h` | Network device and interface contracts, including transmission and link-state constraints. |
| `c/drivers/net/netdev.c` | Receive polling, primary-interface transmission, and link-state publication. |
| `c/drivers/net/rtl8169.c` | RTL8168 packet I/O, PHY link reporting, and hardware verification notes. |
| `c/net/transport/udp.c` | IPv4 UDP checksums, receive queues, and port-unreachable generation. |
| `c/net/transport/tcp.c` | TCP reception for both address families and IPv4 ICMP error handling. |
| `tests/unit/nd_test.c` | ND scenarios and the ICMPv6 TCP error callback used by tests. |
| `tests/unit/dhcp_test.c` | DHCP scenarios using a scripted server, simulated link and clock, and routing code. |
| `tests/boot/run-dhcp-test.sh` | QEMU DHCP lease and HTTP transfer checks. |
| `Makefile` | DHCP host and guest test targets. |