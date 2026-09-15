# WebCrypto and localStorage expansion — 2026-09-09

Implemented in the isolated `/tmp/logitos-browser-expand-0909` checkout. This
records the module evidence; it does not substitute host results for a booted
`browser.aex` or claim broad website compatibility.

## WebCrypto

The former operational `NotSupportedError` paths now compute actual results:

- SHA-1/256/384/512 `digest` uses the existing OS hash translation units.
- HMAC-SHA-256/384/512 `sign` and `verify` use the existing native HMAC;
  equal-length signature comparison visits every byte.
- PBKDF2/HKDF `deriveBits` use that HMAC. `deriveKey` can produce AES/HMAC keys.
- AES-GCM encrypts and authenticates/decrypts with 128/192/256-bit imported keys.
  Authentication failure rejects without returning plaintext.

`js_subtle.c` owns normalization, import validation and private key metadata;
`js_digest.inc` and `js_crypto_ops.inc` bind native operations. Public cached
`key.algorithm`/`key.usages` objects cannot grant operations or change the hash.
The binding also checks JWK `alg`/`use`/`key_ops`, rejects extractable KDF base
keys, and snapshots digest input before reading algorithm getters. Shared
buffer views are rejected.

No second SHA implementation was added. The existing kernel PBKDF2/HKDF helpers
silently refuse salt/info beyond 256 bytes; the browser instead allocates the
RFC concatenations dynamically and reuses native HMAC, including tests with
257-byte salt and 300-byte info.

**Remaining boundaries:** SHA-1 HMAC/KDF, RSA/EC operations, random key generation,
wrap/unwrap, partial-byte HMAC keys and truncated GCM tags still reject. GCM
supports IV lengths 1..1024 bytes and a 128-bit tag. KDF output is capped at
16 MiB; PBKDF2 is capped at 4 million HMAC invocations per call. Operations still
run synchronously inside the promise executor; this is not worker scheduling.

**Evidence:** `crypto-checks.js` contains 48 fixed independent vector inputs
(Python `hashlib`/`hmac` and `cryptography` AESGCM), expanded into **152** byte,
verification, tamper, import, authorization and error assertions. The integrated
host run in `/tmp/logitos-expand-combined.log` reports `total:152, failed:[]`
and `web-crypto-ops: 3 checks, 0 failures`. Earlier DOM subscription compile
failures were apparatus/integration failures; the later integrated build
supersedes that blocker for this target. The separate digest gate passes its
50 cases (`/tmp/web-crypto-final-regression.log`).

The negative build flips result byte zero independently in HMAC, KDF and GCM.
It printed `FAIL real crypto operations match independent vectors`, with named
HMAC/PBKDF2/HKDF/AES-GCM mismatches. The positive target requires this control.

```sh
make BUILD=build-crypto-ops test-web-crypto-ops-negctl
make BUILD=build-crypto-ops test-web-crypto-ops
make BUILD=build-crypto-ops test-web-digest
```

Guest script: `tests/fixtures/engine-expansion/crypto-checks.js`; its visible
summary is written to `#crypto-result`. Guest execution is owned by the root
integration run and is not asserted by the host evidence above.

## localStorage persistence

The previous backend was explicitly memory-only. With
`js_webapi_set_storage_store(&os_store)` installed before any realm opens,
local areas now use `/browser/storage.0` and `/browser/storage.1`. Session areas
remain tab-owned and are never serialized. An embedder passing `NULL` still
gets the original memory-only service.

Each whole-file snapshot has a format version, exact byte length, generation,
area count and CRC32 over header plus payload. Mutations write the inactive
slot and read it back byte-for-byte before replacing the memory map. A damaged
newest slot falls back to the previous valid generation. If records are read
but neither validates, restore returns `STORAGE_CORRUPT`, not an empty map.
Recognizable corruption blocks local access/mutation with `InvalidStateError`.
Write/readback failure preserves the old memory map, throws on the mutation,
and blocks subsequent local changes until reopen; session storage keeps working.

Why this commit mechanism: `logitfs_write_locked()` uses whole-file transactions
and `log_commit()` propagates failures from three cache barriers. Rename cannot
replace an existing file, so temp-plus-rename would introduce a deletion gap.
The old `bstore_ops` comment said write success was zero; shipping
`write_file → vfs_write → logitfs_write_locked` returns the byte count. This
binding accepts zero or the complete count, rejects short positive counts, and
requires successful readback for either accepted convention.

**Remaining boundaries:** one browser-process writer, no interprocess lock or
storage-event bus. Existing limits remain 16 areas, 256 items/area and 256 KiB
encoded key/value bytes/area; serialized origins are limited to 2048 bytes.
`bstore_ops.read` conflates absent files and read errors as a negative result:
two wholly unreadable slots cannot be distinguished from first launch by this
interface. A reported write/barrier/readback failure can occur after bytes
reached disk; the error is surfaced and the previous slot retained, but rollback
of an uncertain external commit is not promised. Host file tests do not prove
power-cut recovery of the actual guest filesystem.

**Evidence:** 61 backend checks cover reopen, separate origins, NUL bytes,
key order, remove/clear, session exclusion, corrupt-newest fallback, recovery,
negative/short/lying/no-op writes, allocation failure and quota. All pass.
Six additional assertions run real file write/read in separate host processes.
The JS binding gate passes 16 checks, including exception mapping and readable
old memory after a failed mutation. ASan+UBSan passes the same backend suite.
Logs: `/tmp/storage-persistence-js.log`, `/tmp/storage-persistence-asan.log`.

