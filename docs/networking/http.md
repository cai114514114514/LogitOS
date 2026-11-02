# HTTP

LogitOS provides HTTP/1.1 and HTTP/2 client engines, HPACK header compression, connection pooling, cookies, URL handling and WebSockets. The browser runs these protocol engines in ring 3 and supplies socket transport callbacks. The implementation lives in `c/net/http/`, with browser connection management and protocol selection in `c/apps/gui/browser/app/runtime.c`.

## Components and execution

The HTTP engines receive transport callbacks from their callers. Their parsers consume available bytes incrementally, allowing the browser event loop to advance several exchanges. Host tests exercise the same parsers through transports held in memory.

The `BROWSER_PIPE` source list in `Makefile` links the protocol components into the browser. Its runtime manages connection ownership and presents HTTP/2 responses through the response interface used by page loading and JavaScript fetch.

The legacy kernel client in `c/net/http/http.c` serializes access to a shared response buffer through an owner lock. Its header, `c/net/http/http.h`, describes asynchronous progression through `http_poll()`, while the implementation performs the fetch synchronously inside `http_get()`; `http_poll()` has an empty body.

## HTTP/1.1

`struct h1_transport` supplies read and write callbacks plus an optional hint for write readiness. Reads distinguish temporary lack of data, orderly EOF and transport failure. EOF completes a response framed by connection closure. EOF during headers, a body of fixed length or chunked framing produces `H1_E_TRUNC`. These contracts are defined in `c/net/http/http1.h` and implemented in `c/net/http/http1.c`.

Requests use `struct h1_request` and a header list. `h1_request_build()` validates the method, target and header fields before serialization. Header names use token syntax, and adding a header rejects CR, LF and embedded NUL in its value. For a nonempty request body, the builder supplies Content-Length when both Content-Length and Transfer-Encoding are absent.

`h1_response_feed()` accepts partial input and advances through status lines, headers, body data, chunk boundaries and trailers. A successful return reports the number of bytes consumed, identifying the message boundary when input extends beyond the response.

| Response framing | Completion |
|---|---|
| HEAD response, or status 101, 204 or 304 | Headers complete |
| Transfer-Encoding with final coding `chunked` | The terminating chunk and trailer section arrive |
| Transfer-Encoding with another final coding | The connection closes |
| Content-Length without Transfer-Encoding | The declared byte count arrives |
| Neither Transfer-Encoding nor Content-Length | The connection closes |

Informational responses other than 101 restart status parsing for the final response. Repeated Content-Length values, including comma lists, must agree. For a response carrying a body, Transfer-Encoding takes precedence when both framing headers occur. The parser also marks that connection for closure, preventing reuse across the ambiguous framing boundary.

`h1_conn_start()` takes ownership of a serialized request buffer. Each `h1_conn_pump()` advances writes and performs at most one read of up to 4 KiB. The read callback reports how much data is available, because TLS can retain buffered records after socket readiness changes. `h1_conn_progress()` reports bytes moved, allowing a caller to stop pumping when an exchange stops making progress.

## Content decoding and streaming

`h1_decode_body()` applies Content-Encoding transformations in reverse order. It handles gzip, its `x-gzip` alias and deflate through the inflater in `rust/src/inflate.rs`. Gzip decoding checks the output CRC and size against the footer. Deflate decoding tries the zlib wrapper, then raw DEFLATE. `h1_accept_encoding()` returns `gzip, deflate`.

A caller can register a body sink with `h1_response_sink()`. With absent or identity Content-Encoding, the parser removes HTTP framing and delivers entity bytes as they arrive. This supports responses such as Server-Sent Events. The consumer controls retained memory and applies backpressure by scheduling further pumps; the buffered body cap bounds accumulation inside the engine, as `c/net/http/http1.c` and `c/net/http/http1.h` implement.

The browser's HTTP/2 adapter exposes final headers and body delivery through `struct h1_response`. In `c/apps/gui/browser/app/runtime.c`, it transfers received HTTP/2 body buffers into that response buffer or its sink as the caller pumps the exchange.

