# Public-key and post-quantum cryptography

LogitOS implements classical signatures and key agreement in `c/crypto/pubkey`, with ML-KEM and ML-DSA in `c/crypto/pq`. RSA and ECDSA support TLS authentication, Ed25519 serves SSH and package signatures, and TLS 1.3 combines ML-KEM-768 with X25519 to derive session secrets. Primitive APIs handle arithmetic and byte encodings; their callers supply protocol framing, algorithm selection and trust policy.

## API organisation

`c/crypto/crypto.h` declares RSA, ECDSA, ECDH, Ed25519 and X25519. X448 and secp256k1 use `c/crypto/pubkey/x448.h` and `c/crypto/pubkey/secp256k1.h`. The post-quantum interfaces live in `c/crypto/pq/mlkem.h` and `c/crypto/pq/mldsa.h`.

Signature verifiers return `1` for acceptance and `0` for rejection. ECDSA signing, secp256k1 signing and public-key derivation, and NIST ECDH return `0` on success and `-1` on failure. Ed25519 signing and X25519/X448 operations return `void` and write through output buffers.

The post-quantum cores accept seeds or randomisers so tests can reproduce their outputs. Wrappers in `c/crypto/pq/mlkem_rand.c` and `c/crypto/pq/mldsa_rand.c` obtain randomness through `kernel_random_bytes`. TLS client and server handshake entry points check `rng_strong` in `c/net/tls/tls.c` and `c/net/tls/tls_server.c`.

## RSA verification

`rsa_pkcs1_verify` and `rsa_pss_verify` accept a modulus, public exponent, signature and message digest. Integer inputs use big-endian encoding. Digest lengths of 32, 48 and 64 bytes select SHA-256, SHA-384 and SHA-512.

The public operation in `c/crypto/pubkey/rsa.c` uses Montgomery multiplication over 64-bit limbs. Using wider limbs reduces the number of word products in each modular multiplication.

| API | Encoded-message checks |
|---|---|
| `rsa_pkcs1_verify` | Checks the `0x00 0x01` prefix, at least eight `0xff` padding bytes, the padding separator, the selected digest identifier and an exact fit for the digest. |
| `rsa_pss_verify` | Checks the trailer and leading bit, unmasks the data block with MGF1, recovers the salt and recomputes the encoded hash. |

The PSS comment in `c/crypto/crypto.h` describes a salt length equal to the digest length. The implementation in `c/crypto/pubkey/rsa.c` accepts the salt length recovered from the padding separator.

Certificate validation calls both verifiers in `c/net/tls/x509.c`. TLS 1.3 handshake authentication uses PSS in `c/net/tls/tls.c`, while TLS 1.2 supports both encodings in `c/net/tls/tls12.c`. OCSP verification uses PKCS#1 v1.5 in `c/net/tls/ocsp.c`. SSH public-key authentication uses PKCS#1 v1.5 for `rsa-sha2-256` and `rsa-sha2-512` in `c/net/ssh/ssh_pubkey.c`.

## ECDSA and ECDH on NIST curves

`c/crypto/pubkey/ecdsa.c` shares curve parameters and integer arithmetic across ECDSA verification, deterministic signing and ECDH.

| Curve argument | Scalar or coordinate bytes | ECDH public-point bytes | Raw signature bytes |
|---|---:|---:|---:|
| `256` | 32 | 65 | 64 |
| `384` | 48 | 97 | 96 |
| `521` | 66 | 133 | 132 |

The ECDH and verification comments in `c/crypto/crypto.h` list P-256 and P-384. The implementation’s `curve_by_id` also selects P-521. Its coordinates occupy 66 bytes, with one significant bit in the leading byte; buffer sizing must use that width.

| API | Contract |
|---|---|
| `ecdsa_verify` | Reads public coordinates as `X || Y`, a signature as `r || s`, and a message digest. |
| `ecdsa_sign` | Takes a private scalar, digest, blinding value and HMAC callback; writes `r || s`. |
| `ecdh_keygen` | Derives `0x04 || X || Y` from a private scalar and blinding value. |
| `ecdh_shared` | Takes a private scalar, blinding value and peer point with its length; writes the shared point’s X coordinate. |

