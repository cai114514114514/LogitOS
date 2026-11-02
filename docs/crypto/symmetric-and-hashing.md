# Hashes, AEADs and KDFs

LogitOS implements hashes, message authentication, symmetric encryption and key derivation in `c/crypto/hash`, `c/crypto/aead` and `c/crypto/kdf`. The Keccak implementation in `c/crypto/pq` supplies SHA-3 and SHAKE. Consumers include TLS, SSH, account authentication, package verification, file checksums, browser cryptography, GPU firmware loading and Wi-Fi. This page describes the primitives, their C interfaces and their current uses.

## API organisation

`c/crypto/crypto.h` declares SHA-2, HMAC, HKDF, the TLS 1.2 PRF, AES-GCM, AES-CTR, AES-CBC, ChaCha20-Poly1305, PBKDF2 and password-record helpers. BLAKE hashes, cSHAKE, KMAC, CRC-32C, XChaCha20-Poly1305, AES-GCM-SIV, scrypt and Argon2 have headers beside their implementations.

Callers supply hash contexts and output buffers. scrypt and Argon2 also take working memory and its capacity, giving callers control over allocation. SHA-2 and BLAKE streaming interfaces count bytes with `size_t`; the shared HMAC, AEAD and PBKDF2 interfaces use `int` for most buffer lengths.

`crypto_wipe()` in `c/crypto/crypto.h` clears memory through a volatile byte pointer. HMAC, KDF and cipher implementations use it to erase sensitive intermediates.

## SHA-2 and SHA-1

SHA-2 uses two compression cores. `c/crypto/hash/sha256.c` implements SHA-224 and SHA-256 with 64-byte blocks. `c/crypto/hash/sha384.c` implements SHA-384, SHA-512, SHA-512/224 and SHA-512/256 with 128-byte blocks.

| Primitive | Digest bytes | API |
|---|---:|---|
| SHA-224 | 28 | `sha224_init()`, `sha256_update()`, `sha224_final()`, `sha224()`; `struct sha256` |
| SHA-256 | 32 | `sha256_init()`, `sha256_update()`, `sha256_final()`, `sha256()`; `struct sha256` |
| SHA-384 | 48 | `sha384_init()`, `sha512_update()`, `sha384_final()`, `sha384()`; `struct sha512` |
| SHA-512 | 64 | `sha512_init()`, `sha512_update()`, `sha512_final()`, `sha512()`; `struct sha512` |
| SHA-512/224 | 28 | `sha512_224_init()`, `sha512_update()`, `sha512_224_final()`, `sha512_224()`; `struct sha512` |
| SHA-512/256 | 32 | `sha512_256_init()`, `sha512_update()`, `sha512_256_final()`, `sha512_256()`; `struct sha512` |
| SHA-1 | 20 | `ocsp_sha1()` hashes a complete buffer |

Each SHA-2 initialiser selects the variant’s starting state. Variants sharing a compression core also share update and padding code. SHA-512/224 and SHA-512/256 use their own initial states, so their results differ from truncating a SHA-512 digest.

TLS selects SHA-256 or SHA-384 for transcripts and key derivation according to the cipher suite. Certificate and handshake signature processing also uses SHA-512. These consumers appear in `c/net/tls/tls.c`, `c/net/tls/tls12.c`, `c/net/tls/tls_server.c`, `c/net/tls/x509.c` and `c/net/tls/ocsp.c`. Ed25519 uses SHA-512 in `c/crypto/pubkey/ed25519.c`.

Other SHA-256 consumers include:

| Consumer | Purpose |
|---|---|
| `c/kernel/core/rng.c` | Hashes generator state, a domain byte and additional input |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/aexsig.c` | Hashes package payloads and signature manifests |
| `c/kernel/exec/load/elf.c` | Computes image digests through `elf_read_sha256()` |
| `c/net/ssh/ssh_kex.c`, `c/net/ssh/ssh_hostkey.c` | Hashes exchange and key-derivation inputs; computes host-key fingerprints |
| `c/apps/coreutils/net_digest.inc` | Supplies SHA-256 and SHA-512 file and HTTP checksums |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Checks firmware bytes against SHA-256 pins |

The Polaris loader sends root-disk files and boot-module copies through the same format and digest checks. `Makefile` generates its pin table with `tools/gpu/amd/fwpin.py` from `third_party/amd-firmware/MANIFEST`. This keeps accepted digests and the recorded firmware revision in one input.

SHA-224 is also available through HMAC, HKDF and PBKDF2. SHA-224 and both SHA-512/t variants have vector and differential cases in `tests/unit/crypto_vec_test.c` and `tests/unit/crypto_diff_test.c`.

The SHA-1 implementation retains the name `ocsp_sha1()` in `c/crypto/hash/sha1.c`. Its header comment describes the original OCSP use and the later WebSocket caller. Current consumers also include Wi-Fi and browser bindings:

- `c/net/tls/ocsp.c` hashes issuer names and keys for certificate identifiers.
- `c/net/http/ws.c` computes the WebSocket handshake response.
- `c/drivers/net/wifi/security/derivation.c` constructs HMAC-SHA1 and WPA2 derivation.
- `c/apps/gui/browser/js/crypto/digest.inc` exposes SHA-1, SHA-256, SHA-384 and SHA-512 digests.

## BLAKE hashes

| Primitive | API | Parameters and uses |
|---|---|---|
| BLAKE2s | `blake2s_init()`, `blake2s_init_key()`, `blake2s_update()`, `blake2s_final()`, `blake2s()`; `struct blake2s` | Digest length 1–32 bytes; key length 0–32 bytes. Used by `tests/unit/blake2s_test.c` and comparison harnesses. |
| BLAKE2b | `blake2b_init()`, `blake2b_update()`, `blake2b_final()`, `blake2b512()`, `blake2b_keyed()`; `struct blake2b` | Digest length 1–64 bytes; key length 0–64 bytes. The `net` command uses the unkeyed 64-byte digest. |
| BLAKE3 | `blake3_init()`, `blake3_init_keyed()`, `blake3_init_derive_key()`, `blake3_update()`, `blake3_finalize()`, `blake3_finalize_seek()`; `struct blake3_hasher` | Hashing, keyed hashing with a 32-byte key, context-based derivation and extendable output. The `net` command uses 32-byte unkeyed output. |

BLAKE2 incorporates digest and key lengths into its initial state. Keyed initialisation absorbs a padded key block. Both implementations retain the final input block until finalisation, when compression receives the final-block flag. The implementations are in `c/crypto/hash/blake2s.c` and `c/crypto/hash/blake2b.c`.

`blake2b_init()` returns `0` on success or `-1` for an invalid digest or key length. `blake2b_keyed()` wraps initialisation, update and finalisation with the same return convention. `blake2b512()` fixes the output at 64 bytes.

BLAKE3 processes 1,024-byte chunks and merges chaining values through a stack. Root output uses an output-block counter. `blake3_finalize()` writes 32 bytes; `blake3_finalize_seek()` accepts a byte offset and output length. Both take a const context, supporting repeated output requests. See `c/crypto/hash/blake3.c`.

`blake3_init_derive_key()` first hashes the supplied application context under the context-derivation flag, then starts the material hash under another flag. The contract in `c/crypto/hash/blake3.h` asks applications to use one fixed context, specific to that application. `tests/unit/blake3_test.c` covers hashing, keyed hashing, derivation and extended output.

The `net` checksum adapter is `c/apps/coreutils/net_digest.inc`. Argon2 uses a private BLAKE2b implementation and a hash construction of variable length in `c/crypto/kdf/argon2.c`.

## SHA-3, SHAKE, cSHAKE and KMAC

`c/crypto/pq/keccak.c` implements Keccak-f[1600] and the sponge operations shared by SHA-3, SHAKE and cSHAKE.

| Primitive | API | Uses |
|---|---|---|
| Keccak-f[1600] | `keccakf1600()` operates on 25 64-bit words | SHA-3, SHAKE and cSHAKE |
| SHA3-256 | `sha3_256()` writes 32 bytes | Public-key hashing in `c/crypto/pq/mlkem.c` |
| SHA3-512 | `sha3_512()` writes 64 bytes | Seed expansion and derivation in `c/crypto/pq/mlkem.c` |
| SHAKE128 | `shake128_init()`, `shake_absorb()`, `shake_finalize()`, `shake_squeeze()`, `shake128()`; `struct shake` | ML-KEM and ML-DSA sampling; cSHAKE sponge operations |
| SHAKE256 | `shake256_init()`, shared absorb/finalise/squeeze functions, `shake256()`; `struct shake` | ML-KEM and ML-DSA expansion, hashing and sampling; cSHAKE sponge operations |
| cSHAKE128/256 | `cshake128_ctx_init()`, `cshake256_ctx_init()`, `cshake_absorb()`, `cshake_finalize()`, `cshake_squeeze()`, `cshake128()`, `cshake256()`; `struct cshake_ctx` | KMAC and `tests/unit/cshake_test.c` |
| KMAC128/256 | `kmac128()`, `kmac256()` | Authentication vectors and comparison harnesses |
| KMACXOF128/256 | `kmacxof128()`, `kmacxof256()` | Extendable authentication output checked by `tests/unit/run-cshake-openssl.sh` |

SHAKE divides processing into absorb, finalise and squeeze phases. This supports rejection sampling: callers can request further output as sampling proceeds. SHAKE128 uses a 168-byte rate and SHAKE256 a 136-byte rate. Their public-key consumers are `c/crypto/pq/mlkem.c` and `c/crypto/pq/mldsa.c`.

cSHAKE adds function-name and customisation strings. When both are empty, `c/crypto/hash/cshake.c` selects SHAKE’s domain suffix and absorbs the message directly. A nonempty string selects the cSHAKE suffix and an encoded prefix.

`c/crypto/hash/cshake.h` exports `left_encode()`, `right_encode()` and the padding helpers `cshake_bp_begin()`, `cshake_bp_feed_string()` and `cshake_bp_end()`, with `struct cshake_bp`. The string interfaces take byte lengths and convert them to bit lengths for encoding. Streaming the padding avoids allocating a concatenated prefix.

KMAC in `c/crypto/hash/kmac.c` selects the function name `"KMAC"`, absorbs the padded key and message, and appends the encoded output length in bits. KMACXOF appends an encoded zero. The KMAC trailer binds the result to the requested length; KMACXOF supplies consistent output prefixes across requests of different lengths.

## HMAC and protocol key derivation

`c/crypto/hash/hmac_hkdf.c` implements HMAC, HKDF and TLS derivation helpers using SHA-2.

| Construction | API and contract | Uses |
|---|---|---|
| HMAC | `hmac()` selects SHA-224, SHA-256, SHA-384 or SHA-512 with digest lengths 28, 32, 48 or 64 | TLS Finished messages and PSK binders, SSH packet authentication, PBKDF2 and browser operations |
| HKDF extract | `hkdf_extract()` writes a pseudorandom key of the selected digest length; absent or empty salt selects a zero salt of that length | TLS 1.3 client, server and PSK derivation |
| HKDF expand | `hkdf_expand()` expands a pseudorandom key with an information string and output length | TLS label expansion and tests |
| TLS 1.3 label expansion | `hkdf_expand_label()` encodes the `"tls13 "` prefix, label, context and output length; returns `0` or `-1` | Traffic keys, IVs, Finished keys, binders and resumption secrets |
| TLS 1.2 PRF | `tls12_prf()` expands a label and seed through an HMAC chain using SHA-256 or SHA-384 | Master secrets, key blocks and Finished messages |

HMAC uses a 64-byte block for SHA-224 and SHA-256, and a 128-byte block for SHA-384 and SHA-512. Keys longer than the block are hashed before constructing the inner and outer pads.

TLS consumers reside in `c/net/tls/tls.c`, `c/net/tls/tls12.c`, `c/net/tls/tls_server.c` and `c/net/tls/tls_psk.c`. The TLS server passes `hmac()` to ECDSA signing for deterministic nonce derivation. SSH uses HMAC-SHA256 in `c/net/ssh/ssh_packet.c`.

The browser binding in `c/apps/gui/browser/js/crypto/crypto_ops.inc` constructs PBKDF2 and HKDF through `hmac()`. It allocates concatenation buffers according to the input sizes, accommodating browser inputs beyond the shared KDF helpers’ fixed buffers.

`frame_install_window()` in `c/apps/gui/browser/js/dom/frame.c` installs a live getter for the creator’s `crypto` property when that property holds an object and the frame lacks its own property. Later reads through that getter observe the creator’s current value.

## Authenticated encryption

The AEAD interfaces in `c/crypto/aead` accept associated data, message bytes and a 16-byte tag. Seal functions write ciphertext equal in length to the plaintext and write the tag to another buffer. Open functions return `0` after authentication succeeds and `-1` on failure.

| Construction | API | Key and nonce sizes | Uses |
|---|---|---|---|
| AES-128-GCM | `aes128_gcm_seal()`, `aes128_gcm_open()` | 16-byte key; 12-byte nonce | TLS records and diagnostics in `c/crypto/cpu_report.c` |
| AES-192-GCM | `aes192_gcm_seal()`, `aes192_gcm_open()` | 24-byte key; 12-byte nonce | Primitive vectors and backend comparisons |
| AES-256-GCM | `aes256_gcm_seal()`, `aes256_gcm_open()` | 32-byte key; 12-byte nonce | TLS records |
| AES-GCM with an IV length | `aes128_gcm_seal_iv()`, `aes128_gcm_open_iv()`, `aes192_gcm_seal_iv()`, `aes192_gcm_open_iv()`, `aes256_gcm_seal_iv()`, `aes256_gcm_open_iv()` | Corresponding AES key size; IV length 1–1,024 bytes | Browser operations in `c/apps/gui/browser/js/crypto/crypto_ops.inc` |
| ChaCha20-Poly1305 | `chacha20_poly1305_seal()`, `chacha20_poly1305_open()` | 32-byte key; 12-byte nonce | TLS records |
| XChaCha20-Poly1305 | `xchacha20_poly1305_seal()`, `xchacha20_poly1305_open()` | 32-byte key; 24-byte nonce | `tests/unit/xchacha20poly1305_test.c` and comparison harnesses |
| AES-128-GCM-SIV | `aes128_gcm_siv_seal()`, `aes128_gcm_siv_open()` | 16-byte key; 12-byte nonce | `tests/unit/aes_gcm_siv_test.c` and comparison harnesses |
| AES-256-GCM-SIV | `aes256_gcm_siv_seal()`, `aes256_gcm_siv_open()` | 32-byte key; 12-byte nonce | `tests/unit/aes_gcm_siv_test.c` and comparison harnesses |

`c/crypto/aead/aesgcm.c` implements GCM counter mode, GHASH framing and initial-counter construction. A 12-byte IV uses the direct construction; other accepted lengths pass through GHASH. Open authenticates ciphertext before decrypting.

`c/crypto/aead/chacha20poly1305.c` derives the Poly1305 key from ChaCha block zero and encrypts from block one. Poly1305 authenticates associated data, ciphertext, padding and lengths. Open compares all tag bytes before writing plaintext.

The ChaCha core exports `chacha_block()` and `chacha20()` through `c/crypto/aead/chacha_core.h`. The block function produces 64 bytes from a 32-byte key, 32-bit counter and 12-byte nonce. The stream function XORs successive blocks with input.

`hchacha20()` in `c/crypto/aead/xchacha20poly1305.c` reuses the block function and subtracts its final state addition to obtain a 32-byte subkey. XChaCha derives this subkey from the first 16 nonce bytes. Four zero bytes followed by the last eight nonce bytes form the inner ChaCha20-Poly1305 nonce.

AES-GCM-SIV derives message keys, authenticates plaintext with POLYVAL and encrypts from a counter derived from the tag. Its exported `polyval()` interface takes complete 16-byte blocks. `c/crypto/aead/aes_gcm_siv.c` implements POLYVAL field arithmetic and message framing.

TLS record code constructs the nonce and associated data, then calls `tls_aead_encrypt()` or `tls_aead_decrypt()` in `c/net/tls/tls.c`. TLS 1.2 and the TLS server share this dispatch.

## AES backends, CTR and CBC

`struct aes_backend` in `c/crypto/aead/aes_backend.h` groups key expansion, block encryption, block decryption and GHASH field multiplication. GCM, CTR, CBC and GCM-SIV implement their constructions in the mode files and obtain block operations from the backend. This keeps framing, counters and padding shared across AES implementations.

`aes_current_backend()` supplies the selected backend. Selection occurs through `crypto_simd_init()` or on first use. `aes_backend_ni()` supplies the implementation in `c/crypto/aead/aes_ni.c` when CPU checks report both AES-NI and PCLMULQDQ; `aes_backend_c()` supplies the portable implementation in `c/crypto/aead/aesgcm.c`.

`crypto_simd_backend_name()` reports the selection, and `crypto_simd_constant_time()` returns the backend’s timing classification. Tests can select portable C with `crypto_simd_force_baseline()`. `crypto_simd_selftest()` compares key schedules, encryption, decryption and field multiplication with the portable reference. `c/crypto/cpu_report.c` reports the backend during boot.

| Mode | API | Contract and uses |
|---|---|---|
| AES-CTR | `aes128_ctr()`, `aes192_ctr()`, `aes256_ctr()` | Keys of 16, 24 or 32 bytes; 16-byte initial counter; equal input and output lengths. The same operation encrypts and decrypts, including in place. SSH uses AES-128-CTR in `c/net/ssh/ssh_packet.c`. |
| AES-CBC encryption | `aes128_cbc_encrypt()`, `aes192_cbc_encrypt()`, `aes256_cbc_encrypt()` | 16-byte IV and PKCS#7 padding. Returns `((len / 16) + 1) * 16` ciphertext bytes, or `-1` for a negative length. Used by primitive tests. |
| AES-CBC decryption | `aes128_cbc_decrypt()`, `aes192_cbc_decrypt()`, `aes256_cbc_decrypt()` | Takes a positive multiple of 16 ciphertext bytes. Returns the unpadded length or `-1`; invalid padding triggers a wipe of the decrypted output. Used by primitive tests. |

CTR increments the full counter as a big-endian 128-bit value. GCM increments the low 32 bits. CBC adds padding to every message, including a full block for input divisible by 16, and decryption examines all 16 candidate padding bytes.

`c/crypto/aead/aes_modes.c` implements these modes. Vectors and differential cases are in `tests/unit/crypto_vec_test.c` and `tests/unit/crypto_diff_test.c`.

## Wi-Fi constructions

Wi-Fi declares its cryptographic interfaces in `c/drivers/net/wifi/security/crypto.h`. Derivation reuses `ocsp_sha1()`, while CCM and key unwrapping use `aes_current_backend()`.

| Construction | API | Contract and uses |
|---|---|---|
| HMAC-SHA1 | `wifi_hmac_sha1()` | Produces 20 bytes. Used for derivation and handshake authentication. |
| WPA2 PSK derivation | `wifi_psk()` | PBKDF2-HMAC-SHA1 with 4,096 iterations; derives a 32-byte master key from a passphrase and SSID. Called by `c/drivers/net/wifi/management/association.c`. |
| Pairwise-key derivation | `wifi_ptk()` | Expands a 32-byte master key with sorted peer addresses and nonces into 48 bytes. Used by `c/drivers/net/wifi/security/handshake.c`. |
| AES-CCM for CCMP-128 | `wifi_ccm_seal()`, `wifi_ccm_open()` | 16-byte key, 13-byte nonce and appended eight-byte tag. Returns the output length or `-1`. Used by `c/drivers/net/wifi/data/frame.c`. |
| AES key unwrapping | `wifi_key_unwrap()` | Takes a 16-byte key and wrapped input; returns the recovered length or `-1`. Used by `c/drivers/net/wifi/security/handshake.c` and `c/drivers/net/wifi/security/group.c`. |

`c/drivers/net/wifi/security/derivation.c` implements HMAC-SHA1, PSK derivation and pairwise-key expansion. These functions return `0` on success or `-1` for rejected inputs.

`c/drivers/net/wifi/security/ccmp.c` implements CCM with a two-byte message-length field and eight-byte tag. Open decrypts into scratch, authenticates the recovered message and copies it to the output after verification.

`c/drivers/net/wifi/security/keywrap.c` implements AES key unwrapping. It checks the recovered eight-byte integrity register before copying key bytes to the output.

## PBKDF2 and password records

`pbkdf2()` in `c/crypto/kdf/pbkdf2.c` selects HMAC-SHA224, HMAC-SHA256, HMAC-SHA384 or HMAC-SHA512 through digest lengths 28, 32, 48 or 64. It takes a password, salt, positive iteration count and requested output length. Each output block XORs the values in its HMAC chain.

`pwhash_make()` obtains a 16-byte salt through a supplied random-byte callback, derives 32 bytes with PBKDF2-HMAC-SHA256 and formats:

```text
$pbkdf2-sha256$<iters>$<salt-b64>$<dk-b64>
```

It returns the formatted length or `-1`. Zero iterations selects 600,000. Account enrollment in `c/apps/coreutils/login.c` passes `ACCT_ITERS`, defined as 120,000 in `c/apps/coreutils/accounts.h`. The account header explains this choice in terms of responsiveness under QEMU TCG.

`pwhash_check()` parses the record’s parameters, derives the key and compares all derived bytes through an accumulated difference. It returns `1` for a match and `0` otherwise. Storing the iteration count preserves verification when enrollment policy changes.

Console login, the greeter and SSH authentication call the account password helper in `c/apps/coreutils/accounts.h`. Their callers are `c/apps/coreutils/login.c`, `c/apps/gui/greeter/greeter.c` and `c/apps/services/ssh/sshd.c`.

The introductory comment in `c/crypto/kdf/pbkdf2.c` records the original choice of PBKDF2 in terms of memory cost, login availability and reuse of HMAC. Current account records continue to use PBKDF2 alongside the scrypt and Argon2 library interfaces.

## scrypt and Argon2

Both KDFs take working memory from the caller. Their consumers are primitive tests and comparison harnesses.

| Construction | API | Parameters and return values |
|---|---|---|
| scrypt | `scrypt()`, `scrypt_scratch_len()` | Password, salt, work factor N, block-size parameter r, parallelisation parameter p, output and scratch capacity. Returns `0` on success or `-1` for checked parameter or capacity errors. |
| Argon2d, Argon2i and Argon2id | `argon2()`, `argon2_memory_blocks()` | Type, password, salt, optional secret and associated data, passes, memory cost, lanes, output and block capacity. Returns `0` on success, `-1` for checked parameter errors or `-2` for insufficient block capacity. |

scrypt exports `salsa20_8_core()`, `scrypt_blockmix()` and `scrypt_romix()` for component operations. The Salsa20/8 core takes and produces 64 bytes. BlockMix takes distinct input and output buffers of `128 * r` bytes; ROMix transforms a buffer of that size in place.

`scrypt_romix_scratch_len()` computes ROMix working space. `scrypt_scratch_len()` adds the initial block array, giving `128 * r * (p + N + 2)` bytes. `c/crypto/kdf/scrypt.c` processes the p ROMix inputs sequentially and reuses one working region. Its internal PBKDF2 implementation streams the salt into SHA-256, accommodating the intermediate block array. Successful `scrypt()` completion wipes the supplied scratch region.

Argon2 uses `struct argon2_block`, containing 1,024 bytes. `argon2_memory_blocks()` applies a minimum of eight blocks per lane, then rounds down to a multiple of four times the lane count. The caller allocates that many blocks and passes the capacity to `argon2()`.

`enum argon2_type` selects `ARGON2_D`, `ARGON2_I` or `ARGON2_ID`. Argon2d derives reference addresses from earlier block contents. Argon2i generates addresses from public parameters. Argon2id uses Argon2i addressing for the first two slices of pass zero and Argon2d addressing for the remaining work.

`c/crypto/kdf/argon2.c` implements version `0x13`, visiting passes, slices and lanes sequentially. Successful completion wipes the working blocks used by the call.

`tests/unit/scrypt_test.c` and `tests/unit/argon2_test.c` contain component and complete-derivation vectors. The Argon2 test also checks intermediate block state when compiled with its test hooks. `tests/crypto2026.mk` enables those hooks and defines OpenSSL comparison targets.

## CRC-32C

`c/crypto/hash/crc32c.h` declares `crc32c_update()`, `crc32c_final()` and `crc32c()`. Incremental computation starts with `CRC32C_INIT`, updates over input bytes and applies the final inversion.

`c/crypto/hash/crc32c.c` uses a table for the reflected Castagnoli polynomial. It returns the checksum register value; serialisation byte order is up to the consumer. Its current use is in `tests/unit/crc32c_test.c`. The IEEE CRC-32 implementation lives in `c/drivers/block/crc32.c`.

## Verification sources

The repository provides vectors, differential comparisons and controls that inject algorithm defects. The table below lists each target and its coverage.

| Make target | Coverage |
|---|---|
| `test-crypto`, `test-crypto-diff` | Shared hash, HMAC, KDF and cipher vectors and differential cases |
| `test-aes-ni` | AES backend selection and primitive comparisons |
| `test-pwhash` | PBKDF2 vectors and password records |
| `test-crc32c` | Checksum vectors and a polynomial defect control |
| `test-blake2b`, `test-blake2s`, `test-blake3` | BLAKE vectors and algorithm controls |
| `test-cshake`, `test-cshake-openssl` | cSHAKE and KMAC examples, KMACXOF comparisons and the SHAKE identity for empty customisation |
| `test-scrypt`, `test-argon2` | KDF vectors, component or intermediate-state checks and defect controls |
| `test-xchacha` | HChaCha20 and XChaCha20-Poly1305 vectors and controls |
| `test-aes-gcm-siv` | POLYVAL and AES-GCM-SIV vectors and a tag-construction control |

`Makefile` defines the shared crypto and AES backend targets. `tests/crypto.mk` defines the password-record target. `tests/crypto2026.mk` defines the additional primitive targets and makes controls prerequisites of their positive targets. `tests/tlsx.mk` adds the SHA-224 defect control to `test-crypto-diff`.

The controls exercise choices such as scrypt BlockMix output ordering, Argon2 addressing and BLAKE3 root-flag placement. Their comparisons with expected outputs supplement encryption/decryption round trips.

## Limitations

- `TRANSPARENCY.md` classifies the cryptographic implementations as experimental; production certification and an external cryptographic audit are both still pending.
- Portable AES uses tables indexed by secret state, and portable GHASH branches on bits derived from secrets. scrypt and Argon2d, including Argon2id’s later phases, access memory through state that depends on the password. Timing properties depend on the primitive and backend.
- CTR and CBC provide encryption alone, and CRC-32C detects errors. Polaris firmware pins check correspondence with manifest digests and stop there.
- Several shared helpers return `void` when rejecting parameters. HKDF expansion accepts information strings up to 256 bytes and output up to 255 digest lengths. TLS label expansion accepts SHA-256 or SHA-384, labels and contexts up to 64 bytes, and the same output bound. The TLS 1.2 PRF accepts labels up to 32 bytes and seeds up to 128 bytes. PBKDF2 accepts salts up to 256 bytes. BLAKE2s relies on callers to satisfy its digest and key length ranges.
- Wi-Fi HMAC-SHA1 accepts keys up to 64 bytes and messages up to 2,304 bytes. PSK derivation accepts passphrases of 8–63 bytes and SSIDs of 1–32 bytes. CCM accepts plaintext up to 2,304 bytes and associated data up to 30 bytes. Key unwrapping accepts wrapped input of 24–512 bytes in multiples of eight.
- The AES-GCM-SIV header specifies untouched output on authentication failure, and the implementation decrypts into the output and wipes it on a tag mismatch. Its source also records missing RFC-vector coverage for the incremental authentication path selected when padded input and the length block exceed its 128-byte local buffer.
- The scrypt header specifies scratch wiping on either return path, and early parameter and capacity rejections precede the wipe. The Argon2 header’s introductory comment says insufficient memory returns `-1`; its detailed contract and implementation return `-2`.
- BLAKE2s’s keyed corpus is fetched externally, and its unit test skips that corpus when absent. OpenSSL comparisons are defined in `tests/crypto2026.mk`. The largest scrypt vector requires about 1 GiB of scratch and is conditional in `tests/unit/scrypt_test.c`.

## Source map

| File | What it contains |
|---|---|
| `c/crypto/crypto.h` | Shared declarations, SHA-2 contexts and volatile wiping helper |
| `c/crypto/hash/sha256.c`, `c/crypto/hash/sha384.c` | SHA-224/256 and SHA-384/512/512-224/512-256 |
| `c/crypto/hash/sha1.c` | SHA-1 buffer interface and historical usage rationale |
| `c/crypto/hash/blake2s.h`, `c/crypto/hash/blake2s.c` | BLAKE2s streaming and keyed hashing |
| `c/crypto/hash/blake2b.h`, `c/crypto/hash/blake2b.c` | BLAKE2b streaming and keyed hashing |
| `c/crypto/hash/blake3.h`, `c/crypto/hash/blake3.c` | BLAKE3 hashing, keyed mode, derivation and extended output |
| `c/crypto/pq/keccak.h`, `c/crypto/pq/keccak.c` | Keccak permutation, SHA-3 and SHAKE |
| `c/crypto/hash/cshake.h`, `c/crypto/hash/cshake.c` | cSHAKE, integer encoding and padding helpers |
| `c/crypto/hash/kmac.h`, `c/crypto/hash/kmac.c` | KMAC and KMACXOF |
| `c/crypto/hash/hmac_hkdf.c` | HMAC, HKDF and TLS derivation helpers |
| `c/crypto/hash/crc32c.h`, `c/crypto/hash/crc32c.c` | Castagnoli checksum API and implementation |
| `c/drivers/block/crc32.c` | IEEE CRC-32 implementation |
| `c/crypto/aead/aes_backend.h`, `c/crypto/aead/aes_dispatch.c` | AES backend contract, selection and comparison |
| `c/crypto/aead/aesgcm.c`, `c/crypto/aead/aes_ni.c` | GCM and portable or accelerated AES primitives |
| `c/crypto/aead/aes_modes.c` | AES-CTR and AES-CBC with PKCS#7 padding |
| `c/crypto/aead/chacha_core.h`, `c/crypto/aead/chacha20poly1305.c` | ChaCha20 core and Poly1305 AEAD |
| `c/crypto/aead/xchacha20poly1305.h`, `c/crypto/aead/xchacha20poly1305.c` | HChaCha20 and XChaCha20-Poly1305 |
| `c/crypto/aead/aes_gcm_siv.h`, `c/crypto/aead/aes_gcm_siv.c` | POLYVAL and AES-GCM-SIV |
| `c/crypto/kdf/pbkdf2.c` | PBKDF2 and password records |
| `c/crypto/kdf/scrypt.h`, `c/crypto/kdf/scrypt.c` | scrypt components, scratch sizing and derivation |
| `c/crypto/kdf/argon2.h`, `c/crypto/kdf/argon2.c` | Argon2 variants, memory sizing and private hashing |
| `c/crypto/cpu_report.c` | Backend reporting and boot diagnostics |
| `c/crypto/pq/mlkem.c`, `c/crypto/pq/mldsa.c`, `c/crypto/pubkey/ed25519.c` | Hash and XOF consumers in public-key algorithms |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/aexsig.c`, `c/kernel/exec/load/elf.c` | Package, manifest and image hashing |
| `c/kernel/core/rng.c` | SHA-256 use in the random generator |
| `c/net/tls/tls.c`, `c/net/tls/tls12.c`, `c/net/tls/tls_server.c`, `c/net/tls/tls_psk.c` | TLS derivation and record-encryption consumers |
| `c/net/tls/x509.c`, `c/net/tls/ocsp.c`, `c/net/http/ws.c` | Certificate, OCSP and WebSocket hashing |
| `c/net/ssh/ssh_kex.c`, `c/net/ssh/ssh_hostkey.c`, `c/net/ssh/ssh_packet.c` | SSH hashing, AES-CTR and HMAC |
| `c/apps/coreutils/accounts.h`, `c/apps/coreutils/login.c`, `c/apps/gui/greeter/greeter.c`, `c/apps/services/ssh/sshd.c` | Account enrollment and password verification |
| `c/apps/coreutils/net_digest.inc` | File and HTTP checksum adapter |
| `c/apps/gui/browser/js/crypto/digest.inc`, `c/apps/gui/browser/js/crypto/crypto_ops.inc` | Browser digest, HMAC, derivation and GCM bindings |
| `c/apps/gui/browser/js/dom/frame.c` | Frame access to the creator’s cryptography object |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading and SHA-256 pin checks |
| `tools/gpu/amd/fwpin.py`, `third_party/amd-firmware/MANIFEST` | Firmware pin generator and digest manifest |
| `c/drivers/net/wifi/security/crypto.h`, `c/drivers/net/wifi/security/internal.h` | Wi-Fi interfaces and parameter bounds |
| `c/drivers/net/wifi/security/derivation.c`, `c/drivers/net/wifi/security/ccmp.c`, `c/drivers/net/wifi/security/keywrap.c` | Wi-Fi derivation, CCM and key unwrapping |
| `c/drivers/net/wifi/management/association.c`, `c/drivers/net/wifi/security/handshake.c`, `c/drivers/net/wifi/security/group.c`, `c/drivers/net/wifi/data/frame.c` | Wi-Fi cryptographic consumers |
| `Makefile`, `tests/crypto.mk`, `tests/crypto2026.mk`, `tests/tlsx.mk` | Crypto checks and control dependencies |
| `TRANSPARENCY.md` | Cryptographic claim and verification rules |

## See also

- [Cryptography overview](overview.md)
- [Public-key and post-quantum cryptography](public-key-and-post-quantum.md)
- [Trust roots and package signing](trust-and-signing.md)
- [TLS and certificates](../networking/tls.md)
- [SSH](../networking/ssh.md)
- [Wi-Fi support](../drivers/wifi.md)
- [GPU drivers](../drivers/gpu.md)
- [Time, randomness and settings](../kernel/time-rng-settings.md)
- [Testing](../architecture/testing.md)
