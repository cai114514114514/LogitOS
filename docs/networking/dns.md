# DNS

LogitOS resolves host names through the kernel stub resolver in `c/net/dns/dns.c`. It queries a configured upstream server, combines IPv4 and IPv6 destinations for client sockets, and caches successful results using address and alias lifetimes. Kernel callers can use asynchronous queries or a blocking IPv4 wrapper. Applications reach the resolver through a legacy syscall interface.

## Resolver interfaces

The kernel interfaces are declared in `c/net/dns/dns.h`.

| Interface | Behaviour |
|---|---|
| `dns_query_start(name)` | Allocates a query and returns its ID, or `-1` on failure. Accepted address literals and cache hits complete immediately. |
| `dns_query_addrs(id, out, max)` | Returns `0` while pending, a negative value on failure, or the number of destinations copied and sorted in `out`. |
| `dns_query_result(id)` | Returns the first IPv4 address in host order, `0` while pending, or `0xFFFFFFFF` on failure or completion without an IPv4 address. |
| `dns_query_free(id)` | Releases the query, its UDP socket, and associated TCP fallback attempts. |
| `dns_start(name)` / `dns_result()` | Starts and polls the legacy IPv4 lookup. Retrieving a terminal result releases its resources. |
| `dns_resolve(name)` | Waits for an IPv4 result while calling `net_poll()` and `net_idle()`. Returns the address or `0` on failure. Requires interrupts enabled. |

The client socket broker in `c/net/core/sock.c` starts a pooled query, retrieves its destination list, and frees the query before attempting connections. Completed pool slots remain allocated until their callers invoke `dns_query_free()`.

The legacy interface backs `SYS_NET_DNS` and `SYS_NET_DNS_RESULT` in `c/kernel/exec/syscall.c`. Application wrappers in `c/apps/logit.h` expose these as `net_dns()` and `net_dns_result()`. The C library’s `getaddrinfo()` and `gethostbyname()` use the syscall pair through `resolve4()` in `c/apps/libc/src/netdb.c`; numeric IPv4 input is handled locally.

## Query lifecycle

For a network lookup, `dns_query_start()` binds a UDP socket on a random source port, initiates ARP resolution for the upstream, and sends an A question. It also sends an AAAA question when `ip6_up()` returns `2`. In `c/net/ip/ip6.c`, that state requires a link-local address and another address that have passed the tentative state, plus a default router. Both questions travel over IPv4.

Each question receives a transaction ID derived from kernel random bytes and the timer counter. Retransmission rebuilds the questions and generates new IDs. The pooled and legacy interfaces share response parsing and query advancement in `c/net/dns/dns.c`.

`dns_poll()` advances UDP queries and TCP fallback attempts. In `c/net/core/net.c`, `net_poll()` calls it before advancing client sockets, allowing a completed lookup to move its socket forward during the same pass. Polling the legacy interface through `dns_result()` also calls `dns_poll()`.

Resolver deadlines use the tick counter and the 100 Hz `TIMER_HZ` setting in `c/drivers/timer/pit.h`.

| Event | Interval |
|---|---|
| UDP retransmission | 50 ticks, approximately 500 ms |
| Overall lookup deadline | 300 ticks, approximately 3 seconds |
| Wait for another requested address family after the first reply | 50 ticks, approximately 500 ms |

A lookup completes when all requested families have replied, the wait after the first reply expires, or the overall deadline expires. A response accepted by the parser marks its family as answered even when it contributes zero addresses. Completion succeeds when the collected address set is nonempty. A receive error from `udp_recv()` marks the lookup failed.

## Response handling

Outgoing questions request Internet-class records, set recursion desired, and include an EDNS0 OPT record advertising a 4096-byte UDP receive size. The UDP receive path checks the upstream address, source port 53, and transaction ID. The shared answer collector checks the transaction ID and response flag.

`collect_answers()` in `c/net/dns/dns.c` converts the requested name to lowercase with a trailing dot. It accepts Internet-class address records whose owner matches that name. A matching CNAME updates the target used for subsequent records, connecting accepted addresses to the requested name through aliases encountered in the response.

IPv4 records enter the result as IPv4-mapped IPv6 addresses, represented as `::ffff:a.b.c.d`. AAAA records pass checks that reject multicast, loopback, unspecified, and IPv4-mapped addresses.

A matching UDP packet with the truncation flag starts a TCP query for the affected family. The fallback connects to the upstream on port 53, sends a question with a two-byte length prefix, reads the response length and body, and delivers the body to the shared collector. The owning lookup’s deadline continues to apply during this exchange.