Scalars, coordinates and signature components use big-endian encoding at the curve’s fixed width. Verification checks both signature components against the curve order, checks coordinate ranges and validates the public point’s curve equation. ECDH checks the peer’s uncompressed encoding and length as well. Private scalars must lie between one and the curve order minus one. Callers drawing random scalars retry after rejection, preserving a uniform distribution without reduction.

`ecdsa_sign` derives its nonce using RFC 6979 and accepts digest lengths of 32, 48 or 64 bytes. The same key, digest and HMAC implementation reproduce the signature. Caller-supplied blinding adds a multiple of the curve order to the scalar used for point multiplication, preserving the resulting point.

The `ecdsa_hmac_fn` callback lets host tests link the arithmetic file without linking the hash implementation. TLS and browser callers pass `hmac`.

TLS maps its NIST key-agreement groups to P-256 and P-384 in `c/net/tls/tls.c`. Certificate validation accepts P-256, P-384 and P-521 in `c/net/tls/x509.c`; the server signs authentication messages through `ecdsa_sign` in `c/net/tls/tls_server.c`. SSH public-key authentication verifies signatures for all three curves in `c/net/ssh/ssh_pubkey.c`. The browser bridge uses these primitives for ECDSA and ECDH in `c/apps/gui/browser/js/crypto/crypto_keys.inc`.

## Ed25519

Ed25519 uses a 32-byte private seed, a 32-byte encoded public key and a 64-byte signature. Its declarations are in `c/crypto/crypto.h`.

| API | Contract |
|---|---|
| `ed25519_pubkey` | Derives a public key from a seed. |
| `ed25519_keypair` | Obtains a seed through a random-byte callback and derives its public key; returns `0` on success or `-1` for a missing callback or a seed of all zeros. |
| `ed25519_sign` | Signs message bytes using a seed and its matching public key. |
| `ed25519_verify` | Verifies a signature over message bytes. |

`c/crypto/pubkey/ed25519.c` expands the seed with SHA-512 and derives the signing nonce from the expanded key and message. Scalar multiplication uses a fixed loop with masked point selection. Verification checks the signature scalar’s canonical range, decodes the public point, recomputes the expected point and compares its encoding with the signature.

The bytes signed depend on the consumer:

| Consumer | Bytes presented to Ed25519 |
|---|---|
| Certificate validation in `c/net/tls/x509.c` | The certificate’s original signed bytes. |
| SSH host authentication in `c/net/ssh/ssh_kex.c` | The 32-byte exchange hash. |
| SSH public-key authentication in `c/net/ssh/ssh_pubkey.c` | The authentication data supplied by the caller. |
| Package signatures in `c/crypto/trust/pkgsig.c` | A SHA-256 digest covering the domain string and signed header, including the payload digest. |
| AEX signatures in `c/crypto/trust/aexsig.c` | A SHA-256 digest covering the domain string, ELF size and ELF digest. |

The AEX loader records the signature verdict in `c/kernel/exec/load/aex.c`. The browser bridge exposes Ed25519 signing and verification through `c/apps/gui/browser/js/crypto/crypto_keys.inc`.

## X25519 and X448

These APIs perform scalar multiplication on Montgomery curves using little-endian scalars and coordinates.

| Property | X25519 | X448 |
|---|---|---|
| Scalar multiplication | `x25519` | `x448` |
| Public-key derivation | `x25519_base` | `x448_base` |
| Scalar, coordinate and output width | 32 bytes | 56 bytes |
| Base-point coordinate | 9 | 5 |
| Declaration | `c/crypto/crypto.h` | `c/crypto/pubkey/x448.h` |