## HTTP/2

The browser selects HTTP/2 through TLS ALPN. Its default TLS connection path offers `h2` and `http/1.1`; `bx_resolve_alpn()` selects the engine from the negotiated protocol. Plain HTTP connections use HTTP/1.1. These choices live in `c/apps/gui/browser/app/runtime.c`.

`h2_conn_start()` queues the client preface, local settings and an initial connection WINDOW_UPDATE. `h2_request()` allocates increasing odd stream identifiers and emits request pseudo-fields before regular headers. Request bodies travel in DATA frames. Pumping a connection advances the streams sharing it.

The receive path assembles HEADERS and CONTINUATION payloads before HPACK decoding. While a header block is open, the dispatcher requires CONTINUATION frames for the same stream. Response validation checks lowercase field names, pseudo-field placement, status syntax and prohibited connection fields. Validation failures reset the affected stream. HPACK decoding errors terminate the connection, including rejection of embedded NUL bytes, as `c/net/http/http2.c` and `c/net/http/hpack.c` implement.

Flow control maintains connection and stream windows in each direction. Sending DATA spends both send windows. Receiving DATA accounts for the complete frame payload, including padding, and replenishes credit as the engine accepts data. Successful stream completion flushes outstanding connection credit, preserving capacity for later streams.

The receive targets are 1 MiB for the connection and 256 KiB per stream. Settings announce the stream window; WINDOW_UPDATE raises the connection window. A configurable stall timeout defaults to 30 seconds and detects pending request data blocked by either send window when the connection has stopped making progress.

Stream admission observes both the local stream table and the peer's concurrency setting. The browser queues work while capacity is occupied.

GOAWAY begins connection retirement. Streams above the peer's last accepted identifier receive `H2_E_REFUSED`; streams at or below it can continue. The browser marks the pool entry as draining and retains active owners while replacement work obtains another connection, subject to pool capacity.

## HPACK

Each HTTP/2 connection owns an encoder and decoder whose dynamic tables persist across header blocks. Later indexed fields depend on earlier insertions, making this state part of the connection's lifetime.

The static table contains 61 entries. Dynamic entries cost name length plus value length plus 32 bytes. New entries receive the first dynamic index, and capacity pressure evicts the oldest entries. The code in `c/net/http/hpack.c` and `c/net/http/hpack.h` copies strings in `hpack_table_add()` before eviction because an inserted field can reference an entry that the insertion will evict.

The Huffman decoder uses canonical decoding tables, bounds decoded output and validates trailing padding. A symbol for end of string embedded in the data causes a decoding error. The encoder uses Huffman coding when it shortens a literal.

The HTTP/2 receive path decodes header blocks whose fields it will discard, including blocks for released streams and promised pushes. This keeps its dynamic table synchronized with the peer for subsequent indexed fields.

The encoder sends `authorization`, `proxy-authorization`, `cookie` and `set-cookie` as literals marked never indexed, keeping their values out of its dynamic table.

## Connection pooling

The pool key consists of host, port and TLS mode. Host comparison ignores ASCII case. Matching all three keeps connection reuse attached to the requested origin, including requests carrying cookies. The pool stores transport handles and invokes a closer supplied by the caller when removing entries, and `c/net/http/hpool.c` defines it.

HTTP/1.1 acquisition reserves an idle connection for one exchange. A reusable response returns it to the pool. Acquisition prefers the connection most recently made idle, reducing the chance of selecting one the server has already expired.

HTTP/2 acquisition reserves a stream on a shared connection. Releasing a reservation decrements its stream count; the connection becomes idle after the last reservation finishes. A pending protocol state holds additional pooled dials to that origin while ALPN resolves, so simultaneous requests can share the session once HTTP/2 is selected.

| Setting | Pool default | Browser configuration |
|---|---:|---:|
| Connection entries | 6 | 6 |
| Connections per origin | 4 | 2 |
| Idle expiry | 10 seconds | 60 seconds |
| HTTP/1.1 request recycling threshold | 100 | 100 |
| HTTP/2 reservations per connection | 16 | 16 |

