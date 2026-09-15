# tests/crypto2026.mk -- the 2026-08-28 crypto primitive batch: thirteen new
# algorithms, their known-answer gates, and every one of their negative
# controls.
#
# IN ITS OWN FRAGMENT, for the reason tests/frameworks.mk's header gives and
# this batch proved the hard way: several lines edit the top-level Makefile at
# once and a whole-file overwrite deletes targets written straight into it.
# Fourteen agents wrote these primitives in parallel; not one of them was
# allowed to touch a .mk. Only the single `-include tests/crypto2026.mk` line
# lives in the Makefile.
#
# ============================================================================
# WHAT THESE ARE, AND THE ONE CLAIM THEY DO NOT MAKE
# ============================================================================
# Every algorithm below is a LIBRARY PRIMITIVE. Not one of them is reachable
# from c/crypto/trust, from the TLS handshake or certificate path, from
# aex.c's package check, or from login. That is deliberate and it is the whole
# reason this batch is not an instance of CLAUDE.md's category (b), "built
# with no real consumer".
#
# Read that category carefully before adding to this file. Its expensive
# members -- fs_prefix with a grant, a ceiling check, inheritance and /proc
# printing and ZERO enforcement points; .lpk signatures with a root key
# compiled into the kernel while aex.c checks only CRC32 -- are expensive
# because THE PRESENCE OF THE CODE MAKES A FALSE CLAIM. A package looks
# signed and is not; a prefix looks enforced and is not. A hash function with
# a KAT gate claims nothing to anybody. It is a library function, and a
# library is exactly the place where breadth is the product.
#
# So: WIRING ANY OF THESE INTO A TRUST PATH IS A SEPARATE DECISION WITH ITS
# OWN ARGUMENT. Shipping a primitive with official vectors is not that
# decision, and this fragment must never become the place it quietly happens.
#
# ============================================================================
# THE ONE THING THAT MAKES A CRYPTO GATE REAL: THE VECTORS ARE NOT OURS
# ============================================================================
# Every positive gate below is answered by something written by somebody else:
# a NIST ACVP/CAVP file, an RFC's own appendix, the algorithm's reference
# implementation's KAT, Project Wycheproof, or a live OpenSSL 3.6.3
# differential. This tree already states the principle for mini-libc --
# "correct means agrees with glibc, which gives every function a REFERENCE
# instead of a hand-written expectation that only records what its author
# already believed" -- and built test-mlkem-openssl for exactly this reason.
# A vector generated from our own implementation proves only that our code
# agrees with itself.
#
# Where an official corpus does not exist for an algorithm, the row says so
# below rather than substituting a self-generated one.
#
# ============================================================================
# EVERY GATE HERE HAS A CONTROL, AND EVERY CONTROL WAS WATCHED FAILING
# ============================================================================
# CLAUDE.md rule 5: a control that cannot be watched failing is worse than no
# control, because it reads like one. This tree has live examples --
# test-demux-fuzz-negctl is satisfied by an ASan startup abort that never
# reaches the injected bug; test-bidi-negctl prints "negative control ok" on a
# machine where the corpus does not exist.
#
# So each control below is a -D flag that injects a SPECIFIC, ARGUED defect --
# the transcription slip a careful implementer actually makes -- and each was
# built and run, with the SHAPE of the failure recorded (which cases go red
# AND which stay green), not merely a nonzero exit. Where the surviving greens
# are the interesting half, the recipe says which they are.
#
# EVERY CONTROL IS A PREREQUISITE OF ITS POSITIVE, never a sibling named on a
# ci-host: line. CLAUDE.md: 61 controls in this tree are "stranded" because
# NOT_CI drops every test-*-negctl from the suite listing on the ground that a
# control is "run by its positive counterpart", and nothing checked that.
# Naming a control on ci-host: instead satisfies the audit and still runs it
# never, which is worse because it looks fixed.
#
# ============================================================================
# THE THIRTEEN, AND ONE THAT DID NOT SURVIVE REVIEW
# ============================================================================
#   test-crc32c        CRC-32C (Castagnoli). RFC 3720 B.4 + the CRC catalogue
#                      check value, against a bit-at-a-time oracle in the
#                      test file. NOT a cryptographic hash and its header
#                      says so; it is here because iSCSI/SCTP/ext4/btrfs all
#                      want it and the reflected-vs-textbook polynomial is
#                      the classic silent-wrong-answer.
#   test-blake2b       BLAKE2b, RFC 7693 App. A + the reference
#                      implementation's own 256 KEYED vectors, COMMITTED as
#                      tests/unit/blake2b_kat.inc.
#   test-blake2s       BLAKE2s -- SEE THE WARNING ON test-blake2s BELOW. Its
#                      keyed corpus is FETCHED, not committed, so with the
#                      corpus absent the gate degrades to a smoke test that
#                      PASSES with its own control's defect compiled in.
#                      Measured, 2026-08-28. This one did NOT survive
#                      adversarial review; it is wired anyway because the .c
#                      is already in c/crypto/hash and therefore already in
#                      CRYPTO_SRC and in the kernel -- an ungated primitive
#                      in the tree is strictly worse than a gated one whose
#                      limits are written down.
#   test-blake3        BLAKE3 hash/keyed_hash/derive_key + XOF, against the
#                      official test_vectors.json.
#   test-cshake        cSHAKE128/256 + KMAC128/256 + KMACXOF (SP 800-185),
#                      against NIST's own published example values.
#   test-scrypt        scrypt (RFC 7914), the RFC's own s11 vectors.
#   test-argon2        Argon2i/d/id (RFC 9106) s5.1-5.3 + the PHC reference
#                      implementation's per-pass block state.
#   test-xchacha       XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha-03),
#                      including the draft's standalone HChaCha20 vector.
#   test-aes-gcm-siv   AES-GCM-SIV (RFC 8452), 50 Appendix C vectors + the
#                      s7 POLYVAL example.
#   test-x448          X448 (RFC 7748) + 498 Wycheproof curve448 cases.
#   test-secp256k1     ECDSA over secp256k1. NO NIST CAVP VECTORS EXIST for
#                      this curve -- the KAT is Wycheproof plus SEC 2 2.4.1's
#                      domain parameters pinned directly, and the OpenSSL
#                      differential is what covers keygen and sign, which
#                      Wycheproof (verify-only) cannot.
#   test-mldsa         ML-DSA-44/65/87 (FIPS 204). FIPS 204 CARRIES NO VECTOR
#                      APPENDIX; the known answers come from NIST's own
#                      ACVP-Server files.
#   test-blake2b/-openssl, and every other *-openssl target: a live
#                      differential against OpenSSL 3.6.3. Each SKIPS (exit 0,
#                      one line naming the missing capability) when the local
#                      openssl cannot do the algorithm -- a missing reference
#                      is not a regression in the code under test, which is
#                      the rule test-mlkem-openssl already follows.
#
# TWO PRIMITIVES IN THIS BATCH WERE NOT BUILT and have no targets here:
# Ed448 (PureEdDSA over edwards448) and SLH-DSA (FIPS 205). No source, no
# test, nothing to wire. Named rather than omitted, because CLAUDE.md's own
# finding is that an absent claim is worse than a stale one.
#
# ============================================================================
# THE OPENSSL ON $PATH IS NOT THE OPENSSL THESE GATES NEED
# ============================================================================
# On this tree's documented host (macOS), `openssl` on $PATH is LibreSSL
# 3.3.6, which has no BLAKE2SMAC, no Argon2, no ML-DSA and no KMAC. Every
# differential script defaults to `openssl` and SKIPS LOUDLY when the
# algorithm is missing -- correct behaviour, and it would have made eleven
# differential gates print "SKIP" forever while reading as wired. So this
# fragment resolves a real OpenSSL 3.x first and hands it to the scripts.
# Override with `make test-x OPENSSL=/path/to/openssl`.
#
# This is CLAUDE.md rule 1 in its cheapest form: is the harness looking at
# the machine, or at itself?
CRYPTO2026_OPENSSL := $(firstword \
    $(wildcard /opt/homebrew/opt/openssl@3/bin/openssl) \
    $(wildcard /usr/local/opt/openssl@3/bin/openssl) \
    $(wildcard /usr/bin/openssl3) \
    openssl)