Both implementations clamp the scalar internally and use a ladder with masked conditional swaps. X25519 uses five limbs of 51 bits in `c/crypto/pubkey/x25519.c`. X448 uses eight limbs of 56 bits through `c/crypto/pubkey/field448.c`, placing the modulus’s middle term on a limb boundary. Its scalar clamp clears the lowest two bits and sets the highest bit.

TLS validates share lengths and rejects derived secrets consisting entirely of zeros in `c/net/tls/tls.c`. X448 participates in TLS 1.2 client and TLS 1.3 client/server key agreement through the shared key-agreement helpers. SSH uses X25519 and checks its result in `c/net/ssh/ssh_kex.c`. The browser bridge also uses X25519 for derivation and rejects a zero result.

## secp256k1

`c/crypto/pubkey/secp256k1.h` exposes ECDSA over secp256k1.

| API | Contract |
|---|---|
| `secp256k1_keygen` | Takes a 32-byte big-endian private scalar and a blinding value; writes a 65-byte `0x04 || X || Y` public point. |
| `secp256k1_sign` | Takes a private scalar, digest, caller-supplied nonce and blinding value; writes a 64-byte `r || s` signature. |
| `secp256k1_verify` | Verifies a raw signature against an uncompressed public point. |
| `secp256k1_verify_der` | Parses a DER signature and verifies it against an uncompressed public point. |

The signing nonce must lie between one and the curve order minus one. The header requires uniformly random nonces with no reuse under the same key: reuse or prediction can expose the private scalar. Signing returns `-1` for invalid scalars or zero signature components, allowing the caller to retry with a fresh nonce.

`c/crypto/pubkey/secp256k1.c` uses generic Barrett reduction and point doubling for the curve coefficient `a = 0`. The NIST implementation’s doubling formula uses an `a = -3` shortcut. Owning its point arithmetic lets secp256k1 use the appropriate formula. Its DER parser checks lengths, integer encodings and complete consumption of the signature before calling the raw verifier.

## ML-KEM-768 and hybrid TLS

`c/crypto/pq/mlkem.h` defines these buffer sizes:

| Value | Bytes |
|---|---:|
| Encapsulation key | 1184 |
| Decapsulation key | 2400 |
| Ciphertext | 1088 |
| Shared secret | 32 |

`mlkem768_keygen_derand` derives a key pair from two 32-byte seeds. `mlkem768_encaps_derand` takes an encapsulation key and a 32-byte random input, producing a ciphertext and shared secret. Encapsulation returns `0` on success or `-1` when encoded key coefficients fail the modulus check. `mlkem768_keygen` and `mlkem768_encaps` obtain their random inputs from the kernel RNG.

`mlkem768_decaps` returns `void` and writes a shared secret. It decrypts and re-encrypts the ciphertext, then uses a mask to select between the candidate secret and a rejection secret derived from the private rejection seed and ciphertext. This implicit rejection keeps ciphertext validity out of the API’s return value. The comparison and selection are implemented in `c/crypto/pq/mlkem.c`.

The arithmetic operates on polynomials with 256 coefficients. Its number theoretic transform has seven layers and multiplication combines pairs of coefficients as quadratic components. Matrix rows are generated as needed to reduce stack storage. SHA-3 and SHAKE come from `c/crypto/pq/keccak.c`.

TLS 1.3 combines ML-KEM-768 and X25519 under `X25519MLKEM768`. `c/net/tls/tls_int.h` defines the layouts:

| Value | Layout | Bytes |
|---|---|---:|
| Client share | ML-KEM encapsulation key followed by X25519 public key | 1216 |
| Server share | ML-KEM ciphertext followed by X25519 public key | 1120 |
| Combined secret | ML-KEM secret followed by X25519 secret | 64 |

The client generates the ML-KEM key pair and decapsulates the server’s ciphertext. The server encapsulates to the client’s key through `tls_srv_kem_reply`. Both roles call `tls_hybrid_combine` in `c/net/tls/tls.c`, which rejects a zero X25519 result and constructs the input to the TLS key schedule. Sharing the combiner gives both roles the same secret ordering and result check.