## Destination ordering

`dns_query_addrs()` copies up to the caller’s requested capacity and sorts that subset using the current local source-address candidates. Evaluating preference at retrieval time lets a cached result reflect changes in local address availability.

The policy in `c/net/ip/ip6_addr.c` considers source availability, scope, deprecated source addresses, address labels, precedence, and prefix matching. Its stable sort preserves the received order when candidates compare equally. Longest-prefix comparison applies to IPv6 pairs; equivalent IPv4 destinations retain their upstream order.

## Caching

The cache in `c/net/dns/dns.c` holds 16 name entries. Each entry stores an address set, its first IPv4 address, and an expiry deadline. An existing name is refreshed in place; new names use round-robin replacement.

Each response that adds addresses contributes a deadline based on the smallest TTL among its accepted address records and followed CNAMEs. The query retains the earliest deadline across responses. Deadlines begin when responses are processed, so time spent waiting for another address family consumes part of the cache lifetime.

`DNS_TTL_CEIL` caps caching at 3600 seconds. A zero-TTL result serves the current query and expires immediately for cache reuse. When a completed query supplies an address set whose deadline has expired, cache insertion invalidates any existing entry for the same name.

An older comment in `c/net/dns/dns.c` describes a five-second TTL floor. The correction beside it explains the current policy: short and zero TTLs retain their supplied lifetimes.

Cache keys preserve the caller’s spelling and compare byte for byte. Answer-owner matching uses lowercase names with a trailing dot.

For hostnames, `dns_start()` sends a fresh network query and can populate the cache on completion. Older comments also describe `dns_resolve()` as using that legacy slot and bypassing the cache. The current function calls `dns_query_start()`, giving each blocking caller a pooled query and access to cached results.

## Configuration

The upstream address is the `dns` member of `struct net_config` in `c/net/core/net.h`. Resolver sends and UDP source checks obtain it through `net_config_snapshot()`.

`net_init()` in `c/net/core/net.c` delegates boot configuration to `net_cfg_acquire()` in `c/net/core/netcfg.c`.

| Configuration path | DNS address source |
|---|---|
| Saved `net.dhcp` is `0` | Saved `net.dns`, with `10.0.2.3` as its default |
| DHCP lease includes option 6 | First IPv4 address in that option |
| Boot DHCP attempt fails under a hypervisor | Static fallback `10.0.2.3` |
| Boot DHCP attempt fails on physical hardware | Initial value `0.0.0.0` |

The previous description applied the static fallback to every failed DHCP attempt. The current policy tests the hypervisor feature before applying it.

During boot, `dhcp_run()` in `c/net/core/dhcp.c` gives the link wait and DHCP exchange one four-second deadline measured through `time_mono_ns()`. The exchange gets up to three seconds within the remaining budget. When the boot attempt times out, `dhcp_poll()` continues acquisition through `net_poll()`.

Background discovery uses retransmission delays of 4, 8, 16, 32, and then 64 seconds, with up to one second of random jitter in either direction. Waiting for a request acknowledgement has its own retry and restart rules. A link becoming available restarts acquisition immediately; a client holding a lease requests confirmation through INIT-REBOOT.

`apply_lease()` adopts option 6 during initial configuration, lease renewal, and acquisition after boot. A lease for a different IPv4 address resets the mask, gateway, and DNS fields before applying supplied options. Renewal of the same address starts from the current configuration. Publication holds the network lock used by `net_config_snapshot()`.

A matching DHCP NAK received while a request is outstanding clears the configured address, mask, gateway, and DNS address, withdraws the interface’s routes, and restarts discovery.

The settings schema in `c/kernel/core/settings.c` defines these defaults:

| Key | Default |
|---|---|
| `net.dhcp` | `1` |
| `net.ip` | `10.0.2.15` |
| `net.mask` | `255.255.255.0` |
| `net.gw` | `10.0.2.2` |
| `net.dns` | `10.0.2.3` |

The Settings application in `c/apps/gui/settings/settings.c` reads and saves these values. The frontend in `c/apps/coreutils/pref.c` provides `pref read` and `pref write`.

Settings permissions follow the active store. Root can write the system store, and a user can write schema values in their own store. Older comments in `c/apps/coreutils/pref.c` describe schema keys as “root only”; the current ownership check is in `c/kernel/core/settings.c`.

At boot, `c/kernel/init/kmain.c` loads system settings before calling `net_init()`. User settings are adopted later during login.

## Tests