OPENSSL ?= $(CRYPTO2026_OPENSSL)
# Handed to every script below. BUILD and CC come along because the scripts
# rebuild their own CLIs and must land in the caller's build dir -- several
# agents share one tree and two makes racing to write build/x is CLAUDE.md's
# "a sweep that manufactures bugs".
C26_ENV := BUILD="$(BUILD)" CC="$(CC)" OPENSSL="$(OPENSSL)"

# Sanitisers. darwin/arm64 ASan has no leak detector, so -fsanitize=leak is
# deliberately absent; these are the two that do work here. Kept OFF for the
# two password KDFs (scrypt, argon2) because their whole point is to be slow
# and ASan multiplies that by an order of magnitude.
C26_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all

.PHONY: blake2-fetch \
        test-crc32c test-crc32c-negctl \
        test-blake2b test-blake2b-negctl test-blake2b-openssl test-blake2b-openssl-negctl \
        test-blake2s test-blake2s-negctl test-blake2s-openssl \
        test-blake3 test-blake3-negctl \
        test-cshake test-cshake-negctl test-cshake-openssl \
        test-scrypt test-scrypt-negctl test-scrypt-openssl \
        test-argon2 test-argon2-negctl test-argon2-openssl \
        test-xchacha test-xchacha-negctl test-xchacha-openssl test-xchacha-openssl-negctl \
        test-aes-gcm-siv test-aes-gcm-siv-negctl test-aes-gcm-siv-openssl \
        test-x448 test-x448-negctl test-x448-openssl \
        test-secp256k1 test-secp256k1-negctl test-secp256k1-openssl test-secp256k1-openssl-negctl \
        test-mldsa test-mldsa-negctl test-mldsa-openssl \
        crypto2026

