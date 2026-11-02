# Storage, crypto, text and media APIs

The browser combines native C services with JavaScript installed into QuickJS contexts. Local storage and persistent cookies use filesystem snapshots, session storage follows a tab’s lifetime, and IndexedDB and CacheStorage keep their data in the page context. Cryptographic bindings call the project’s primitives, encoding APIs convert between strings and bytes, and media bindings connect DOM elements to demuxers, decoders, audio output and painting.

## Installation and ownership

`c/apps/gui/browser/js/runtime/page.c` installs APIs in dependency order. The Web API layer supplies storage, cookies, encoding and fetch objects. The platform layer creates `crypto` and supplies facilities such as `DOMException` and `structuredClone()`. SubtleCrypto extends `crypto`; IndexedDB follows the event and cloning facilities; CacheStorage follows the fetch interfaces.

Object behavior lives largely in JavaScript embedded in C string literals. Native functions provide filesystem access, cryptographic computation and media processing. Both layers determine the behavior visible to a page.

## Web Storage

`localStorage` and `sessionStorage` share `c/apps/gui/browser/storage/backend.c`. Their bindings in `c/apps/gui/browser/js/runtime/webapi.c` expose `getItem()`, `setItem()`, `removeItem()`, `clear()`, `key()` and `length`.

For HTTP and HTTPS documents, local areas are keyed by origin. Session areas also include a tab identifier. `c/apps/gui/browser/app/main.c` retains that identifier across navigation and runtime teardown, then drops its session areas when the tab closes. Each storage wrapper owns its partition key, so releasing and reusing a backend slot preserves the wrapper’s association with its origin.

The limits in `c/apps/gui/browser/storage/backend.h` apply across the browser process:

| Resource | Limit |
|---|---:|
| Areas across local and session storage | 16 |
| Entries per area | 256 |
| Encoded key and value bytes per area | 256 KiB |

Keys and values carry byte lengths, preserving embedded NUL characters. Mutations allocate replacement data and check quota before committing. Quota failures raise `QuotaExceededError`. Accessing either storage property from a document marked with an opaque origin raises `SecurityError`.

Older comments in `c/apps/gui/browser/js/runtime/webapi.c` describe local storage and cookies as backed by memory for the browser process lifetime. Dated corrections and the startup code in `c/apps/gui/browser/app/main.c` establish the current filesystem adapters before page loading.

For local storage, `c/apps/gui/browser/storage/persistence.inc` writes two versioned snapshots with generation numbers and CRC32 checks. A mutation writes the alternate slot and verifies a complete readback before publishing the candidate map in memory. Opening the store selects the valid snapshot with the highest generation.

This scheme fits the adapter’s operations on whole files. The implementation documents that rename replacement is unavailable, which explains the alternating snapshots. A failed write or readback latches an error until reopening; later mutation attempts report `InvalidStateError`.

## Cookies

Script access and network requests share one cookie jar for the browser process. `c/apps/gui/browser/js/runtime/webapi.c` supplies the host, path, transport security and request context to `c/net/http/cookies.c`.

The jar implements domain and path matching, expiry, Secure and HttpOnly attributes, cookie name prefixes and SameSite rules. Domain admission and site comparison share public suffix data. Request classification includes the initiating site, scheme, navigation status and method.

`document.cookie` selects script access rules in the jar. These rules filter HttpOnly cookies and protect them against replacement by script. Fetch retains the initiating request’s cookie context for attaching request cookies and accepting response cookies.

The defaults in `c/net/http/cookies.h` allow 300 cookies overall and 50 per domain. Names and values together have a 4,096-byte limit, and persistent expiry is capped at 400 days. Header construction reports overflow as an error, enabling callers to reject an incomplete header.

`c/apps/gui/browser/storage/cookies.c` serializes persistent, unexpired entries and validates restored records against current cookie policy. When a persistent change alters stored state, the code writes and verifies the new generation in both slots. Writing both copies prevents an older backup from restoring a deleted cookie.

