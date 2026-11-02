# The ch chat client

`ch` is a native chat application that sends conversation history to a configured model endpoint and displays reply text as Server-Sent Events (SSE) arrive. It combines the asynchronous socket interface, the shared HTTP/1.1 client and the AUI window toolkit. Application state lives in `c/apps/gui/ch/ch.c`; SSE framing and JSON string extraction live in `c/apps/gui/ch/ch_sse.c`.

## Application structure

`app_main()` creates a window titled “Chat”, loads configuration, focuses the prompt field and enters an event loop. That loop handles input, advances the HTTP exchange and schedules drawing.

`tests/ch.mk` links the application and parser with mini-libc, AUI, the graphics library and `c/net/http/http1.c`. Its package list installs the application at guest path `/bin/ch.aex` and the configuration example at `/etc/ai.conf.example`; `Makefile` includes that list in the disk image. `fsroot/as/examples/chlaunch.as` provides a launcher through the desktop interface for opening paths.

The SSE parser uses fixed arrays and calls no allocation or libc functions. Its translation unit is shared by the application and host tests, allowing the framing logic to be exercised with different input boundaries.

## Endpoint configuration

`conf_load()` reads the guest file `/etc/ai.conf`. The assignments below come from `c/apps/gui/ch/ai.conf.example`:

```ini
host  = api.example.com
port  = 443
path  = /v1/chat/completions
tls   = 1
model = the-model-name
key   =
```

The host and model are placeholders.

| Setting | Meaning | Requirement or default |
|---|---|---|
| `host` | Destination host passed to the socket interface | Required and nonempty |
| `port` | TCP destination port | Defaults to 443 with TLS, otherwise 80; accepted range is 1–65535 |
| `path` | HTTP request target | Defaults to `/v1/chat/completions`; a supplied value must be nonempty |
| `tls` | Transport selection | Required; `1` selects TLS and `0` selects plain TCP |
| `model` | Model identifier placed in the request body | Required and nonempty |
| `key` | Bearer credential | Required assignment; an empty value selects an unauthenticated request |

The parser accepts blank lines, comments beginning with `#` after leading whitespace, and `name = value` assignments. It trims surrounding spaces and tabs and accepts CRLF line endings. Values are literal text, so quote characters and trailing comment text become part of a value. Unknown names and malformed assignments produce configuration errors. These rules are implemented in `c/apps/gui/ch/ch.c`.

Configuration is read at startup and before every send. An edit takes effect on the next message, which uses the new endpoint and model with the retained conversation. After a configuration error, the prompt controls remain available so the user can fix the file and retry.

## Requests and model endpoints

`send_prompt()` constructs an HTTP/1.1 `POST` to the configured path. `build_body()` supplies the configured `model`, `"stream":true` and a `messages` array containing retained, nonempty user and assistant turns. Application notices have a local role and are excluded from the array. `jesc()` escapes conversation text for JSON strings.

| Header | Value |
|---|---|
| `Host` | Configured host, with the port appended when it differs from the transport’s default |
| `Content-Type` | application/json |
| `Content-Length` | Serialized body length |
| `Accept` | text/event-stream |
| `Accept-Encoding` | `identity` |
| `Connection` | `close` |
| `Authorization` | `Bearer ` followed by the configured key, when nonempty |

The two media types in this table are HTTP header values. Their construction is in `c/apps/gui/ch/ch.c`.

Endpoint compatibility depends on the message shape used by chat completions: reply text comes from `choices[0].delta.content`, and `data: [DONE]` marks stream completion. Host, port, path and model all come from configuration.

For TLS, the application passes `SOCK_F_TLS | SOCK_F_ALPN_HTTP11` to the socket interface. `include/abi/logit_abi.h` defines these flags; `c/net/core/sock.c` advances connection establishment and the TLS handshake. The application maps socket errors to messages about name resolution, connection failure, TLS refusal or exhausted socket resources.

## Credentials

The comment in `c/apps/gui/ch/ch.c` explains the choice of a configuration file: the settings store exposes enumeration of keys and values. `settings_kv_at()` in `c/kernel/core/settings.c` includes application settings in that enumeration. The file keeps the credential out of that list.