# ---------------------------------------------------------------------------
# CRC-32C (Castagnoli)
# ---------------------------------------------------------------------------
# Vectors: RFC 3720 Appendix B.4 (the iSCSI CRC-32C examples) plus the CRC
# catalogue check value for "123456789". The test also runs a bit-at-a-time
# reference implementation that shares no table with the code under test.
#
# The control, -DCRC32C_BAD_POLY, is the reflected/textbook polynomial mixup:
# it feeds 0x1EDC6F41 (the correctly-transcribed NON-reflected constant) into
# the loop shaped for the REFLECTED algorithm. Watched failing 2026-08-28:
# "4 passed, 16 failed", exit 1. The four survivors are the informative half
# and they are structurally immune by construction -- two incremental-vs-
# one-shot consistency checks (both sides use the same broken table so they
# still agree with each other), the empty-input case (0 bytes never touches
# the table), and the bit-at-a-time oracle's own catalogue check, because
# ref_crc32c hardcodes 0x82F63B78 in its own source where the -D cannot reach
# it. That last one is what proves the oracle is a separate implementation
# and not a second copy of the thing it is judging.
test-crc32c: test-crc32c-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -Werror $(C26_SAN) -o $(BUILD)/crc32c_test \
	    tests/unit/crc32c_test.c c/crypto/hash/crc32c.c -Ic/crypto/hash
	$(BUILD)/crc32c_test

test-crc32c-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -Werror -DCRC32C_BAD_POLY -o $(BUILD)/crc32c_test_negctl \
	    tests/unit/crc32c_test.c c/crypto/hash/crc32c.c -Ic/crypto/hash
	@if $(BUILD)/crc32c_test_negctl >$(BUILD)/crc32c_negctl.log 2>&1; then \
	  echo "CONTROL DID NOT REDDEN: CRC32C_BAD_POLY passed the KAT"; \
	  cat $(BUILD)/crc32c_negctl.log; exit 1; \
	else echo "crc32c negctl ok -- $$(tail -1 $(BUILD)/crc32c_negctl.log)"; fi

# ---------------------------------------------------------------------------
# BLAKE2b (RFC 7693) -- unkeyed and keyed, digest 1..64
# ---------------------------------------------------------------------------
# Vectors: RFC 7693 Appendix A plus the BLAKE2 reference implementation's own
# 256 keyed vectors, which are COMMITTED here as tests/unit/blake2b_kat.inc
# rather than fetched. Contrast test-blake2s below, whose identical corpus is
# fetched -- that single difference is why one of these two survived review.
#
# The control, -DBLAKE2B_BUG_ROT63, is a one-character slip in G's final
# rotation: `x >> 63 | x << 63` instead of `x >> 63 | x << 1`, i.e. both terms
# rotating the same way. Watched failing 2026-08-28: 646/646 -> 133 passed,
# 513 failed, exit 1, with 0/256 official vectors agreeing. Every failure was
# a printed digest MISMATCH; ASan and UBSan were both linked in and stayed
# silent, so the divergence is in the compared bytes and not a sanitizer abort
# standing in for one (the exact way test-demux-fuzz-negctl is satisfied by
# the wrong thing).
test-blake2b: test-blake2b-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/blake2b_test \
	    tests/unit/blake2b_test.c c/crypto/hash/blake2b.c -Ic/crypto/hash -Itests/unit
	$(BUILD)/blake2b_test

test-blake2b-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -DBLAKE2B_BUG_ROT63 \
	    -o $(BUILD)/blake2b_test_negctl tests/unit/blake2b_test.c c/crypto/hash/blake2b.c \
	    -Ic/crypto/hash -Itests/unit
	@if $(BUILD)/blake2b_test_negctl >$(BUILD)/blake2b_negctl.log 2>&1; then \
	  echo "CONTROL DID NOT REDDEN: BLAKE2B_BUG_ROT63 passed the KAT"; exit 1; \
	else echo "blake2b negctl ok -- $$(grep -m1 -E 'passed|failed' $(BUILD)/blake2b_negctl.log)"; fi

# Ours against OpenSSL 3.x BLAKE2b512 / BLAKE2BMAC, byte for byte, over
# unkeyed and keyed cases at digest lengths the RFC's own appendix never
# reaches. Skips (exit 0) if the local openssl lacks either.
test-blake2b-openssl: test-blake2b-openssl-negctl
	@$(C26_ENV) bash tests/unit/run-blake2b-openssl.sh

test-blake2b-openssl-negctl:
	@$(C26_ENV) bash tests/unit/run-blake2b-openssl.sh --controls

# ---------------------------------------------------------------------------
# BLAKE2s (RFC 7693) -- AND THE WARNING THAT COMES WITH IT
# ---------------------------------------------------------------------------
# READ THIS BEFORE QUOTING test-blake2s AS COVERAGE.
#
# The corpus that makes this gate meaningful -- the reference
# implementation's 256 KEYED vectors -- is FETCHED by tools/blake2_fetch.sh
# at the pin in tools/blake2_revision.txt, and is NOT committed. The test
# treats a missing corpus as a missing capability and skips it (exit 0),
# which is the right shape and is what CLAUDE.md's rule demands of a gate
# that cannot run.
#
# But the ONLY vector left when it skips is RFC 7693 Appendix B, which is
# BLAKE2s("abc") unkeyed with a 32-byte digest -- and that is exactly the one
# case the control's defect cannot touch. MEASURED 2026-08-28, both builds
# with the corpus absent:
#
#   ./blake2s_test /nonexistent   -> PASS Appendix B, SKIP kat, exit 0
#   ./blake2s_test /nonexistent   -> PASS Appendix B, SKIP kat, exit 0
#     (second build compiled -DLOGIT_BLAKE2S_BAD_PARAM)
#
# Identical output. The gate is GREEN with its own control's defect compiled
# in. That is why this primitive did not survive adversarial review, and it
# is a textbook instance of CLAUDE.md rule 5 rather than a novel finding.
#
# So the load-bearing gate for BLAKE2s is test-blake2s-openssl, which needs
# no fetch, and that is the one named on ci-host: below. test-blake2s remains
# because with the corpus present it is the stronger of the two, and
# `make blake2-fetch` is one command.
blake2-fetch:
	@bash tools/blake2_fetch.sh $(BUILD)/blake2-kat

