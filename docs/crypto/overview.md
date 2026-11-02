# Cryptography overview

LogitOS implements cryptographic primitives in `c/crypto` and uses them in TLS, SSH, package verification, account authentication and applications. Consumers call C interfaces for hashing, message authentication, key derivation, encryption, key agreement and signatures. AES selects a portable or CPU instruction backend. Verification code combines published test vectors, comparisons with reference implementations and guest checks of register preservation across interrupts.

## Implemented primitives

`c/crypto/crypto.h` declares the shared interfaces. Additional families provide headers beside their implementations.

| Area | Implemented scope | Implementation |
|---|---|---|
| SHA-2 | SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/224 and SHA-512/256 | `c/crypto/hash/sha256.c`, `c/crypto/hash/sha384.c` |
| Keccak constructions | SHA3-256/512, SHAKE128/256, cSHAKE128/256, KMAC128/256 and KMACXOF128/256 | `c/crypto/pq/keccak.c`, `c/crypto/hash/cshake.c`, `c/crypto/hash/kmac.c` |
| BLAKE hashes | BLAKE2b and BLAKE2s with keyed operation and configurable digest lengths; BLAKE3 hashing, keyed hashing, key derivation and extendable output | `c/crypto/hash/blake2b.c`, `c/crypto/hash/blake2s.c`, `c/crypto/hash/blake3.c` |
| Message authentication and derivation | HMAC, HKDF and PBKDF2 using SHA-224/256/384/512; TLS 1.2 PRF and TLS 1.3 label expansion | `c/crypto/hash/hmac_hkdf.c`, `c/crypto/kdf/pbkdf2.c` |
| Authenticated encryption | AES-128/192/256-GCM, ChaCha20-Poly1305, XChaCha20-Poly1305 and AES-128/256-GCM-SIV | `c/crypto/aead/aesgcm.c`, `c/crypto/aead/chacha20poly1305.c`, `c/crypto/aead/xchacha20poly1305.c`, `c/crypto/aead/aes_gcm_siv.c` |
| Additional AES modes | AES-128/192/256-CTR and CBC with PKCS#7 padding | `c/crypto/aead/aes_modes.c` |
| Memory-based password derivation | scrypt and Argon2d/i/id | `c/crypto/kdf/scrypt.c`, `c/crypto/kdf/argon2.c` |
| Key agreement | X25519, X448 and ECDH on P-256/384/521 | `c/crypto/pubkey/x25519.c`, `c/crypto/pubkey/x448.c`, `c/crypto/pubkey/ecdsa.c` |
| Classical signatures | ECDSA on P-256/384/521; Ed25519 public key derivation, signing and verification; RSA PKCS#1 v1.5 and PSS verification; secp256k1 public key derivation, signing and verification | `c/crypto/pubkey/ecdsa.c`, `c/crypto/pubkey/ed25519.c`, `c/crypto/pubkey/rsa.c`, `c/crypto/pubkey/secp256k1.c` |
| Post-quantum operations | ML-KEM-768 key generation, encapsulation and decapsulation; pure ML-DSA-44/65/87 key generation, signing and verification | `c/crypto/pq/mlkem.c`, `c/crypto/pq/mldsa.c` |

SHA-1 in `c/crypto/hash/sha1.c` serves OCSP certificate identifiers, the WebSocket handshake and the browser digest adapter. Its callers are in `c/net/tls/ocsp.c`, `c/net/http/ws.c` and `c/apps/gui/browser/js/crypto/digest.inc`. The hash directory also contains the CRC-32C error detection checksum in `c/crypto/hash/crc32c.c`.

## Calling the code

Consumers include the relevant header and supply input, output and context buffers. `Makefile` discovers crypto sources for the kernel and lists selected implementations for applications such as login and the network command. Host tests compile primitive sources into their test programs.

For incremental SHA-256 hashing, `sha256_init`, `sha256_update` and `sha256_final` operate on `struct sha256`. The `sha256` function combines those steps. SHA-224 shares that context and update function. SHA-384 and the SHA-512 variants share `struct sha512` and `sha512_update`, with initialization and finalization functions for each digest.

Authenticated encryption interfaces accept a key, nonce, additional authenticated data, message buffers and a tag. `aes128_gcm_open` and `chacha20_poly1305_open` return zero on successful authentication and decryption, or minus one on rejection. Both accumulate differences across all 16 tag bytes before decrypting into the output buffer. These entry points use 12-byte nonces; AES-GCM also provides interfaces accepting an IV length. The implementations are in `c/crypto/aead/aesgcm.c` and `c/crypto/aead/chacha20poly1305.c`.

Check each function’s return contract. For example, `ecdsa_verify` and `ed25519_verify` return one for a valid signature, while `ecdsa_sign` returns zero on success.