The application reads the key into configuration storage and copies it into the authorization header during request construction. Its startup and stream diagnostics show authentication as `bearer` or `none`; the window header shows the endpoint, model and authentication mode. Configuration errors report a line number and cause, with the setting name included for an unknown name.

Correction, 2026-11-02: the opening comment in `c/apps/gui/ch/ch.c` and the configuration example describe file modes as volatile. Current `c/fs/logitfs/logitfs.c` persists mode, owner and group through its attribute operations. `vfs_read()` in `c/fs/vfs/vfs.c` checks read access through `vmeta_permission()` in `c/fs/vfs/vfs_meta.c`. LogitFS supplies mode `0644` and root ownership for regular files whose attributes have never been set.

## Receiving the stream

The receive path divides the work across three components:

| Component | Responsibility |
|---|---|
| Socket adapters in `c/apps/gui/ch/ch.c` | Adapt asynchronous socket reads, writes and readiness to the HTTP transport interface |
| `c/net/http/http1.c` | Parse HTTP responses and remove transfer framing, including chunked encoding |
| `c/apps/gui/ch/ch_sse.c` | Assemble SSE records and extract decoded content strings |

The application registers `body_sink()` with `h1_response_sink()` before pumping the exchange. For status 200, the sink passes entity bytes to `sse_feed()`. It reads the status from the HTTP response object because a single pump call can finish headers and deliver body bytes before returning to the application.

The SSE parser preserves partial input between calls. Bytes accumulate into lines, lines accumulate into records, and a blank line dispatches a record. This allows JSON strings, UTF-8 sequences and line endings to span socket reads or HTTP chunks.

Framing accepts CRLF, LF and bare CR terminators. It strips an initial UTF-8 byte order mark, counts comment lines and joins successive `data:` fields with newline characters. Role records and records with null content contribute to record accounting; decoded content strings reach the transcript callback.

The JSON reader walks object members and array elements to find `choices[0].delta.content`. It decodes string escapes, combines Unicode surrogate pairs and replaces unpaired surrogates with U+FFFD. Structural traversal lets a reply contain quoted JSON text without confusing that text with the response’s fields.

An oversized line is discarded and marks its record for rejection. Record overflow, JSON traversal failure and content-decoding errors invoke the refusal callback and update counters. Parsing continues with later records.

At exchange completion, `sse_eof()` flushes a pending final record and checks for `[DONE]`. `finish()` reports an incomplete reply when that marker is absent, or notes refusals in a completed response.

For statuses other than 200, `body_sink()` retains an error-body prefix. `finish()` uses `sse_json_string()` to extract `error.message`. Status 401 receives a message that the credential was rejected, 429 receives a message about rate limiting, and other statuses are shown numerically. These paths are in `c/apps/gui/ch/ch.c`.

## Interface and event flow

The window contains an endpoint header, a scrolling transcript, a status strip and a prompt field with a Send button. Transcript labels distinguish “You”, “Assistant” and “ch”.

Pressing Enter or clicking Send submits a nonempty prompt. After configuration succeeds, the application appends the user turn, opens an assistant turn and starts the request. During connection establishment and streaming, the prompt controls are disabled and the status strip displays a spinner and a count of content deltas.

Incoming content marks the window dirty. During an exchange, the main loop spaces frames requested by that flag using `CH_REPAINT_MS`, currently 40 ms. Input handling can draw frames, and toolkit animation deadlines schedule further frames. Exchange completion requests a final repaint.

Wrapping advances as words complete. The paint loop draws the visible line range and the open turn’s live tail; resizing rebuilds wrapping from retained turn records. Wheel input stops automatic following, and submitting another prompt enables it again.

During an exchange, the loop uses a 10 ms socket-pump deadline. An idle window waits for input or toolkit animation work. Closing the window releases the HTTP exchange and socket before exiting. This flow is implemented in `c/apps/gui/ch/ch.c`.

The interface labels its counter “tokens”. The underlying `n_deltas` field counts successfully decoded content-string records, including empty strings. It measures protocol deltas, not provider tokenizer units.

## Limitations

Storage bounds come from `c/apps/gui/ch/ch.c` and `c/apps/gui/ch/ch_sse.h`. String lengths count bytes.