Cookie snapshots use the private adapter in `c/apps/gui/browser/storage/cookie_guest.inc`. It checks ownership, file type and durable permission attributes, enforcing directory mode `0700` and file mode `0600`. The checks reject symbolic links and regular files with multiple links.

## IndexedDB

`c/apps/gui/browser/js/storage/idb.c` implements databases, transactions, object stores, indexes, key ranges, requests and cursors in JavaScript. A map inside the installed closure owns the databases for that context.

Stored values pass through `structuredClone()` on insertion and retrieval. Key comparison orders numbers, dates, strings, binary values and arrays. Object stores support key paths and automatic key generation; indexes support unique constraints and multiple entries. Cursors rebuild their candidate lists from current records as they advance.

Record operations compute their results during the API call and defer request events through `setTimeout()`. The scheduling helper queues delivery before running the operation, starting with an error payload. Successful computation replaces that payload; exceptions select the error event. Delivery checks transaction completion and converts an outstanding request to an abort error when appropriate.

Transactions count outstanding requests and schedule completion after that count reaches zero. Record mutations enter an undo log that is applied in reverse order during abort. During an upgrade, creating or deleting an object store also updates the transaction’s store scope.

Open connections participate in version changes. An upgrade or deletion sends `versionchange` events and reports `blocked` while connections remain open. Closing connections allows queued work to resume.

## CacheStorage and service worker registration

`c/apps/gui/browser/js/storage/cache.c` implements `caches` and named `Cache` objects over the existing `Request`, `Response`, `Headers` and `fetch` interfaces.

Entries retain request headers, response metadata and body bytes. Matching handles `Vary` and the `ignoreSearch`, `ignoreMethod` and `ignoreVary` options. A match constructs a response with a copy of the stored body, giving each returned response its own consumption state.

`put()` validates the request and response before reading the body. Checks cover the request method and scheme, partial responses, `Vary: *`, and bodies that are used or locked. `add()` and `addAll()` fetch their inputs and require successful response status. The default quota covers 64 MiB of response body bytes across the context’s named caches; exceeding it rejects with `QuotaExceededError`.

`c/apps/gui/browser/js/storage/swreg.c` installs `navigator.serviceWorker`. Registration validates the document’s trustworthy origin, resolves script and scope URLs, compares their origins and checks scope placement beneath the script directory. Query methods resolve to an empty registration list or an undefined registration. Execution limits appear below.

## Digests and SubtleCrypto

`c/apps/gui/browser/js/crypto/subtle.c` owns algorithm normalization, parameter validation, key authorization and Promise results. The included `c/apps/gui/browser/js/crypto/crypto_keys_script.inc` replaces earlier method definitions with key generation, asymmetric operations and DER import and export.

| Operation | Implemented algorithms |
|---|---|
| Digest | SHA-1, SHA-256, SHA-384, SHA-512 |
| Encryption and decryption | AES-GCM with 128, 192 or 256 bit keys |
| Signing and verification | HMAC with SHA-256, SHA-384 or SHA-512; ECDSA with those digests; Ed25519 |
| Key derivation | HKDF and PBKDF2 with SHA-256, SHA-384 or SHA-512; ECDH; X25519 |
| Key generation | AES and HMAC secret keys; ECDSA, ECDH, Ed25519 and X25519 key pairs |

EC operations support P-256, P-384 and P-521. Native helpers call the project’s hash, AES, HMAC and public-key implementations.

Digest input is copied using the supplied buffer view’s offset and length before entering native code. `c/apps/gui/browser/js/crypto/digest.inc` dispatches to the selected hash primitive and returns an `ArrayBuffer`.

`CryptoKey` metadata lives in a closure’s `WeakMap`. Authorization uses stored algorithm and usage records, preserving permissions when script mutates the public `algorithm` or `usages` objects. Export checks the stored extractability flag.