`tests/unit/dns_test.c` exercises the production resolver with packet fixtures, modeled UDP and TCP transports, and a controlled clock. Cases cover owner matching, CNAME traversal, truncation fallback, compression-pointer loops, EDNS0, transaction IDs, and TTL handling.

`tests/unit/dns_ttl_test.c` covers alias lifetimes, short and zero TTLs, concurrent cache replacement, and time spent waiting for another family. `tests/unit/dns_class_test.c` covers Internet-class filtering. `tests/net.mk`, `tests/dns_ttl.mk`, and `tests/dns_class.mk` attach these suites to `test-dns`, with their negative controls as prerequisites.

`tests/unit/ip6_dns_test.c` covers A/AAAA queries, merged results, destination ordering, and completion when one family is delayed. `tests/unit/net_proto_test.c` checks the legacy resolver’s response to ICMP errors using the production UDP and ICMP paths.

`tests/unit/dhcp_test.c`, selected by `test-dhcp-host` in `Makefile`, checks DNS option adoption, acquisition after boot, the shared boot deadline, link changes, and configuration clearing after a NAK.

## Limitations

- The resolver uses one IPv4 upstream and trusts its responses. Local recursive resolution and DNSSEC validation are absent.
- CNAME traversal follows up to eight aliases in one forward scan of the answer section. Aliases and terminal addresses must appear in traversal order.
- The pool holds eight queries, each retaining up to eight addresses across both families. TCP fallback has two slots and accepts response bodies up to 4096 bytes.
- Negative caching and response-code-specific error reporting are absent. The collector skips echoed questions without validating their names, types, or classes.
- The legacy syscall pair shares one global lookup slot; another start replaces its lookup. The C library uses this path for IPv4 resolution.
- An initial DHCP lease acquired during boot without option 6 leaves DNS at zero. A lease for a different address also clears the previous DNS value when that option is absent.
- Upstream configuration changes leave existing cache entries available until expiry.
- Saved network settings take effect at boot. Saving values in a user’s store neither reconfigures the active stack nor changes the system settings used at the next boot.

## See also

- [Network stack](../architecture/network-stack.md)
- [Sockets](sockets.md)
- [Link layer, IPv4 and IPv6](link-and-ip.md)
- [TCP and UDP](tcp-udp.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
|---|---|
| `c/net/dns/dns.h` | Kernel resolver interfaces and result conventions |
| `c/net/dns/dns.c` | Query advancement, answer parsing, caching, deadlines, and TCP fallback |
| `c/net/core/net.h` | Network configuration and snapshot accessor |
| `c/net/core/net.c` | Network initialisation and resolver polling |
| `c/net/core/netcfg.c` | Saved configuration, boot DHCP acquisition, and hypervisor fallback policy |
| `c/net/core/dhcp.c` | Lease adoption, DNS options, background acquisition, and NAK handling |
| `c/net/core/sock.c` | Pooled resolver consumer for client connections |
| `c/net/ip/ip6.c` | IPv6 readiness and local source-address candidates |
| `c/net/ip/ip6_addr.c` | Source selection and destination ordering |
| `c/drivers/timer/pit.h` | Timer frequency and tick interface |
| `c/kernel/init/kmain.c` | Settings and network initialisation order |
| `c/kernel/exec/syscall.c` | Legacy DNS syscall handlers and settings adoption during login |
| `c/apps/logit.h` | Application DNS wrappers |
| `c/apps/libc/src/netdb.c` | C library name resolution |
| `c/kernel/core/settings.c` | Network defaults, settings stores, and permission checks |
| `c/apps/gui/settings/settings.c` | Settings application load and save operations |
| `c/apps/coreutils/pref.c` | Command-line settings frontend |
| `tests/unit/dns_test.c` | Parser and transport fixtures |
| `tests/unit/dns_ttl_test.c` | Cache lifetime regression cases |
| `tests/unit/dns_class_test.c` | Record-class regression cases |
| `tests/unit/ip6_dns_test.c` | A/AAAA lookup and ordering fixtures |
| `tests/unit/net_proto_test.c` | Legacy resolver and ICMP error cases |
| `tests/unit/dhcp_test.c` | DNS option, acquisition, deadline, link, and NAK regression cases |
| `Makefile` | DHCP, IPv6 DNS, and protocol test targets |
| `tests/net.mk` | Base DNS suite and owner-matching negative control |
| `tests/dns_ttl.mk` | TTL suite and negative controls |
| `tests/dns_class.mk` | Record-class suite and negative control |