| Resource | Bound |
|---|---|
| Configuration file | Fewer than 4096 bytes |
| Host, path, model and key values | 127, 191, 63 and 511 bytes respectively |
| Prompt text | 1023 bytes plus a terminator |
| Transcript storage | 32 KiB |
| Retained turn records | 32, shared by conversation turns and application notices |
| Wrapped display-line records | 2048 |
| Request-body buffer | 24 KiB, including termination space |
| Retained HTTP error-body text | 2047 bytes plus a terminator |
| SSE line | 4096 bytes, excluding its terminator |
| SSE record data storage | 16 KiB, including appended line separators |
| Decoded content string | 8191 bytes plus a terminator |
| JSON traversal depth | 24 |

- Conversation storage is held in process memory. Dropping an old turn record leaves its transcript bytes allocated. Request construction can omit older turns to fit the body buffer. Turn loss, transcript exhaustion and exhaustion of the line table produce window warnings.
- The interface handles one request at a time and displays plain text. System prompts, temperature controls, tool calls, attachments and Markdown rendering are absent.
- The parser extracts the first choice’s content string. Named-event dispatch, retries, reconnection and stream resumption are absent. Its JSON scanner uses permissive scalar skipping and compares member names as raw bytes. The configured model name is inserted into JSON without escaping, so quotes, backslashes or control characters can produce an invalid request.
- Streaming requires identity content encoding. A compressed response enters the HTTP buffering path, and `ch` does not decode or consume that buffered body. Completion waits for the HTTP exchange to terminate; the application gives up after more than 30 seconds without HTTP byte progress.
- Credentials remain plaintext in the file and application buffers. The application provides no permission setup or secure buffer erasure. With `tls = 0`, a configured bearer credential travels over plain TCP. VFS permissions govern file access, and root retains read access.

## Development checks

The coverage below was established by source inspection on 2026-11-02. Tests were not run for this review.

| Target in `tests/ch.mk` | Harness coverage |
|---|---|
| `test-ch-host` | Replays SSE input whole, byte by byte, at every split into two parts and with chunk sizes 2–64; checks framing, decoded output, truncation and selected refusal cases |
| `test-ch-negctl` | Compiles a parser variant that discards partial lines between feeds and requires its test program to fail |
| `test-ch` | Depends on both host checks, then drives the application in QEMU against a mock endpoint |
| `test-ch-refusal` | Exercises missing and malformed configuration through the QEMU harness |

`tests/unit/ch_sse_test.c` contains the parser assertions. `tests/qmp/qmp_ch.py` checks screen changes during streaming, guest counters, a reply hash, repaint accounting, request contents, HTTP errors, truncation, TLS refusal and closing during a stream. Its credential check places a canary in the key field and searches application serial output produced during a 401 exchange for that value.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/ch/ch.c` | Configuration, request construction, transcript storage, network state and window flow |
| `c/apps/gui/ch/ch_sse.h` | Parser interface, fixed bounds, callbacks and counters |
| `c/apps/gui/ch/ch_sse.c` | SSE framing, JSON traversal, string decoding and completion checks |
| `c/apps/gui/ch/ai.conf.example` | Endpoint configuration example |
| `c/apps/gui/aui_fields.inc` | Prompt-field editing and Enter handling |
| `c/net/http/http1.h` | HTTP transport and response interfaces |
| `c/net/http/http1.c` | Request serialization, response framing, body sinks and exchange pumping |
| `c/net/core/sock.c` | Asynchronous connection and TLS progression |
| `include/abi/logit_abi.h` | Socket flags and error definitions |
| `c/kernel/core/settings.c` | Settings enumeration |
| `c/fs/vfs/vfs.c` | File reads and permission-check integration |
| `c/fs/vfs/vfs_meta.c` | Permission decisions |
| `c/fs/logitfs/logitfs.c` | Persistent ownership and mode attributes |
| `Makefile` | Disk-image integration and test-fragment inclusion |
| `tests/ch.mk` | Application build, installation paths and test targets |
| `tests/unit/ch_sse_test.c` | Parser replay and refusal assertions |
| `tests/qmp/qmp_ch.py` | Mock endpoint and application interaction checks |
| `fsroot/as/examples/chlaunch.as` | Desktop launch helper |

## See also

- [Desktop applications](desktop-apps.md)
- [Sockets](../networking/sockets.md)
- [HTTP](../networking/http.md)
- [TLS and certificates](../networking/tls.md)
- [Virtual file system](../filesystems/vfs.md)