Key formats depend on the algorithm:

| Key material | Import and export formats |
|---|---|
| AES and HMAC secret keys | Raw and JWK |
| HKDF and PBKDF2 base keys | Raw import, with extractability set to false |
| EC, Ed25519 and X25519 public keys | Raw, JWK and SPKI |
| EC, Ed25519 and X25519 private keys | JWK and PKCS8 |

The DER reader checks lengths, algorithm identifiers, curves and complete consumption. Native validation checks EC points and agreement between imported private and public material.

AES-GCM accepts an IV and optional additional authenticated data. Encryption returns ciphertext followed by its authentication tag. The native binding accepts IV lengths from 1 to 1,024 bytes and uses a 128 bit tag.

The Promise executors perform native computation synchronously. Bounds in `c/apps/gui/browser/js/crypto/crypto_ops.inc` cap derived output at 16 MiB, PBKDF2 work at four million HMAC invocations and HKDF output at 255 hash blocks.

## Entropy contract

`c/apps/gui/browser/js/crypto/entropy.h` supplies the shared entropy helper for random filling and key operations. In the guest, it queries `getrandom_strong()` before filling the buffer through `getrandom_bytes()`.

The filling wrapper in `c/apps/logit.h` loops over short syscall results. The kernel limits each request to 4,096 bytes, so this loop matters for larger browser requests. In `c/kernel/core/rng.c`, a reseed marks the generator as strong after at least two successful 64 bit hardware samples from RDSEED or RDRAND have entered its state.

`crypto.getRandomValues()` fills the selected view over an integer typed array and returns that view. Requests above 65,536 bytes raise `QuotaExceededError`; failure to obtain strong entropy raises `OperationError`. `randomUUID()` uses the same native filling path and sets the UUID version and variant bits. These bindings live in `c/apps/gui/browser/js/runtime/platform.c`.

Key generation uses the shared entropy helper. ECDSA signing and ECDH private operations also request blinding input through it. Deriving EC public material during import of a private key crosses the same entropy check.

## Encoding and internationalization

Encoding combines native table lookup with JavaScript decoder state machines. `c/apps/gui/browser/js/text/encoding.inc` canonicalizes labels using ASCII case conversion and the encoding standard’s whitespace set. Invalid labels, including labels containing embedded NUL characters, produce `RangeError`.

`TextDecoder` implements UTF-8, both UTF-16 byte orders, generated single byte mappings and `x-user-defined`. The shared driver in `c/apps/gui/browser/js/text/encoding_prelude.inc` carries partial sequences across streaming calls, handles byte order marks and selects replacement characters or exceptions according to `fatal`.

`TextEncoder` produces UTF-8. Its encoding methods share scalar conversion, replacing lone surrogates with U+FFFD. `encodeInto()` writes complete encoded characters and reports UTF-16 code units read and bytes written. When `TransformStream` is available, the layer also installs `TextEncoderStream` and `TextDecoderStream`.

`c/apps/gui/browser/js/text/intl.c` supplies English number, date, plural, list and relative time formatting, plus `Intl.Collator`. Number formatting includes grouping, fractional digits, percentages and currency symbols. Plural rules cover English cardinal and ordinal selection. Number and date locale methods on the built-in prototypes route through the same formatting functions.

## Media elements and Media Source

`c/apps/gui/browser/js/media/media.c` owns JavaScript objects, DOM accessors, event delivery and the adapter to the operating system. `c/apps/gui/browser/js/media/media_src.c` owns media bytes, demuxers, decoders and playback timing. The `media_platform` callback table supplies drawing, monotonic time and audio output.

Media accessors check the element’s tag. Each audio or video element receives an integer identifier in `data-logit-mediaid`. The painter uses that identifier to associate the DOM box with playback state through the hook in `c/apps/gui/browser/paint/renderer.c`.