test-blake2s: test-blake2s-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/blake2s_test \
	    tests/unit/blake2s_test.c c/crypto/hash/blake2s.c -Ic/crypto/hash
	$(BUILD)/blake2s_test $(BUILD)/blake2-kat/blake2s-kat.txt

# The control lives in the OpenSSL differential, not in the KAT, for the
# reason argued above: with the corpus absent the KAT cannot see this defect.
# --controls asserts the SHAPE -- exactly the unkeyed/32-byte cases survive,
# every keyed or non-32-byte case dies -- so "the binary is broken" cannot
# satisfy it.
test-blake2s-negctl:
	@$(C26_ENV) bash tests/unit/run-blake2s-openssl.sh --controls

# test-blake2s IS A PREREQUISITE HERE, not a sibling on ci-host:. It is safe
# as one because with the corpus absent it SKIPS and exits 0, so it can never
# redden CI for a missing fetch -- and whenever `make blake2-fetch` has run, CI
# gets the 256 keyed vectors for free. Leaving it off the graph entirely was
# the alternative and it is the worse one: audit_tests.py reported it as an
# unwired target, which is how a gate stops being run and nobody notices.
test-blake2s-openssl: test-blake2s-negctl test-blake2s
	@$(C26_ENV) bash tests/unit/run-blake2s-openssl.sh

# ---------------------------------------------------------------------------
# BLAKE3 -- hash, keyed_hash, derive_key, and the XOF
# ---------------------------------------------------------------------------
# Vectors: the official test_vectors.json from the BLAKE3 reference
# repository, committed as tests/unit/blake3_vectors.inc.
#
# The control, -DBLAKE3_BUG_ROOT_ALWAYS, sets the ROOT flag on EVERY chunk's
# final compression instead of only the tree root. Watched failing
# 2026-08-28: "52 passed, 54 failed", and the split is the whole point --
# single-chunk 51/51 PASSED (every input <= 1024 bytes, where the root
# genuinely is the sole chunk and the extra flag is a no-op) and multi-chunk
# 0/54 FAILED, listed by length from 1025 upward. A control that reddened
# everything would not have distinguished "the tree hashing is wrong" from
# "the build is broken"; this one does.
test-blake3: test-blake3-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/blake3_test \
	    tests/unit/blake3_test.c c/crypto/hash/blake3.c -Ic/crypto/hash -Itests/unit
	$(BUILD)/blake3_test

test-blake3-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DBLAKE3_BUG_ROOT_ALWAYS -o $(BUILD)/blake3_test_negctl \
	    tests/unit/blake3_test.c c/crypto/hash/blake3.c -Ic/crypto/hash -Itests/unit
	@if $(BUILD)/blake3_test_negctl >$(BUILD)/blake3_negctl.log 2>&1; then \
	  echo "CONTROL DID NOT REDDEN: BLAKE3_BUG_ROOT_ALWAYS passed the KAT"; exit 1; \
	else echo "blake3 negctl ok -- $$(grep -m1 -E 'passed' $(BUILD)/blake3_negctl.log)"; fi

# ---------------------------------------------------------------------------
# cSHAKE128/256 + KMAC128/256 + KMACXOF (NIST SP 800-185)
# ---------------------------------------------------------------------------
# Vectors: NIST's own published cSHAKE and KMAC example values.
#
# NOTE THE LINK LINE: these are the only new primitives that reach into
# c/crypto/pq for the Keccak permutation. That is also why the root
# Makefile's CRYPTO_SRC had to grow c/crypto/pq/keccak.c -- see the comment
# there; the day cshake.c landed, the glob picked it up and `make test`
# stopped COMPILING.
#
# THE MODE NEVER LIVES IN A BACKEND, and this is the batch's clearest case:
# KMAC is a construction over cSHAKE, and the shortcut kmac.h's own comment
# names -- implement KMAC by calling KMACXOF and truncating -- is wrong,
# because the two differ in their right_encode trailer, not in their length.
# The control, -DKMAC_NEGCTL_XOF_TRAILER, IS that shortcut. Watched failing
# 2026-08-28 in exactly the asymmetric shape that distinguishes it from a
# broken build: "13 passed, 8 failed" -- all four cSHAKE checks and all eight
# identity sub-checks stayed GREEN, all six official KMAC vectors went red,
# plus the two structural checks that KMAC128 is not KMACXOF128 and is not a
# XOF. Against OpenSSL the same control produces exactly NTRIAL*2 failures
# (kmac128 and kmac256 at xof=0) with every kmacxof and cshake case in the
# same trials still green, because KMACXOF's own trailer never changes.
test-cshake: test-cshake-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/cshake_test \
	    tests/unit/cshake_test.c c/crypto/hash/cshake.c c/crypto/hash/kmac.c \
	    c/crypto/pq/keccak.c -Ic/crypto/hash -Ic/crypto/pq -Itests/unit
	$(BUILD)/cshake_test