Random inputs enter through parameters or callbacks. `ed25519_keypair` takes a randomness callback. ML-KEM exposes deterministic entry points accepting seeds, allowing reproducible comparison with stored answers. The wrappers in `c/crypto/pq/mlkem_rand.c` and `c/crypto/pq/mldsa_rand.c` obtain randomness from the kernel generator. This arrangement lets host tests exercise the core algorithms with controlled inputs.

The scrypt and Argon2 APIs take working memory from the caller and check its capacity before computation. `scrypt_scratch_len` reports bytes; `argon2_memory_blocks` reports the required number of `struct argon2_block` entries. `crypto_wipe`, defined in `c/crypto/crypto.h`, overwrites sensitive storage through a volatile pointer.

## CPU acceleration

AES dispatch operates below the encryption modes. `struct aes_backend` in `c/crypto/aead/aes_backend.h` supplies key expansion, block encryption, block decryption and GHASH field multiplication.

GCM’s counter handling, authentication and tag comparison live in `c/crypto/aead/aesgcm.c`. CTR and CBC mode logic lives in `c/crypto/aead/aes_modes.c`. Sharing the mode implementation reduces duplicated logic and lets tests compare backend primitives directly.

`aes_current_backend` in `c/crypto/aead/aes_dispatch.c` returns the selection, initializing it on first use when needed.

| Backend name | Selection | Operations |
|---|---|---|
| `aesni` | CPUID reports both AES-NI and PCLMULQDQ | AES instructions implement key expansion and block encryption/decryption; PCLMULQDQ implements GHASH multiplication in `c/crypto/aead/aes_ni.c` |
| `c` | Portable fallback, also selectable by tests | Byte-oriented AES and field arithmetic in `c/crypto/aead/aesgcm.c` |

The accelerated backend’s stated motivation is removing memory lookups and branches influenced by secrets. Requiring both CPU features extends that choice to GHASH multiplication.

AES-GCM-SIV uses the selected AES block operations too. Its POLYVAL field arithmetic has its own implementation in `c/crypto/aead/aes_gcm_siv.c`.

Boot calls `cpu_early_init` from `c/boot/long.asm`. It initializes dispatch through `crypto_simd_init` and reports CPU features and the selected backend to serial. `crypto_simd_backend_name` exposes the selection, and `crypto_simd_force_baseline` lets tests select portable C. The accelerated operations use XMM registers, which interrupt entry preserves with FXSAVE/FXRSTOR in `c/boot/isr.asm`.

## Randomness

The kernel generator in `c/kernel/core/rng.c` uses SHA-256 with different domain bytes for reseeding, producing output and advancing internal state. A spinlock protects the shared state. Before generation, it checks accumulated counters against reseed thresholds of 1,024 generation calls and 1 MiB of output.

Entropy collection prefers RDSEED, tries RDRAND when needed, and mixes timestamp and timer values into the seed material. `rng_strong` reports whether a reseed has incorporated at least two successful hardware words. That status stays set across later reseeds, recording hardware input already incorporated into the state.

Kernel consumers call `kernel_random_bytes`. Userland reaches `rng_syscall` through `SYS_GETRANDOM`, declared in `include/abi/logit_abi.h` and dispatched by `c/kernel/exec/syscall.c`. TLS consults `rng_strong` when starting client and server handshakes and when generating a server identity.

## Consumers and trust decisions

Consumers choose algorithms and apply protocol or trust policy around primitive calls.

| Consumer | Use of cryptography |
|---|---|
| TLS | `c/net/tls/tls.c` uses authenticated encryption, HKDF and key agreement, including X25519MLKEM768 hybrid exchange. `c/net/tls/x509.c` dispatches certificate signature verification. `c/net/tls/tls_server.c` uses ECDSA signing. |
| SSH | `c/net/ssh/ssh_kex.c` uses X25519 for key agreement and Ed25519 for the server signature. `c/net/ssh/ssh_packet.c` combines AES-128-CTR with HMAC-SHA-256. |
| Package signatures | `c/crypto/trust/pkgsig.c` signs a hash of the package header using Ed25519. The header includes the payload length and SHA-256 digest. Verification checks the payload digest and signature, and can require signer membership in the package roots. |
| AEX signatures | `c/crypto/trust/aexsig.c` signs a manifest containing the ELF length and SHA-256 digest, using the package root accessors to identify trusted signers. |
| Account records | `c/crypto/kdf/pbkdf2.c` creates and checks PBKDF2-HMAC-SHA256 records carrying the iteration count, salt and derived value. Login and greeter callers use the shared account interfaces in `c/apps/coreutils/accounts.h`. |
| File and download checksums | `c/apps/coreutils/net_digest.inc`, included by `c/apps/coreutils/net.c`, adapts SHA-256, SHA-512, BLAKE2b and BLAKE3. |
| Polaris firmware | `c/drivers/gpu/amd/polaris/resource/files.c` hashes firmware buffers with SHA-256 and compares them with digests generated from `third_party/amd-firmware/MANIFEST`. |