## ML-DSA

`c/crypto/pq/mldsa.h` defines the `mldsa_params` type and three parameter objects.

| Parameter object | Algorithm | Public key bytes | Secret key bytes | Signature bytes |
|---|---|---:|---:|---:|
| `mldsa44_params` | ML-DSA-44 | 1312 | 2560 | 2420 |
| `mldsa65_params` | ML-DSA-65 | 1952 | 4032 | 3309 |
| `mldsa87_params` | ML-DSA-87 | 2592 | 4896 | 4627 |

A shared engine in `c/crypto/pq/mldsa.c` takes the parameter object to select dimensions, sampling bounds and encoding widths. Its polynomial arithmetic uses an eight-layer transform and multiplication of individual coefficients in the transform domain. Callers size key and signature buffers from the selected parameter object.

`mldsa_keygen_internal` derives keys from a 32-byte seed. `mldsa_sign_internal` and `mldsa_verify_internal` consume a formatted message representative. The public `mldsa_sign` and `mldsa_verify` wrappers construct it as `0x00 || context length || context || message`, with the context length encoded in one byte.

Signing takes a 32-byte randomiser. A randomiser consisting entirely of zeros selects deterministic signing; `mldsa_sign_hedged` obtains random bytes from the kernel RNG. `mldsa_keygen` similarly wraps seeded key generation.

Signing returns `0` on success and `-1` on failure, including exhaustion of the 1000-attempt rejection-loop bound. Verification returns `1` for acceptance and `0` for rejection. Its checks include hint encoding, coefficient bounds and the recomputed challenge hash. The public wrappers also reject unsupported context or formatted-message lengths.

## Test entry points

The following host test recipes cover vectors, rejection cases and comparisons with other implementations.

| Command | Defined coverage |
|---|---|
| `make test-crypto` | Classical vectors, RSA and ECDSA verification, ECDH, and supporting crypto checks. |
| `make test-ecdsa-sign` | Deterministic signatures compared with a Python RFC 6979 reference, plus OpenSSL verification. |
| `make test-ed25519` | RFC 8032 vectors, rejection cases and generated OpenSSL comparisons when available. |
| `make test-x448` | RFC 7748 vectors and Wycheproof cases. |
| `make test-secp256k1` | Domain parameters, Wycheproof verification and local signing checks. |
| `make test-mlkem-openssl` | OpenSSL comparisons, including corrupted ciphertexts and implicit rejection. |
| `make test-mldsa` | A subset of NIST ACVP key-generation, deterministic-signing and verification vectors. |
| `make test-tls-pq` | Hybrid TLS handshakes through the OpenSSL interoperability harness. |

The targets are defined in `Makefile`, `tests/crypto.mk`, `tests/crypto2026.mk` and `tests/tlsx.mk`. `tests/crypto2026.mk` also defines OpenSSL comparisons for X448, secp256k1 and ML-DSA.

Controls inject defects into curve arithmetic, nonce derivation, sampling and rejection logic. For example, the ECDSA signing control changes RFC 6979 separator bytes. The resulting signatures can still verify, while comparison with the expected signature detects the changed nonce derivation.

## Limitations