test-cshake-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DKMAC_NEGCTL_XOF_TRAILER -o $(BUILD)/cshake_test_negctl \
	    tests/unit/cshake_test.c c/crypto/hash/cshake.c c/crypto/hash/kmac.c \
	    c/crypto/pq/keccak.c -Ic/crypto/hash -Ic/crypto/pq -Itests/unit
	@if $(BUILD)/cshake_test_negctl >$(BUILD)/cshake_negctl.log 2>&1; then \
	  echo "CONTROL DID NOT REDDEN: KMAC_NEGCTL_XOF_TRAILER passed the KAT"; exit 1; \
	else echo "cshake negctl ok -- $$(grep -m1 -E 'passed|failed' $(BUILD)/cshake_negctl.log)"; fi
	@$(C26_ENV) bash tests/unit/run-cshake-openssl.sh --controls

test-cshake-openssl: test-cshake-negctl
	@$(C26_ENV) bash tests/unit/run-cshake-openssl.sh

# ---------------------------------------------------------------------------
# scrypt (RFC 7914)
# ---------------------------------------------------------------------------
# Vectors: RFC 7914 section 11, plus the Salsa20/8 core and scryptBlockMix
# worked examples from sections 8 and 9. The RFC's fourth vector
# (N=1048576) needs ~1 GiB of scratch and is skipped by default; the test
# SAYS SO in its output rather than silently omitting it, and
# SCRYPT_TEST_BIG=1 turns it on.
#
# No sanitisers here: scrypt at N=16384,r=8 is deliberately expensive and
# ASan makes the gate cost minutes for nothing this particular code can hide.
#
# The control, -DSCRYPT_BREAK_INTERLEAVE, writes scryptBlockMix's output in
# produced order instead of RFC 7914 s4 step 3's interleaved order. It is the
# reason this gate has r=8 vectors at all: AT r=1 THE TWO ORDERS ARE
# IDENTICAL, so a suite built only from r=1 cases would let this control pass
# silently. Watched failing 2026-08-28: "9 passed, 2 failed" -- the Salsa20/8
# core, the in-place core, blockmix at r=1, ROMix at r=1, and full scrypt
# test 1 (N=16,r=1,p=1) all still read "ok"; only the two r=8 vectors died.
# run-scrypt-negctl.sh asserts that exact line-by-line shape with grep, not a
# nonzero exit, so an unrelated crash cannot satisfy it.
test-scrypt: test-scrypt-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/scrypt_test tests/unit/scrypt_test.c \
	    c/crypto/kdf/scrypt.c c/crypto/hash/sha256.c -Ic/crypto -Ic/crypto/kdf -Itests/unit
	$(BUILD)/scrypt_test
	@echo "   (N=1048576,r=8,p=1 needs ~1 GiB scratch and was skipped above;"
	@echo "    SCRYPT_TEST_BIG=1 $(BUILD)/scrypt_test includes it)"

test-scrypt-negctl:
	@$(C26_ENV) bash tests/unit/run-scrypt-negctl.sh

test-scrypt-openssl: test-scrypt-negctl
	@$(C26_ENV) bash tests/unit/run-scrypt-openssl.sh

# ---------------------------------------------------------------------------
# Argon2i / Argon2d / Argon2id (RFC 9106)
# ---------------------------------------------------------------------------
# Vectors: RFC 9106 sections 5.1/5.2/5.3 (all three types, with a non-empty
# secret AND associated data, which is the half most implementations never
# exercise) plus the PHC reference implementation's own per-pass BLOCK STATE,
# compared through the ARGON2_TEST_HOOKS seam. The block-state comparison is
# there because a defect that cancels out by the final tag is invisible to a
# tag-only KAT.
#
# The control, -DARGON2_NEGCTL_ALWAYS_DATA_DEP, collapses Argon2i's and
# Argon2id's data-INDEPENDENT addressing to Argon2d's data-dependent one --
# "switching, never". It is compiled into BOTH argon2.c and the test, and the
# test knows which pattern its own build must show: Argon2d correct,
# Argon2i/id wrong at the tag AND at the per-pass block level. It exits 0
# only if exactly that holds, so a flag that failed to reach the code shows
# up as exit 1 rather than a silent pass.
#
# AND IT WAS CAUGHT PASSING WHEN IT SHOULD NOT HAVE, which is the finding
# worth keeping: at t=1, m=8 KiB, p=1 the injected defect produced a
# BYTE-IDENTICAL tag on 45 of 60 random trials, because the first reference
# window in any run has exactly one candidate regardless of addressing mode
# and nothing else happens before the run ends. The fix was raising every
# differential case to segment_length >= 8 (0 of 420 coincidental matches
# after). The 75% number is left in run-argon2-openssl.sh's comment rather
# than deleted, because the evidence is the point.
test-argon2: test-argon2-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -DARGON2_TEST_HOOKS -o $(BUILD)/argon2_test \
	    tests/unit/argon2_test.c c/crypto/kdf/argon2.c -Ic/crypto -Ic/crypto/kdf -Itests/unit
	$(BUILD)/argon2_test