The defaults come from `hpool_init()` in `c/net/http/hpool.c`; `bfetch_init()` changes the origin limit and idle expiry in `c/apps/gui/browser/app/runtime.c`. Its comments attribute these choices to the CPU cost of TLS handshakes under emulation and gaps between resource loading phases while the browser performs layout.

At capacity, the pool can evict an idle connection belonging to another origin. It prefers an HTTP/1.1 victim, preserving HTTP/2 sessions and their accumulated compression state when possible. Counters record hits, misses, opening, eviction and multiplexed reuse.

## URLs

`struct url` represents a transport endpoint: an HTTPS flag, host, port and request target. In `c/net/http/url.c`, `url_parse()` accepts the `http://` and `https://` prefixes, supplies default ports 80 and 443, supplies `/` for a missing path and removes the fragment from the request target. It publishes the parsed record after capacity checks succeed.

`url_resolve()` handles absolute references, references beginning with `//`, root relative references and references relative to the base directory. It retains navigation fragments in the absolute result. The browser wrapper in `c/apps/gui/browser/net/url.inc` also removes dot segments from paths while preserving query and fragment contents.

JavaScript URL objects use the parser in `c/apps/gui/browser/js/net/url.c`, with records and operations declared in `c/apps/gui/browser/js/net/url.h`. That layer provides component parsing, serialization, setters and the URL and URLSearchParams interfaces.

## Cookies

The jar in `c/net/http/cookies.c` stores name, value, domain, path, expiry and policy attributes. Operations receive a `struct cookie_ctx` describing the destination and whether access comes from HTTP or script. A `struct cookie_request` carries the site for cookies, its scheme and the navigation context.

`cookie_request_kind()` distinguishes requests within a site, requests across sites, safe navigation across sites and unsafe navigation across sites. Site comparison combines the scheme with a registrable domain lookup using the vendored ICANN and PRIVATE public suffix data in `c/net/http/cookie_psl.inc`.

Admission checks domain attributes, Secure, HttpOnly, SameSite and the `__Secure-` and `__Host-` prefixes. Retrieval filters cookies by domain, path, expiry and request policy. An absent SameSite attribute follows Lax retrieval rules. Max-Age takes precedence over Expires, and persistent lifetime is capped at 400 days. Default limits are 300 entries overall and 50 per stored domain.

Header serialization orders eligible cookies by longer path first, then earlier creation time. `cookie_header_with_diagnostics()` calculates the required space before serialization. Overflow returns `CK_E_NOFIT` with an empty output, allowing callers to fail an exchange before transmitting partial cookie state.

The browser shares its jar between fetch, `document.cookie` and page resource loading through `c/apps/gui/browser/js/runtime/webapi.c`. Integration in `c/apps/gui/browser/app/runtime.c` attaches cookies to navigation and resource requests and processes received Set-Cookie fields.

Embedded documents combine sandbox flags from the iframe attribute, inherited restrictions and enforced Content-Security-Policy response headers. `c/apps/gui/browser/frame/passive.c` combines these flags using `c/apps/gui/browser/frame/sandbox.h`; `c/apps/gui/browser/js/runtime/page.c` forwards the origin restriction before WebAPI installation.

For a document with an opaque origin, fetch resolves relative URLs against the document URL, sends `Origin: null` and suppresses Referer. Its requests use cross-origin credential rules. In CORS mode, response access requires the server's permission even when the destination matches the document URL's host. The default `same-origin` credentials mode withholds cookies, and both `document.cookie` accessors throw `SecurityError`. These checks live in `c/apps/gui/browser/js/runtime/webapi.c`.

The frame loader derives the site for cookies by comparing the document URL with the ancestor site. SameSite policy classifies the request's site context; fetch credentials decide whether credentials accompany it.

Persistence lives in `c/apps/gui/browser/storage/cookies.c`. Snapshots contain unexpired persistent cookies, and restore validates records against current cookie rules. When persistent content changes, flushing writes a snapshot with a CRC and generation number and verifies its bytes in both storage slots before reporting success. Updating both slots prevents recovery from an older slot from reversing a completed deletion.