The password implementation records predictable working memory at the login boundary as the reason for choosing PBKDF2. The scrypt and Argon2 interfaces make memory reservation a caller responsibility.

In hardware bring-up builds selected by `make HWHUD=1`, `c/kernel/init/devssh.c` validates the encoding of Ed25519 public key records from a boot module and provisions the `dev` account and SSH files. Its password hash field contains `!`, which `pwhash_check` rejects. The server in `c/apps/services/ssh/sshd.c` matches authentication keys against the installed authorized keys and verifies signed authentication requests. The provisioning hook in `c/kernel/init/kmain.c` runs after a successful root filesystem mount and before login starts.

`Makefile` generates the firmware digest table through `tools/gpu/amd/fwpin.py`. The Polaris loader checks root filesystem ownership and permissions, copies candidate firmware into heap storage, and applies format and digest checks. Firmware supplied through a boot module passes through the same format and digest validation.

Package and AEX signing include different domain strings in the hashes signed by Ed25519, giving each format its own signature input while sharing trusted signer keys. Certificate anchors use the representation in `c/crypto/trust/roots.h`; `c/crypto/trust/roots.c` includes generated anchor data from `c/crypto/trust/roots_bundle.inc`.

## Test vectors and verification

The following table describes verification recipes inspected from source. These commands were not run for this review.

| Command | Coverage |
|---|---|
| `make test-crypto` | Digest, HMAC, HKDF, authenticated encryption, X25519 and AES mode vectors and rejection cases; CPU feature, AES backend, ECDSA, RSA, RNG, ECDH and certificate root checks |
| `make test-crypto-diff` | Reproducible generated cases compared with Python reference calculations, replaying AES cases through available backends |
| `make test-aes-ni` | Published GCM answers, backend selection assertions, and comparisons of AES primitives and modes |
| `make test-ed25519` | RFC 8032 vectors, rejection cases and generated OpenSSL comparisons |
| `make test-mlkem` | Stored OpenSSL answers, malformed encapsulation keys and corrupted ciphertext behavior |
| `make test-mlkem-openssl` | Live OpenSSL comparisons in both directions, implicit rejection and negative controls |
| `make test-pwhash` | Published PBKDF2 vectors, generated OpenSSL comparisons and password record checks |
| `make test-pkg` | Package format, digest, signature and signer trust checks |
| `make crypto2026` | Additional hash, KDF, authenticated encryption, X448, secp256k1 and ML-DSA batteries, reference comparisons and controls |
| `make test-entropy` | Guest checks of access to the kernel generator, including reads across a fork and rejection of a kernel address |
| `make test-simd-os` | Guest serial assertion for the boot SIMD self-test |

`tests/unit/crypto_vec_test.c` contains published digest values, RFC 4231 HMAC cases, RFC 5869 HKDF cases, RFC 8439 authenticated encryption cases and NIST AES mode vectors. Rejection cases modify ciphertexts, tags, keys and other inputs.

`tests/unit/crypto_diff_gen.py` checks its implemented reference algorithms against known answers before generating cases; it also uses Python’s standard hash and HMAC interfaces. `tests/unit/crypto_diff_test.c` compares C results byte for byte. AES backend tests force portable C as well as exercising the CPU-selected path.

ML-DSA tests use a subset of NIST ACVP key generation, signature generation and verification answers stored in `tests/unit/mldsa_kat.inc`. Large expected outputs are represented by SHA-256 digests. AES-GCM-SIV tests use RFC 8452 vectors and its POLYVAL worked example. Their recipes are in `tests/crypto2026.mk`.

Negative controls target specific defects. For example, `make test-aes-ni` requires a control that forces portable C on a capable CPU and checks for the expected dispatch failures. `make test-crypto-diff` requires a control that corrupts SHA-224’s initial state and checks that SHA-224 cases fail while the other digest cases survive.

Guest verification examines interrupt state preservation. After timer initialization, `c/kernel/init/kmain.c` calls `cpu_simd_selftest` from `c/crypto/cpu_report.c`. It probes XMM registers across interrupts, compares the selected AES backend with portable C and runs a published GCM case. Its success verdict requires timer progress during both the register probe and crypto calculations.

## Limitations