The negative build skips the actual disk write while returning success. Its
second process printed `FAIL: new process restores exact local bytes`. It is a
prerequisite of both the backend and JS positive gates.

```sh
make BUILD=build-storage-persistence test-storage-persistence-negctl
make BUILD=build-storage-persistence test-storage-persistence
clang -O1 -g -fsanitize=address,undefined -DSTORAGE_BACKEND_TEST \
  -Ic/apps/browser -o build-storage-persistence/storage_persistence_asan \
  tests/unit/storage_persistence_test.c c/apps/browser/storage_backend.c
build-storage-persistence/storage_persistence_asan
```

Guest fixture: `tests/fixtures/engine-expansion/storage.html` with
`storage-checks.js`. Loading performs **no write**. Click **Write marker**, record
the LOCAL token, close/reopen the browser, and verify that LOCAL is unchanged
while SESSION is absent. **Clear marker** must also survive another restart.
These guest actions remain a separate integration acceptance step.

## Correction from the first actual guest reopen

The host pass above did **not** establish guest persistence. The first guest
wrote `guest-1788944252230` and displayed `WRITE COMMITTED`, but a new browser
process at the same URL displayed `LOCAL: absent`. Evidence is in
`/tmp/logitos-browser-expansion-evidence/storage-written.png` and
`storage-reopened.png`; this failure is retained beside the earlier host claim.

Root cause: `storage_read_slot` read only a 32-byte header before allocating the
record body. Actual `logitfs.c:inode_read` is all-or-nothing: if the provided
buffer is smaller than the entire file, it returns -1. Every nonempty snapshot
therefore looked missing. The original host fake and `fread` adapter returned
prefixes and concealed the production contract.

Correction: read the entire snapshot in one call using the format's bounded
maximum capacity, then validate its exact length and CRC. Host memory/file
adapters now reject undersized read buffers. Before the fix those realistic
adapters produced **19 failures out of 61**; after the fix all 61 backend checks,
16 JS checks and separate-process file recovery pass with
`make BUILD=build-expand test-storage-persistence` (exit zero). Logs:
`/tmp/storage-wholefile-before.log`, `/tmp/storage-wholefile-after.log`.

A second permanent negative control reinstates the 32-byte prefix read and
prints `FAIL: new process restores exact local bytes`. Both positive storage
gates require it:

```sh
make BUILD=build-expand test-storage-persistence-prefix-negctl
make BUILD=build-expand test-storage-persistence
```

Actual guest write/reopen must be repeated with this corrected binary before
claiming that the guest failure is resolved. No guest pass is inferred here.

## Link and rebuild audit

`BROWSER_CRYPTO_OPS_SRC` is the single source authority: `hash/hmac_hkdf.c`,
`aead/aesgcm.c`, `aead/aes_dispatch.c`, `aead/aes_ni.c`, and
`c/kernel/cpu/cpufeat.c`. Hash sources come from `BROWSER_DIGEST_SRC`.
`storage_backend.c` is included by `js_webapi.c`; its new persistence helper is
an `.inc`, not a second translation unit.

Read-only audit found a **real WPT compile blocker**: `WPT_FROM_BROWSER` inherits
AES-NI from `BROWSER_PIPE`, but the observed `WPT_CF` lacked `-Ic/kernel/cpu`.
`aes_ni.c` includes `cpufeat.h` before its non-x86 conditional, so ARM hosts also
need the path. Root was notified to add it to `WPT_CF`, which covers `wpt_test`,
`wpt_fire2` and `wpt_negctl`. The minimal failure was reproduced with:

```sh
clang -fsyntax-only -Ic/crypto c/crypto/aead/aes_ni.c
# fatal error: 'cpufeat.h' file not found
```

Correction later in this audit: root added `-Ic/kernel/cpu` to `WPT_CF`;
the same syntax command with that include directory now exits zero. This
settles header resolution, not a complete WPT link or WPT execution.

Production `UCFLAGS` already uses `INCDIRS`, which includes the CPU directory.
The crypto gate supplies it explicitly. HTTP2 filters only `c/net/http/%` from
`BROWSER_PIPE`; WebSocket has its own explicit source list, so neither inherits
this new AES dependency. Broad one-command host links such as WPT also lack
explicit prerequisites for these new `.inc` helpers; header-only edits may
leave those binaries stale unless the dependencies or their rebuild are forced.
A fresh link and `make test-mk-wired` remain root integration checks.

## Primary references

- [W3C Web Cryptography specification](https://www.w3.org/TR/webcrypto/): operation normalization, key usages, HMAC, AES-GCM and KDF behavior.
- [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104): HMAC construction.
- [RFC 5869](https://www.rfc-editor.org/rfc/rfc5869): HKDF extract/expand and the 255-block output bound.
- [RFC 8018, section 5.2](https://www.rfc-editor.org/rfc/rfc8018#section-5.2): PBKDF2 block construction.
- [NIST SP 800-38D](https://csrc.nist.gov/pubs/sp/800/38/d/final): GCM authenticated encryption.
- Repository primary implementation evidence: `c/apps/browser/tabs.h`,
  `c/apps/logit.h`, `c/kernel/exec/syscall.c`, `c/fs/vfs.c`,
  `c/fs/logitfs.c` and `c/kernel/core/settings.h` for the storage contract.

最终更正：主目录全新镜像已通过guest浏览器进程关闭/重新启动后读取同一个LOCAL标记，SESSION未恢复。证据与未覆盖的断电边界见[统一报告](expansion-status-2026-09-09.md)。