## WebSockets

`c/net/http/ws.c` supplies handshake calculations, frame serialization, incremental parsing, UTF-8 validation and close code checks. The browser layer in `c/apps/gui/browser/js/net/websocket.c` owns sockets, assembles fragmented messages and dispatches JavaScript events.

Each WebSocket receives a dedicated socket because the upgraded connection carries its byte stream throughout its lifetime. WSS connections offer `http/1.1` through ALPN.

The opening exchange uses the HTTP/1.1 request builder and response parser. The browser encodes a random nonce as Sec-WebSocket-Key, requests version 13 and validates status 101, the Upgrade and Connection tokens, and Sec-WebSocket-Accept. Bytes read beyond the HTTP headers pass through the HTTP parser's spill buffer into the WebSocket parser before another socket read.

Outgoing client frames receive random masking keys. Incoming validation checks masking direction, opcodes, reserved bits and control frame size and fragmentation. The browser layer checks continuation sequencing, validates assembled text as UTF-8, answers ping with pong and manages the closing exchange.

The browser supports eight concurrent WebSocket connections, a 16 MiB assembled message and a 1 MiB outgoing buffer including frame overhead. Dialing has a deadline of 30 seconds; entering the handshake starts another deadline of 30 seconds, refreshed by incoming handshake bytes. Closing uses a deadline of five seconds. Open connections remain available during idle periods. `js_websocket_pump()`, called from `c/apps/gui/browser/js/runtime/page.c`, services their progress.

## Bounds and checks

The protocol headers define these defaults:

| Bound | Value | Definition |
|---|---:|---|
| HTTP/1.1 line length | 8 KiB | `H1_LINE_MAX` |
| HTTP/1.1 header section budget | 64 KiB | `H1_HDR_MAX` |
| HTTP/1.1 buffered body | 8 MiB | `H1_BODY_MAX` |
| HTTP/2 stream slots | 16 | `H2_MAX_STREAMS` |
| HTTP/2 assembled header block | 64 KiB | `H2_HEADER_BLOCK_MAX` |
| HTTP/2 buffered stream body | 16 MiB | `H2_BODY_MAX` |
| HPACK decoded header list budget | 32 KiB | `HPACK_LIST_MAX` |
| Browser cookie header buffer | 8 KiB | `CK_HEADER_MAX` |

These definitions are in `c/net/http/http1.h`, `c/net/http/http2.h`, `c/net/http/hpack.h` and `c/net/http/cookies.h`. The HPACK list budget counts name length, value length and 32 bytes per field. Callers can adjust body limits through `h1_response_limit()` and `h2_stream_limit()`; the browser applies its limits in `c/apps/gui/browser/app/runtime.c`.

The repository defines the following checks:

| Target | Coverage |
|---|---|
| `make test-browser` | HTTP/1.1 parsing and content decoding, cookie basics and pool behavior within the browser battery |
| `make test-h2` | HPACK vectors, HTTP/2 framing, streams and flow control |
| `make test-h2-control` | Mutation of HPACK table eviction order |
| `make test-h2mux` | Browser protocol selection, shared sessions, concurrency and connection lifetime, with prerequisite controls |
| `make test-cookie-jar` | Cookie policy, public suffix handling and prerequisite mutation controls |
| `make test-http-url` | Transport URL capacity checks and a prerequisite truncation control |
| `make test-ws` | WebSocket codec and browser integration, with prerequisite masking, handshake validation and termination controls |
| `make test-h2-os` | Guest HTTPS exchanges against public servers using HTTP/1.1 and HTTP/2 modes |
| `make test-frame-sandbox-host` | Embedded document sandbox policy, cookie access errors and outgoing Origin and Referer headers, with prerequisite controls |