For a network `src`, the binding fetches the response body and passes its bytes to the engine. A generation check discards responses superseded by a newer source assignment. Sources supplied in markup are applied when `play()` or `load()` runs.

Media Source uses growing buffers. Appended bytes extend a buffer, and a box walker identifies the complete MP4 prefix. The engine reopens the demuxer over that prefix when playback or range queries need updated information. Sequence mode reparses during append to calculate the placement of newly appended samples.

`SourceBuffer` supplies appending, range removal, abort, timestamp offsets and segment or sequence modes. After a successful append, the binding sets `updating` and fires `updatestart`. The media pump later clears that state and delivers `update` and `updateend`. Attaching a MediaSource queues `sourceopen`, allowing script to attach its listener before delivery.

The shared object URL implementation in `c/apps/gui/browser/js/net/fetch_object_url_prelude.inc` manages Blob and File URLs and delegates MediaSource objects to the media bindings.

Both `MediaSource.isTypeSupported()` and media element `canPlayType()` use `mse_type_supported()`:

| Declaration | Accepted by the capability query |
|---|---|
| H.264 in MP4 | `avc1` or `avc3` with Baseline, Main or High profile and a level value through 5.1 |
| AAC in MP4 | `mp4a.40.2` and `mp4a.40.5` |
| MP3 in MP4 | `mp4a.40.34` and accepted MP3 object-type aliases |
| Bare audio MIME types | “audio/mpeg” and “audio/aac” |

MP4 queries require a `codecs` parameter, and every codec listed in it must pass the policy. These MIME values describe the query interface; the engine opens supplied bytes through its media processing path.

A timer chain drives playback. With audio output active, the engine derives its audio clock from frames played by the device. Nonblocking sound writes allow the same browser thread to continue decoding, running callbacks and handling input. Due video frames are painted into the rectangle recorded by the renderer.

The markup source path also discovers default `<track>` children for subtitles or captions. Their bytes are parsed as WebVTT or SRT through `c/lib/media/subs.c`. The engine selects cues at the current picture time, and the bindings draw the text over the video.

## Limitations

- Local storage snapshots assume one browser process writing the store. The adapter conflates missing files and read failures. Failed writes can have an uncertain disk outcome; the previous map stays in memory until reopening. See `c/apps/gui/browser/storage/persistence.inc`.
- Cookie persistence errors are logged after the jar mutation. The jar keeps the accepted change in memory, and the persistence error blocks further flushes until reopening. Session cookies live for the browser process lifetime.
- IndexedDB and CacheStorage disappear with their page context, and Worker installation omits both APIs. IndexedDB rejects Blob and File values at insertion. Transaction activity checks use completion state; abort undo covers record mutations, with schema changes and automatic key counters outside that undo log. See `c/apps/gui/browser/js/storage/idb.c` and `c/apps/gui/browser/js/workers/worker.c`.
- Service worker registration rejects with `NotSupportedError` after validation. The implementation covers registration only, and `controller` is always null. `ready` also rejects, a documented specification deviation chosen to let callers finish waiting. See `c/apps/gui/browser/js/storage/swreg.c`.
- RSA computation, key wrapping and unwrapping, AES cipher modes other than GCM, SHA-1 HMAC and KDF operations, SHA-1 ECDSA operations, and truncated GCM tags fall outside the computation backends. Algorithm recognition and key import can cover more names than the computation backends.
- Legacy multibyte CJK decoding falls back to UTF-8 and emits a warning. Internationalization applies English rules to every locale argument. Date formatting uses local `Date` accessors while reporting `UTC`; collation uses string comparison with limited case and numeric handling. See `c/apps/gui/browser/js/text/encoding_prelude.inc` and `c/apps/gui/browser/js/text/intl.c`.
- Network media loading buffers the complete resource before decoding. Media buffers accept up to 48 MiB each. Range removal retains the bytes, and repeated parsing of growing prefixes can accumulate quadratic work. Capability queries reject HEVC, AV1 and WebM declarations.
- Playback runs at rate 1, and the `play()` Promise resolves immediately. `TextTrack` and advanced Media Source operations fall outside the implemented set. The engine ignores the installed `appendWindowStart` and `appendWindowEnd` properties. See `c/apps/gui/browser/js/media/media.c`.
- `SECURITY.md` and `TRANSPARENCY.md` classify the browser and cryptography as experimental components unsuitable for hostile content or valuable secrets.