test-argon2-negctl:
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -DARGON2_TEST_HOOKS -DARGON2_NEGCTL_ALWAYS_DATA_DEP \
	    -o $(BUILD)/argon2_test_negctl tests/unit/argon2_test.c c/crypto/kdf/argon2.c \
	    -Ic/crypto -Ic/crypto/kdf -Itests/unit
	$(BUILD)/argon2_test_negctl
	@$(C26_ENV) bash tests/unit/run-argon2-openssl.sh --controls

test-argon2-openssl: test-argon2-negctl
	@$(C26_ENV) bash tests/unit/run-argon2-openssl.sh

# ---------------------------------------------------------------------------
# XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha-03)
# ---------------------------------------------------------------------------
# Vectors: the draft's A.1 AEAD vector AND its standalone 2.2.1 HChaCha20
# vector. The second one is not decoration -- see the first control.
#
# chacha20poly1305.c gave up `static` on chacha_block/chacha20 so this file
# could reuse them through chacha_core.h. That is the alternative to a second
# ChaCha core in the tree, which would be the fourth-rasterizer mistake in
# another subsystem.
#
# TWO controls, and they are caught by DIFFERENT gates on purpose:
#
#   -DXCHACHA_BUG_ADD_BACK    HChaCha20 forgetting that it does NOT add the
#                             input state back to the permutation output --
#                             the single most common HChaCha20 bug. Watched
#                             failing: 739 of 804 checks red, including the
#                             standalone HChaCha20 vector. IT IS INVISIBLE TO
#                             THE OPENSSL DIFFERENTIAL and that is documented
#                             in the script rather than hidden: the script
#                             derives openssl's reference material from OUR
#                             subkey, so both sides are wrong together and
#                             agree. Exactly the case run-mlkem-openssl.sh
#                             already records for three of its five defects.
#                             The standalone vector in test-xchacha is what
#                             actually catches it.
#   -DXCHACHA_BUG_NONCE_APPEND  inner nonce built as nonce[16:24] || 0000
#                             instead of 0000 || nonce[16:24] -- a perfectly
#                             valid AEAD that will never interoperate.
#                             Watched failing: 738 of 804 in the KAT, and 4
#                             of 12 in the differential.
test-xchacha: test-xchacha-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/xchacha_test \
	    tests/unit/xchacha20poly1305_test.c c/crypto/aead/xchacha20poly1305.c \
	    c/crypto/aead/chacha20poly1305.c -Ic/crypto -Ic/crypto/aead -Itests/unit
	$(BUILD)/xchacha_test

test-xchacha-negctl:
	@mkdir -p $(BUILD)
	@for CTL in XCHACHA_BUG_ADD_BACK XCHACHA_BUG_NONCE_APPEND; do \
	    $(CC) -O2 -Wall -Wextra -D$$CTL -o $(BUILD)/xchacha_ctl \
	        tests/unit/xchacha20poly1305_test.c c/crypto/aead/xchacha20poly1305.c \
	        c/crypto/aead/chacha20poly1305.c -Ic/crypto -Ic/crypto/aead -Itests/unit || exit 1; \
	    if $(BUILD)/xchacha_ctl > $(BUILD)/xchacha_ctl.log 2>&1; then \
	        echo "CONTROL DID NOT REDDEN: $$CTL passed -- the gate cannot see this defect"; exit 1; \
	    else echo "xchacha negctl ok, $$CTL: $$(tail -1 $(BUILD)/xchacha_ctl.log)"; fi; \
	done

test-xchacha-openssl: test-xchacha-openssl-negctl
	@$(C26_ENV) bash tests/unit/run-xchacha-openssl.sh

test-xchacha-openssl-negctl:
	@$(C26_ENV) bash tests/unit/run-xchacha-openssl.sh --controls

# ---------------------------------------------------------------------------
# AES-GCM-SIV (RFC 8452) -- nonce-misuse-resistant AEAD
# ---------------------------------------------------------------------------
# Vectors: 50 RFC 8452 Appendix C vectors plus the section 7 standalone
# POLYVAL example, both AES-128 and AES-256, transcribed by
# tests/unit/aes_gcm_siv_gen.py into a committed .inc.
#
# The control, -DSIV_CTL_NO_TAG_MASK, drops RFC 8452 section 4's "clear the
# most significant bit of the last byte" before the POLYVAL result is
# encrypted into the tag. Watched failing 2026-08-28: 151/0 -> "113 passed,
# 38 failed". THE COUNT IS THE INTERESTING PART: 38 of 50, not 50 of 50,
# because the defect only bites when that bit happened to be 1 -- it reads as
# flakiness rather than as a bug, which is why a gate that only sampled a few
# vectors would have shipped it. run-aes-gcm-siv-negctl.sh asserts
# baseline-green AND control-red in the same run and reports a broken build
# in either configuration with a different message, so an unrelated failure
# is never scored as a firing control.
test-aes-gcm-siv: test-aes-gcm-siv-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/aes_gcm_siv_test tests/unit/aes_gcm_siv_test.c \
	    c/crypto/aead/aes_gcm_siv.c c/crypto/aead/aes_dispatch.c c/crypto/aead/aes_ni.c \
	    c/crypto/aead/aesgcm.c c/kernel/cpu/cpufeat.c \
	    -Ic/crypto -Ic/crypto/aead $(KCPU_INC) -Itests/unit
	$(BUILD)/aes_gcm_siv_test

test-aes-gcm-siv-negctl:
	@$(C26_ENV) bash tests/unit/run-aes-gcm-siv-negctl.sh