- `TRANSPARENCY.md` classifies the cryptographic code as educational project implementations without certification or an external cryptographic audit. Test outcomes apply to the inputs and environment exercised.
- Timing properties vary by primitive and backend. Portable AES uses memory lookups and branches influenced by secrets. NIST EC, secp256k1 and ML-DSA document timing qualifications in their sources. `crypto_simd_constant_time` reports the selected AES backend’s declared property; vector comparisons provide functional checks.
- AVX remains disabled in `c/boot/long.asm` because FXSAVE/FXRSTOR omits the upper halves of YMM registers.
- Timing inputs alone provide weak entropy. TLS refuses handshakes while `rng_strong` is false. The post-quantum randomness wrappers leave entropy policy to their callers.
- CTR and CBC provide encryption without message authentication.
- The AEX loader records signature verdicts and allows loading for absent, invalid, untrusted and trusted signatures, subject to its format and integrity checks. See `c/kernel/exec/load/aex.c`.
- Polaris firmware pins establish a match with the vendored manifest, without vendor signature verification. Developer SSH provisioning persists account and key files across boots, as documented in `c/kernel/init/devssh.h`.
- Coverage depends on CPU features, reference software and corpora. AES backend comparisons require both acceleration features, and the guest SIMD gate checks the selected backend. OpenSSL comparisons can skip unavailable algorithms. An absent BLAKE2s corpus removes that corpus’s keyed vectors; the OpenSSL comparison in `tests/unit/run-blake2s-openssl.sh` supplies additional keyed coverage when its required algorithms are available.

## Source map

| File | What it contains |
|---|---|
| `c/crypto/crypto.h` | Shared primitive interfaces, hash contexts, return contracts and sensitive memory wiping |
| `c/crypto/aead/aes_backend.h`, `c/crypto/aead/aes_dispatch.c` | AES operation table, backend selection, reporting and test hooks |
| `c/crypto/aead/aes_ni.c` | AES-NI and PCLMULQDQ operations |
| `c/crypto/aead/aesgcm.c`, `c/crypto/aead/aes_modes.c`, `c/crypto/aead/aes_gcm_siv.c` | Portable AES, encryption modes and POLYVAL |
| `c/crypto/cpu_report.c` | CPU feature reporting and SIMD interrupt checks |
| `c/boot/long.asm`, `c/boot/isr.asm`, `c/kernel/init/kmain.c` | CPU setup, interrupt state preservation and boot call sites |
| `c/kernel/core/rng.c`, `c/kernel/core/rng.h` | Kernel generator, entropy collection and syscall backend |
| `c/crypto/pq/mlkem.h`, `c/crypto/pq/mldsa.h` | Post-quantum interfaces and parameter sets |
| `c/crypto/kdf/pbkdf2.c`, `c/crypto/kdf/scrypt.h`, `c/crypto/kdf/argon2.h` | Password records and caller memory contracts |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/aexsig.c`, `c/crypto/trust/roots.h` | Package signatures, AEX signatures and certificate anchor representation |
| `c/net/tls/tls.c`, `c/net/tls/tls_server.c`, `c/net/tls/x509.c` | TLS primitive consumers, entropy policy and certificate verification |
| `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h`, `c/apps/services/ssh/sshd.c` | Developer key provisioning and SSH authentication |
| `c/drivers/gpu/amd/polaris/resource/files.c`, `c/drivers/gpu/amd/polaris/resource/files.h` | Firmware loading, format checks and SHA-256 pin comparison |
| `tools/gpu/amd/fwpin.py`, `third_party/amd-firmware/MANIFEST` | Firmware digest table generation and recorded firmware inputs |
| `Makefile`, `tests/crypto.mk`, `tests/crypto2026.mk`, `tests/tlsx.mk`, `tests/ci.mk` | Source selection and verification recipes |
| `tests/unit/crypto_vec_test.c`, `tests/unit/crypto_diff_gen.py`, `tests/unit/crypto_diff_test.c`, `tests/unit/aes_ni_test.c` | Published vectors, reference calculations and backend comparisons |
| `tests/unit/mlkem_test.c`, `tests/unit/mldsa_test.c`, `tests/unit/aes_gcm_siv_test.c` | ML-KEM, ML-DSA and AES-GCM-SIV checks |
| `tests/boot/run-entropy-test.sh` | Guest entropy interface checks |
| `TRANSPARENCY.md` | Project claim rules and security qualifications |

## See also

- [Hashes, AEADs and KDFs](symmetric-and-hashing.md)
- [Public-key and post-quantum cryptography](public-key-and-post-quantum.md)
- [Trust roots and package signing](trust-and-signing.md)
- [TLS and certificates](../networking/tls.md)
- [Time, randomness and settings](../kernel/time-rng-settings.md)
- [Security model](../architecture/security-model.md)
- [Testing](../architecture/testing.md)