## See also

- [The JavaScript runtime](javascript-runtime.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [Workers, streams and WebAssembly](workers-streams-wasm.md)
- [Browser security model](security-model.md)
- [Testing the browser](testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/js/runtime/page.c` | Page API installation order |
| `c/apps/gui/browser/js/runtime/webapi.c` | Storage bindings, cookie integration and encoding installation |
| `c/apps/gui/browser/app/main.c` | Filesystem adapters, startup restoration and tab session ownership |
| `c/apps/gui/browser/storage/backend.h` | Storage partition keys, limits and backend interface |
| `c/apps/gui/browser/storage/backend.c` | Storage areas and checked mutations |
| `c/apps/gui/browser/storage/persistence.inc` | Local storage snapshots, recovery and commits |
| `c/net/http/cookies.h` | Cookie limits and request context |
| `c/net/http/cookies.c` | Cookie admission, matching, expiry and header construction |
| `c/apps/gui/browser/storage/cookies.c` | Persistent cookie snapshots and flush behavior |
| `c/apps/gui/browser/storage/cookie_guest.inc` | Private guest filesystem adapter for cookies |
| `c/apps/gui/browser/js/storage/idb.c` | IndexedDB records, transactions, upgrades and cursors |
| `c/apps/gui/browser/js/storage/cache.c` | CacheStorage, matching and body quota |
| `c/apps/gui/browser/js/storage/swreg.c` | Service worker registration validation and rejection |
| `c/apps/gui/browser/js/workers/worker.c` | Worker API installation |
| `c/apps/gui/browser/js/runtime/platform.c` | Random filling and UUID bindings |
| `c/apps/gui/browser/js/crypto/subtle.c` | SubtleCrypto interface, normalization and key authorization |
| `c/apps/gui/browser/js/crypto/digest.inc` | Native digest dispatch |
| `c/apps/gui/browser/js/crypto/crypto_ops.inc` | HMAC, KDF and AES-GCM operations |
| `c/apps/gui/browser/js/crypto/crypto_keys.inc` | Native key operations and random generation |
| `c/apps/gui/browser/js/crypto/crypto_keys_script.inc` | Key generation, asymmetric operations and DER formats |
| `c/apps/gui/browser/js/crypto/entropy.h` | Shared entropy checks |
| `c/apps/logit.h` | Guest random syscall wrappers |
| `c/kernel/core/rng.c` | Kernel generator, entropy status and syscall bounds |
| `c/apps/gui/browser/js/text/encoding.inc` | Encoding label and index lookup |
| `c/apps/gui/browser/js/text/encoding_prelude.inc` | Encoders, decoders and stream adapters |
| `c/apps/gui/browser/js/text/intl.c` | Formatting and comparison interfaces |
| `c/apps/gui/browser/js/media/media.c` | Media bindings, fetching, events and platform adapter |
| `c/apps/gui/browser/js/media/media.h` | Media engine interfaces and platform callbacks |
| `c/apps/gui/browser/js/media/media_src.c` | Media buffers, capability queries, playback and cue selection |
| `c/apps/gui/browser/js/net/fetch_object_url_prelude.inc` | Object URL registry and media delegation |
| `c/apps/gui/browser/paint/renderer.c` | Media painting hook |
| `c/lib/media/subs.c` | WebVTT and SRT parsing |
| `SECURITY.md`, `TRANSPARENCY.md` | Security guidance and claim limits |