test-aes-gcm-siv-openssl: test-aes-gcm-siv-negctl
	@$(C26_ENV) bash tests/unit/run-aes-gcm-siv-openssl.sh

# ---------------------------------------------------------------------------
# X448 (RFC 7748) + the GF(2^448 - 2^224 - 1) field it needs
# ---------------------------------------------------------------------------
# Vectors: RFC 7748's two scalarmult vectors, its iterated x1/x1000 test, its
# Alice/Bob DH vector, and 498 Wycheproof curve448 XdhComp cases. The RFC's
# 1,000,000-iteration vector costs ~150 s here and is OFF by default; the
# gate says so in its output. LOGIT_X448_MILLION=1 turns it on.
#
# TWO controls, both being X25519's constants worn by X448 -- the exact way
# this gets written wrong by someone who started from x25519.c:
#   -DLOGIT_X448_CTL_BAD_A24    a24 = 121665 (x25519's) instead of 39081.
#                               Watched: 494 of 546 checks red; 9 of 9 in the
#                               openssl differential.
#   -DLOGIT_X448_CTL_BAD_CLAMP  x25519's 3-low-bit clamp (cofactor 8) instead
#                               of x448's 2-low-bit clamp (cofactor 4).
#                               Watched: 262 of 546 red; 6 of 9 in the
#                               differential, and the asymmetry across
#                               keygen/derive-a/derive-b is itself the
#                               signature of a clamp bug rather than a build
#                               failure.
# Both had their baseline re-confirmed green in both harnesses first, so the
# redness is attributable to the injected defect and not to a broken harness.
test-x448: test-x448-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/x448_test \
	    tests/unit/x448_test.c c/crypto/pubkey/x448.c c/crypto/pubkey/field448.c \
	    -Ic/crypto/pubkey -Itests/unit
	$(BUILD)/x448_test

test-x448-negctl:
	@$(C26_ENV) bash tests/unit/run-x448-kat-negctl.sh

test-x448-openssl: test-x448-negctl
	@$(C26_ENV) bash tests/unit/run-x448-openssl.sh

# ---------------------------------------------------------------------------
# ECDSA over secp256k1
# ---------------------------------------------------------------------------
# NO NIST CAVP VECTORS EXIST FOR THIS CURVE -- it is a SEC 2 curve, not a
# NIST one, and saying so is cheaper than pretending. The known answers are
# Project Wycheproof's ecdsa_secp256k1_sha256_test.json (476 cases, committed
# with the source file's own sha256 in the header) plus SEC 2 v2.0 section
# 2.4.1's domain parameters pinned directly as G, 2G. Wycheproof is
# VERIFY-ONLY, so keygen and sign are covered by the OpenSSL differential and
# by nothing else -- that is why the differential is not optional decoration
# here the way it is for a hash function.
#
# The control, -DSECP256K1_CTL_NIST_DOUBLE, wires ecdsa.c's NIST a = -3
# Jacobian doubling shortcut (M = 3(X-Z^2)(X+Z^2)) onto this curve, whose a
# is 0. Watched failing 2026-08-28 three ways: the KAT went 492/0 -> 171
# failed, and the split is exact -- ALL 168 Wycheproof "valid" cases flipped
# to invalid and EVERY "invalid" case stayed invalid, which is the shape a
# broken verifier has and a broken build does not; the openssl differential
# reddened keygen, our-sig-to-openssl and openssl-sig-to-us; and both
# variants still compile clean under the freestanding x86_64-elf target, so
# the control is a semantic difference and not a build artifact.
#
# Honest caveat kept from the implementer rather than smoothed over: in THIS
# implementation the corrupted keygen produces a point on y^2 = x^3 - 3x + 7,
# which point_on_curve() rejects on its own because it hardcodes a=0/b=7
# independently of the doubling code. So the defect would be caught here even
# without an external oracle. The Wycheproof/openssl gates are still what
# carry it, and would be the only thing carrying it in an implementation that
# shared that check with the doubling.
test-secp256k1: test-secp256k1-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/secp256k1_test \
	    tests/unit/secp256k1_test.c c/crypto/pubkey/secp256k1.c c/crypto/hash/sha256.c \
	    -Ic/crypto -Ic/crypto/pubkey
	$(BUILD)/secp256k1_test < tests/unit/secp256k1_wycheproof.inc

test-secp256k1-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -DSECP256K1_CTL_NIST_DOUBLE -Wall -Wextra $(C26_SAN) \
	    -o $(BUILD)/secp256k1_test_negctl tests/unit/secp256k1_test.c \
	    c/crypto/pubkey/secp256k1.c c/crypto/hash/sha256.c -Ic/crypto -Ic/crypto/pubkey
	@if $(BUILD)/secp256k1_test_negctl < tests/unit/secp256k1_wycheproof.inc \
	      >$(BUILD)/secp256k1_negctl.log 2>&1; then \
	  echo "CONTROL DID NOT REDDEN: SECP256K1_CTL_NIST_DOUBLE changed nothing the KAT could see"; exit 1; \
	else echo "secp256k1 negctl ok -- $$(tail -1 $(BUILD)/secp256k1_negctl.log)"; fi

test-secp256k1-openssl: test-secp256k1-openssl-negctl
	@$(C26_ENV) bash tests/unit/run-secp256k1-openssl.sh