- `TRANSPARENCY.md` describes the cryptographic implementations as experimental and without certification or an independent audit for production use. The test recipes above establish coverage to run, not a current passing result. OpenSSL comparisons depend on the reference implementation’s capabilities and may report a skip.
- RSA provides verification, with a modulus bound of 516 bytes. PSS assumes a modulus whose bit length fills its byte width; its verifier recovers the salt length without enforcing equality to the digest length. See `c/crypto/pubkey/rsa.c`.
- NIST EC and secp256k1 secret operations use arithmetic with variable timing. ML-DSA documents its arithmetic as variable timing and its kernel stack use as unbudgeted. Although Ed25519’s scalar multiplication uses masked selection, `sc_muladd` in `c/crypto/pubkey/ed25519.c` contains loops controlled by carry values, so the fixed multiplication loop does not establish constant timing for the whole signer.
- secp256k1 and ML-DSA have primitive tests but no protocol or trust-path consumers in the current C sources. secp256k1 delegates nonce generation and low-S policy to callers.
- ML-KEM implements the 768 parameter set. ML-DSA implements pure signatures; HashML-DSA and external-mu entry points are absent. Its public wrappers check a context limit of 255 bytes and a formatted-message limit of 16,384 bytes.
- Randomised post-quantum wrappers rely on callers to enforce RNG-strength policy. X25519 and X448 callers own protocol checks for a zero secret. Raw APIs also rely on callers to provide buffers of the declared sizes.
- AEX signature verdicts are advisory. Unsigned images and images with well-formed but invalid or untrusted signature records can proceed through the loader’s other checks in `c/kernel/exec/load/aex.c`.

## Source map

| File | What it contains |
|---|---|
| `c/crypto/crypto.h` | Classical public-key declarations, encodings and callback types. |
| `c/crypto/pubkey/rsa.c` | RSA public operation, PKCS#1 v1.5 and PSS verification. |
| `c/crypto/pubkey/ecdsa.c` | NIST curves, deterministic ECDSA signing, verification, ECDH and scalar blinding. |
| `c/crypto/pubkey/ed25519.c` | Ed25519 key derivation, signing, verification and scalar arithmetic. |
| `c/crypto/pubkey/x25519.c` | X25519 field arithmetic and ladder. |
| `c/crypto/pubkey/x448.h`, `c/crypto/pubkey/x448.c` | X448 interface, clamping and ladder. |
| `c/crypto/pubkey/field448.c` | Field arithmetic for X448. |
| `c/crypto/pubkey/secp256k1.h`, `c/crypto/pubkey/secp256k1.c` | secp256k1 contracts, arithmetic and DER signature parsing. |
| `c/crypto/pq/mlkem.h`, `c/crypto/pq/mlkem.c` | ML-KEM-768 sizes, seeded operations and implicit rejection. |
| `c/crypto/pq/mldsa.h`, `c/crypto/pq/mldsa.c` | ML-DSA parameters, message formatting and signature engine. |
| `c/crypto/pq/mlkem_rand.c`, `c/crypto/pq/mldsa_rand.c` | Wrappers using kernel randomness. |
| `c/crypto/pq/keccak.c` | SHA-3 and SHAKE implementations. |
| `c/net/tls/tls.c`, `c/net/tls/tls_int.h`, `c/net/tls/tls_server.c` | TLS key agreement, hybrid layouts, entropy checks and server signing. |
| `c/net/tls/x509.c`, `c/net/tls/tls12.c`, `c/net/tls/ocsp.c` | Certificate, handshake and OCSP signature verification. |
| `c/net/ssh/ssh_kex.c`, `c/net/ssh/ssh_pubkey.c` | SSH key exchange, host signing and public-key authentication verification. |
| `c/crypto/trust/pkgsig.c`, `c/crypto/trust/aexsig.c` | Ed25519 package and AEX signature construction and verification. |
| `c/kernel/exec/load/aex.c` | AEX signature verdicts and loading policy. |
| `c/apps/gui/browser/js/crypto/crypto_keys.inc` | Browser bridge to classical curve operations. |
| `Makefile`, `tests/crypto.mk`, `tests/crypto2026.mk`, `tests/tlsx.mk` | Test entry points, comparison harnesses and controls. |
| `TRANSPARENCY.md` | Claim rules and cryptographic assurance limits. |

## See also

- [Cryptography overview](overview.md)
- [Hashes, AEADs and KDFs](symmetric-and-hashing.md)
- [Trust roots and package signing](trust-and-signing.md)
- [TLS and certificates](../networking/tls.md)
- [SSH](../networking/ssh.md)
- [Applications and AEX packages](../architecture/applications-aex.md)