Definitions live in `Makefile`, `tests/http2.mk`, `tests/cookiegate.mk`, `tests/http_url.mk`, `tests/ws.mk` and `tests/frame_sandbox.mk`. The guest HTTP harness is `tests/boot/run-h2-smoke.sh`. The sandbox host checks exercise browser integration through `tests/unit/passive_frame_test.c`.

## Limitations

- Compressed response decoding requires buffering through completion. Unsupported Content-Encoding values return `H1_E_ENCODING`.
- HTTP/2 server push is disabled; received promises are decoded with HPACK and refused. Stream priority information is discarded.
- The transport URL splitter has 128 bytes for the host and 512 bytes for the request target, including terminators. Its grammar covers fewer URL forms than the JavaScript parser.
- WebSockets omit extensions such as permessage-deflate, HTTP/2 extended CONNECT, handshake redirects and cookie jar integration.
- Cookie snapshot readback verifies stored bytes. Durability across a power cycle requires guest evidence beyond that check, as noted in `c/apps/gui/browser/storage/cookies.c`.
- `TRANSPARENCY.md` identifies the network and browser as experimental implementations. The browser lacks a sandbox suitable for hostile content; interoperability results apply to the behavior exercised in the named environments.

## Source map

| File | What it contains |
|---|---|
| `c/net/http/http.c`, `c/net/http/http.h` | Legacy kernel fetch client and API |
| `c/net/http/http1.c`, `c/net/http/http1.h` | HTTP/1.1 transport contract, request building, response parsing, streaming and decoding |
| `c/net/http/http2.c`, `c/net/http/http2.h` | HTTP/2 connections, streams, frames and flow control |
| `c/net/http/hpack.c`, `c/net/http/hpack.h` | HPACK tables, integer coding and Huffman coding |
| `c/net/http/hpool.c`, `c/net/http/hpool.h` | Origin matching, acquisition, release, retirement and expiry |
| `c/net/http/url.c`, `c/net/http/url.h` | HTTP endpoint parsing and reference resolution |
| `c/net/http/cookies.c`, `c/net/http/cookies.h` | Cookie admission, retrieval, limits and header serialization |
| `c/net/http/cookie_psl.inc` | Vendored public suffix lookup data |
| `c/net/http/ws.c`, `c/net/http/ws.h` | WebSocket handshake calculations, frame codec and validation helpers |
| `rust/src/inflate.rs` | Raw DEFLATE and zlib decoding |
| `c/apps/gui/browser/app/runtime.c` | Browser pooling, ALPN, requests and HTTP/2 adaptation |
| `c/apps/gui/browser/net/url.inc` | Browser transport URL resolution and path normalization |
| `c/apps/gui/browser/js/net/url.c`, `c/apps/gui/browser/js/net/url.h` | JavaScript URL parsing, serialization and interfaces |
| `c/apps/gui/browser/js/runtime/webapi.c` | Fetch policy, opaque origins and cookie integration |
| `c/apps/gui/browser/frame/passive.c`, `c/apps/gui/browser/frame/sandbox.h` | Embedded document sandbox flags and site for cookies |
| `c/apps/gui/browser/storage/cookies.c` | Cookie snapshot restore, writing and readback |
| `c/apps/gui/browser/js/net/websocket.c` | Browser WebSocket connections, messages and events |
| `c/apps/gui/browser/js/runtime/page.c` | Document sandbox setup and WebSocket pumping |
| `Makefile` | Product source lists and core HTTP test targets |
| `tests/http2.mk`, `tests/cookiegate.mk`, `tests/http_url.mk`, `tests/ws.mk` | Focused checks and mutation controls |
| `tests/frame_sandbox.mk`, `tests/unit/passive_frame_test.c` | Embedded document sandbox integration checks and controls |
| `tests/boot/run-h2-smoke.sh` | HTTP/1.1 and HTTP/2 guest interoperability harness |
| `TRANSPARENCY.md` | Project claim rules and security scope |

## See also

- [Network stack](../architecture/network-stack.md)
- [Sockets](sockets.md)
- [TCP and UDP](tcp-udp.md)
- [DNS](dns.md)
- [TLS and certificates](tls.md)