test-secp256k1-openssl-negctl:
	@$(C26_ENV) SECP256K1_CTL=SECP256K1_CTL_NIST_DOUBLE bash tests/unit/run-secp256k1-openssl.sh

# ---------------------------------------------------------------------------
# ML-DSA-44 / 65 / 87 (FIPS 204)
# ---------------------------------------------------------------------------
# FIPS 204 CARRIES NO VECTOR APPENDIX. The known answers here come from
# NIST's own ACVP-Server files (keyGen, sigGen, sigVer), committed as
# tests/unit/mldsa_kat.inc, and the second oracle is a live OpenSSL 3.6+
# ML-DSA differential over all three parameter sets in both verify
# directions.
#
# THREE controls, and the third is the one worth reading:
#   -DMLDSA_CTL_NO_TRANSPOSE   ExpandA absorbs (i,j) instead of (j,i) --
#                              column vs row. KAT 108 -> 96, differential
#                              24 of 48 red. Self-consistent with itself,
#                              interoperable with nothing; the identical
#                              control ML-KEM already has.
#   -DMLDSA_CTL_NO_KL_DOMAIN   keygen expands H(xi) instead of H(xi||k||l).
#                              KAT 108 -> 96, differential 24 of 48 red.
#   -DMLDSA_CTL_WEAK_ZBOUND    drops the ||r0||inf >= gamma2-beta rejection
#                              inside the signing loop. THIS ONE PRODUCES
#                              SIGNATURES THAT VERIFY -- against our own
#                              verifier AND against OpenSSL's. KAT 108 -> 104
#                              (only 4 of 6 sigGen signature-HASH checks
#                              failed; every keyGen, every sigVer and every
#                              round trip stayed green), and in the
#                              differential only the byte-exact "B sign"
#                              check failed, 3 of 48, while cross-verify
#                              passed both directions every time. That is the
#                              sharpest evidence in this batch for why a
#                              round-trip or accept/reject test is not a gate
#                              for a rejection-sampling loop: the ONLY thing
#                              that sees it is a byte-exact comparison
#                              against somebody else's implementation.
# The baseline was re-confirmed green (108/108, 48/48) immediately before
# each control run, so a red baseline could never be mistaken for a firing
# control.
test-mldsa: test-mldsa-negctl
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra $(C26_SAN) -o $(BUILD)/mldsa_test \
	    tests/unit/mldsa_test.c c/crypto/pq/mldsa.c c/crypto/pq/keccak.c \
	    c/crypto/hash/sha256.c -Ic/crypto/pq -Ic/crypto -Itests/unit
	$(BUILD)/mldsa_test

test-mldsa-negctl:
	@$(C26_ENV) bash tests/unit/run-mldsa-openssl.sh --controls

test-mldsa-openssl: test-mldsa-negctl
	@$(C26_ENV) bash tests/unit/run-mldsa-openssl.sh

# ---------------------------------------------------------------------------
# The aggregate, and the CI wiring
# ---------------------------------------------------------------------------
# crypto2026 is a convenience for a human ("did the whole batch survive my
# change to c/crypto?"). It is NOT what CI reads: tools/ci.sh asks
# audit_tests.py --suites=host, which derives host-vs-boot from the recipe and
# never consults an aggregate.
#
# IT IS DELIBERATELY NOT CALLED test-crypto2026, and that is not cosmetic.
# audit_tests.py counts every `test-*` target and reports any the suites do not
# reach; an aggregate whose own members are all on ci-host: would be listed as
# UNWIRED forever (measured -- it was, on the first audit run after this
# fragment landed) while putting it ON ci-host: would make CI run the whole
# batch twice. A convenience alias is not a gate, so it does not carry a gate's
# name. The individual gates below are what ci-host: takes; `ci-host:` accepts
# prerequisites from any fragment, which is the mechanism audit_tests.py's
# reachability check actually follows.
#
# EVERY -negctl IS ABSENT FROM THIS LIST ON PURPOSE. Each one is already a
# prerequisite of its positive target above. CLAUDE.md: naming a control on a
# ci-host: line instead of on its positive satisfies the stranded-control
# audit and still runs it never, which is worse because it looks fixed.
crypto2026: test-crc32c test-blake2b test-blake2b-openssl test-blake2s-openssl \
                 test-blake3 test-cshake test-cshake-openssl \
                 test-scrypt test-scrypt-openssl test-argon2 test-argon2-openssl \
                 test-xchacha test-xchacha-openssl \
                 test-aes-gcm-siv test-aes-gcm-siv-openssl \
                 test-x448 test-x448-openssl \
                 test-secp256k1 test-secp256k1-openssl \
                 test-mldsa test-mldsa-openssl
	@echo "crypto2026: 13 primitives, all gates and all controls green"

ci-host: test-crc32c test-blake2b test-blake2b-openssl \
         test-blake2s-openssl test-blake3 \
         test-cshake test-cshake-openssl \
         test-scrypt test-scrypt-openssl \
         test-argon2 test-argon2-openssl \
         test-xchacha test-xchacha-openssl \
         test-aes-gcm-siv test-aes-gcm-siv-openssl \
         test-x448 test-x448-openssl \
         test-secp256k1 test-secp256k1-openssl \
         test-mldsa test-mldsa-openssl
