# Aether OS — M11: HTTP + Browser app (design)

**Date:** 2026-06-01
**Status:** design approved; implementing
**Arc:** browser milestone 2 of 4 (M10 TCP ✅ · **M11 HTTP+render** · M12 TLS · M13 layout)

## Goal

First "it loads a web page": a kernel HTTP/1.0 GET client over the M10 TCP stack
+ a ring-3 **Browser** app with an address bar that fetches `http://example.com`,
shows the de-tagged body text, and lets you click extracted links to navigate.
Plain HTTP only — `https://` is M12 (TLS).

## Decisions (locked)
- **HTTP lives in the kernel** as an all-in-one fetch (TCP's blocking API runs in
  kernel context). The app drives it via async syscalls, exactly like ping/DNS.
- **Render = de-tag body + clickable link list.** Kernel produces a marked-up
  plain-text rendering the app reads in one shot; links are a separate table.
- **Address bar input** (`http://host[:port]/path`); relative links resolved by
  the kernel to absolute URLs.

## Architecture / data flow

```
Browser app (user/browser.c)
  - address bar (type URL, Enter) -> SYS_HTTP_GET(url)
  - poll SYS_HTTP_STATUS (0 in-progress / 1 done / <0 error)
  - on done: SYS_HTTP_READ(buf,max) copies the rendered text; draw line-by-line
    with scroll (terminal.c render model); link lines shown highlighted
  - click a link line -> SYS_HTTP_LINK(n) returns its absolute URL -> SYS_HTTP_GET it
net/http.c (kernel)
  - http_get(url): parse http://host[:port]/path; dns_resolve(host);
    tcp_connect(ip,port); send "GET <path> HTTP/1.0\r\nHost: ..\r\n\r\n";
    tcp_recv until close; locate body after the blank line.
  - http_poll(): step the fetch state machine (pumped from net_poll).
  - html_render(body): strip <script>/<style> wholesale + other <...> tags,
    decode entities (&amp; &lt; &gt; &quot; &nbsp; &#NN;), collapse whitespace,
    map <p>/<br>/<div>/</hN> to newlines; extract <a href> into a link table and
    insert an inline "[n]" marker in the text. Output: rendered text buffer +
    link[] {text, absolute_url}.
```

State machine mirrors dns: `SYS_HTTP_GET` kicks it off (non-blocking), the work
advances in `http_poll()` on the WM loop, the app polls status.

## Components
- **`net/url.c` / `include/url.h`** — `url_parse(url) -> {scheme,host,port,path}`
  and `url_resolve(base, ref) -> absolute` (handles `/abs`, `rel`, full URLs).
- **`net/http.c` / `include/http.h`** — fetch state machine + `html_render`.
  Buffers (static): raw response ≤64 KiB, rendered text ≤32 KiB, ≤64 links.
- **ABI (`include/aether_abi.h`)** — `SYS_HTTP_GET (26)`, `SYS_HTTP_STATUS (27)`,
  `SYS_HTTP_READ (28)`, `SYS_HTTP_LINK (29)`; helpers in `user/aether.h`. Routed in
  `wm_gui_syscall` (kernel/wm.c), like the SYS_NET_* block.
- **`user/browser.c`** — the app; Makefile APP_RULE (base 0x45000000) + Dock.

## Reuse
- `tcp_connect/send/recv/close/alive` (`net/tcp.c`), `dns_resolve` (`net/dns.c`),
  `net_poll` pump (`net/net.c`), `timer_ticks` time base.
- Text render/scroll pattern from `user/terminal.c`; net-syscall routing pattern
  from the SYS_NET_* cases in `kernel/wm.c`.

## Error handling
- DNS fail / connect fail / non-2xx / oversize body -> `SYS_HTTP_STATUS` < 0 with
  a code; app shows an error line. Bodies past the cap are truncated. Fetch runs
  with interrupts enabled (M9/M10 time-base lesson).

## Verification (staged, pcap + serial markers; reuse M9/M10 harness)
1. **L1 fetch:** kernel self-test `http_get("http://example.com")`; serial prints
   the response's first line (expect `HTTP/1.x 200`) -> `AETHER_HTTP_FETCH_OK`.
2. **L2 render:** print the rendered body's first non-empty line + link count
   -> `AETHER_HTTP_RENDER_OK` (example.com has one link, to iana.org).
3. **L3 app:** QMP — launch Browser, address bar already `example.com`, Enter,
   confirm body text renders; click the link line, confirm it navigates
   (`SYS_HTTP_LINK` -> new GET); screenshot.
4. `make test` stays green (`AETHER_BOOT_OK`).

## Build order (each builds + boots; temp self-tests removed before final commit)
1. `net/url.c` + `net/http.c` fetch (GET, body extraction) + self-test.
2. `html_render` (tags/entities/links) + self-test.
3. ABI syscalls + `user/browser.c` (address bar, render, link navigation) + docs.

## Risks / notes
- example.com is small, plain-HTTP, returns 200 with one `<a>` link — ideal.
- HTTP/1.0 + `Connection: close` avoids chunked transfer-encoding; if a server
  sends chunked anyway, de-chunk is out of scope for M11 (example.com doesn't).
- Out of scope: HTTPS (M12), real CSS/box layout (M13), forms/JS, images.

## Result — implemented & verified

All three layers shipped:
- **L1 fetch:** `url_parse`/`url_resolve` (`net/url.c`) + `http_get` (`net/http.c`)
  resolve DNS, TCP-connect, send `GET / HTTP/1.0`, receive to a buffer. Kernel
  self-test printed the real body (`<!doctype html>...Example Domain...`) ->
  `AETHER_HTTP_FETCH_OK`.
- **L2 render:** `html_render` (`net/html.c`) stripped tags (incl. script/style
  bodies), decoded entities, collapsed whitespace, and extracted the `<a>` link
  resolved to absolute -> rendered "Example Domain" + 1 link
  `https://iana.org/domains/example` -> `AETHER_HTTP_RENDER_OK`.
- **L3 app:** `SYS_HTTP_GET/STATUS/READ/LINK` + `user/browser.c` (address bar,
  scrolled text, blue clickable link lines). Verified over QMP: the Browser
  loads `http://example.com` and renders "Example Domain / This domain is for
  use in documentation examples... / Learn more" with status "loaded".

**Root cause found during L3** (systematic debugging via serial trace + pcap):
`SYS_HTTP_GET` ran with IF=0 because int 0x80 is an interrupt gate, so
`http_get`'s blocking loop (which pumps `net_poll` and times out on the PIT)
spun forever with a frozen `timer_ticks()` and emitted zero packets. Fix: the
syscall re-enables interrupts around the blocking fetch (the calling app is
parked; only the WM thread runs), then restores IF for the `iretq`. This is the
general rule for any blocking net op invoked from a syscall.

Next: **M12 TLS 1.3** (X25519 + ChaCha20-Poly1305 + SHA-256/HKDF) so `https://`
links — like the one example.com points at — actually open.
