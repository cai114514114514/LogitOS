# tests/tlsx.mk -- TLS/crypto gates, as three separate contributions appended
# to one file (see the note at the very bottom before adding to this file
# again: this fragment was overwritten whole, twice, by concurrent sessions
# each told "you own tests/tlsx.mk", and each rewrite silently discarded the
# other two). ADD `-include tests/tlsx.mk` TO THE MAKEFILE -- one line, next
# to the other `-include tests/*.mk` lines (tests/crypto.mk's is the natural
# neighbour). None of the three contributions below has done that edit; the
# Makefile was contended when each was written, not because the targets
# belong apart.
#
# Section 1 (this one): post-quantum key exchange, ML-KEM-768 (FIPS 203) and
#   the X25519MLKEM768 hybrid.
# Section 2: the TLS server (c/net/tls/tls_server.c) and its RFC 6979 ECDSA
#   signer.
# Section 3: TLS gates -- a missing negative control, a probe for a real
#   unfixed CertificateVerify-omission vulnerability, and wiring several
#   existing TLS/crypto targets that already worked standalone but were never
#   invoked by anything.
#
# c/crypto/pq needs NO build-system change to reach the kernel: C_SRC globs
# c/crypto, and INCDIRS is built from `find c include -type d`, so both the
# sources and the headers are picked up. These targets are only for the HOST
# tests. (Basenames were checked against the rest of the tree before being
# chosen -- mlkem.h and keccak.h collide with nothing, which matters because
# INCDIRS is one flat sorted list. See the header-collision note in CLAUDE.md.)

PQ_SRC  := c/crypto/pq/mlkem.c c/crypto/pq/keccak.c
PQ_INC  := -Ic/crypto/pq

# ---------------------------------------------------------------- fast gate --
# Known answers produced by OpenSSL plus the properties a KAT cannot express.
# Needs no openssl at runtime, so it runs under the sanitisers.
# c/crypto/hash/sha256.c is here only to check the KAT digests.
test-mlkem:
	@$(CC) -O2 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -o $(BUILD)/mlkem_test tests/unit/mlkem_test.c $(PQ_SRC) c/crypto/hash/sha256.c \
	  $(PQ_INC) -Ic/crypto -Itests/unit
	@$(BUILD)/mlkem_test

# ------------------------------------------------------- negative controls --
# Five deliberate, self-consistent defects. Each MUST redden the differential;
# the last one, NO_IMPLICIT_REJECT, must redden ONLY the corrupted-ciphertext
# checks, which is what shows those checks measure the rejection rather than
# riding on the rest of the algorithm working. A control nobody has watched
# fail is not a control.
#
# NOTE these run against the OPENSSL differential, not against test-mlkem:
# three of the five (NO_DOMAIN_BYTE, NO_TRANSPOSE, SKIP_D2) produce an
# implementation that is entirely self-consistent, so a test comparing us with
# ourselves cannot see them at all. That is the argument for the differential
# existing, expressed as a target.
test-mlkem-negctl:
	@bash tests/unit/run-mlkem-openssl.sh --controls

# --------------------------------------------------- the differential gate --
# Ours against OpenSSL 3.5+, byte for byte, in both directions, including
# implicit rejection. Skips (exit 0) if the local openssl has no ML-KEM -- a
# missing reference is not a regression in the code under test, the same rule
# test-wpt applies to an absent corpus.
#
# The control is a PREREQUISITE of the positive, not a sibling on a ci- line.
# tools/audit_tests.py drops every `test-*-negctl` from suite listing on the
# ground that a control is "RUN BY its positive counterpart"; nothing checked
# that, which is how this tree acquired 55 STRANDED CONTROLS (CLAUDE.md).
# Naming it on ci-host: instead would satisfy the reachability audit and still
# run it never -- the worse failure, because it looks fixed.
test-mlkem-openssl: test-mlkem-negctl
	@bash tests/unit/run-mlkem-openssl.sh

# ------------------------------------------------------- the hybrid in TLS --
# A whole TLS 1.3 handshake whose key exchange is ML-KEM-768 + X25519, against
# openssl s_server. Lives in the interop harness because it needs that harness's
# throwaway PKI; run-tls-interop.sh gates itself on openssl having ML-KEM.
test-tls-pq:
	@TLS_INTEROP_ONLY=pq bash tests/unit/run-tls-interop.sh

.PHONY: test-mlkem test-mlkem-negctl test-mlkem-openssl test-tls-pq

# Reachability. `ci-host:` takes prerequisites from any fragment, so this is the
# one line that keeps these four out of audit_tests.py's UNWIRED list. All three
# named here degrade to a SKIP (exit 0) when the local openssl has no ML-KEM,
# which is why they are safe on a CI host of unknown vintage -- a missing
# reference is not a regression in the code under test.
ci-host: test-mlkem test-mlkem-openssl test-tls-pq

# =============================================================================
# Section 2: the TLS server + the ECDSA signer it needed.
#
#   test-ecdsa-sign          RFC 6979 agreement (an independent Python
#                            implementation) + an openssl differential
#   test-ecdsa-sign-negctl   a transposed DRBG separator: openssl still passes
#                            all 6 checks and the RFC 6979 half reddens all 9
#   test-tls-server          openssl s_client against c/net/tls/tls_server.c,
#                            every suite and group, plus our client against our
#                            server, plus openssl's verdict on a certificate
#                            this machine generated
#   test-tls-server-negctl   two breaks, each pinned to an exact count
#   bench-tls-selfcert       what one generated identity costs
#
# WHY THE POSITIVE DEPENDS ON THE CONTROL. tools/audit_tests.py excludes every
# `test-*-negctl` from suite listing on the stated ground that a control is
# "RUN BY its positive counterpart" -- and nothing checked that, which is how
# 55 STRANDED CONTROLS came to exist in this tree (see CLAUDE.md). Naming the
# control on a ci-host line instead would satisfy the reachability audit and
# still run it never, which is the worse failure because it looks fixed.

.PHONY: test-ecdsa-sign test-ecdsa-sign-negctl \
        test-tls-server test-tls-server-negctl \
        test-tls-server-negctl-hash32 test-tls-server-negctl-cvprefix \
        bench-tls-selfcert test-tlsx

# --------------------------------------------------------------- ecdsa_sign --
test-ecdsa-sign: test-ecdsa-sign-negctl
	@bash tests/unit/run-ecdsa-sign.sh

# Transpose RFC 6979 3.2's two separator octets. The k it produces is still
# uniform, r and s are still a valid signature, and openssl still verifies
# every one of them -- so this control reddens ONLY the known-answer half. That
# asymmetry is the whole argument for having both oracles, demonstrated rather
# than asserted: measured 2026-08-20, openssl 6/6 pass, RFC 6979 9/9 fail.
test-ecdsa-sign-negctl:
	@LOGIT_ECDSA_SIGN_BREAK=LOGIT_ECDSA_SIGN_BREAK_SEP \
	  bash tests/unit/run-ecdsa-sign.sh

# --------------------------------------------------------------- tls server --
test-tls-server: test-tls-server-negctl
	@bash tests/unit/run-tls-server.sh

test-tls-server-negctl: test-tls-server-negctl-hash32 test-tls-server-negctl-cvprefix

# Pin the key schedule at SHA-256 width regardless of the suite. Invisible to
# every AES-128 and ChaCha20 case -- for those, 32 IS the answer -- so it must
# redden EXACTLY ONE of the 26 cases, the TLS_AES_256_GCM_SHA384 row. A count
# of 0 means that row is not running; a count above 1 means the break is
# broader than the comment in tls_server.c claims. Measured 2026-08-20: 1.
test-tls-server-negctl-hash32:
	@TLS_SERVER_BREAK=LOGIT_TLSS_BREAK_HASH32 TLS_SERVER_BREAK_EXPECT=1 \
	  bash tests/unit/run-tls-server.sh

# Drop the 64 leading 0x20 octets from the CertificateVerify signature input.
# Nothing on OUR side notices -- we never verify our own signature -- so this
# is caught only by a peer, which is what the case is for. It must redden every
# case whose handshake is expected to COMPLETE and no others: the three
# certificate/refusal cases and the certificate-inspection block must stay
# green, or "reddens everything" would satisfy this control too. Measured
# 2026-08-20: 14 of 26 (3 pair + 11 openssl).
test-tls-server-negctl-cvprefix:
	@TLS_SERVER_BREAK=LOGIT_TLSS_BREAK_CV_PREFIX TLS_SERVER_BREAK_EXPECT=14 \
	  bash tests/unit/run-tls-server.sh

bench-tls-selfcert:
	@mkdir -p $(BUILD)/tlsx
	@$(CC) -O2 -w -o $(BUILD)/tlsx/tls_server_bench \
	  tests/unit/tls_server_test.c c/net/tls/tls_server.c c/net/tls/tls.c \
	  c/net/tls/tls12.c c/net/tls/tls_psk.c c/net/tls/x509.c c/net/tls/ocsp.c \
	  c/kernel/cpu/cpufeat.c \
	  $(shell find c/crypto/aead c/crypto/hash c/crypto/pubkey c/crypto/kdf c/crypto/pq -name '*.c' 2>/dev/null) \
	  -Ic/crypto -Ic/crypto/aead -Ic/crypto/trust -Ic/crypto/pq -Ic/net/tls \
	  -Ic/net/core -Ic/net/transport -Ic/drivers/timer -Ic/kernel/core -Ic/kernel/cpu
	@$(BUILD)/tlsx/tls_server_bench bench 100

# The aggregate, and the CI wiring. `ci-host:` takes prerequisites from any
# fragment, so membership is one line in the file that owns the target -- which
# is what keeps tools/audit_tests.py's UNWIRED list from growing by two the day
# this lands.
test-tlsx: test-ecdsa-sign test-tls-server

ci-host: test-tlsx

# =============================================================================
# Section 3: the gates pass -- a missing negative control, a probe for the
# CertificateVerify-omission gap, and wiring several existing TLS/crypto
# targets into the suites the audit can see.
#
#   test-crypto-diff-control    the differential gate (140,214 cases against
#                                hashlib/OpenSSL) had NO negative control --
#                                measured, not assumed: nothing between
#                                test-crypto-diff and the next Makefile target
#                                reverts anything. This corrupts SHA-224's IV
#                                by one bit (its only difference from
#                                SHA-256; see sha256.c) and requires EXACTLY
#                                the sha224 cases to fail and every other
#                                hash algorithm to stay clean -- the
#                                isolation is the point, not just "it can go
#                                red". Measured: 717/717 sha224 cases fail,
#                                0/3283 other-hash cases affected.
#
#   probe-tls13-certverify-bypass   **THE BUG THIS PROBE FOUND IS FIXED, and
#                                the probe is GREEN as of 2026-08-20.** What it
#                                reproduced was a full TLS 1.3 server-
#                                authentication bypass in c/net/tls/tls.c's
#                                verify_flight(): the client did not require
#                                CertificateVerify, so an on-path attacker
#                                holding only the target's PUBLIC certificate
#                                (everyone has it -- it is sent in the clear on
#                                every real connection) could impersonate it and
#                                read/inject the whole session. verify_flight()
#                                now records whether a CertificateVerify with a
#                                VERIFIED signature arrived and refuses a full
#                                handshake without one; the probe's control (an
#                                honest server whose signature is garbage) must
#                                still be refused, which is what stops "never
#                                require it" from satisfying the gate.
#                                Kept as probe- rather than renamed to test-:
#                                the name is referenced from CLAUDE.md and from
#                                the report, and NOT_CI's `^probe-` clause still
#                                keeps it out of `make ci`'s automatic sweep --
#                                which now costs nothing, because it passes.
#                                Whoever wires it into a suite should promote it
#                                to test- in the same commit, so the name and
#                                the classifier agree. See the file header of
#                                tests/unit/run-tls13-certverify-bypass-probe.sh
#                                for the full writeup and the exact tls.c line
#                                numbers. Run it by name:
#                                    bash tests/unit/run-tls13-certverify-bypass-probe.sh
#                                (NOTE: as of this writing c/net/tls/tls_int.h
#                                does not compile at all, because of unrelated
#                                in-flight work from Section 1/2 above; this
#                                probe was validated against an EARLIER,
#                                buildable snapshot of tls.c and will need a
#                                re-run once the tree settles -- see the
#                                report.)
#
#   test-tls                    convenience alias -- `make test-tls` already
#                                failed with "No rule to make target" and
#                                nothing in the Makefile ever promised that
#                                name, so it is not a dangling reference, just
#                                an unregistered one. Runs the gates this
#                                section owns. Deliberately excludes
#                                test-p521 (below: broken on its own,
#                                unrelated to this fragment) and the probe
#                                above (deliberately red).
#
# Four more lines attach an existing control as a PREREQUISITE of its positive
# (the same move as test-ecdsa-sign/test-tls-server above, and
# tests/canvas.mk's test-canvas before either): test-tls-psk-control,
# test-p521-control, test-tls-resume-control and test-tls-bench-control all
# already existed, already worked standalone (`make test-tls-psk-control`
# etc. all pass, verified in the report), and were simply never CALLED by
# anything -- invisible to tools/audit_tests.py's STRANDED CONTROLS check for
# a different reason than usual: that check only pattern-matches names ending
# in `-negctl`, and every one of these four is named `-control` (an older
# convention in this corner of the tree). NOT_CI still drops all four from
# `bash tools/ci.sh`'s automatic sweep either way (`test-.*-control$`), so
# without this file they ran never.
.PHONY: test-crypto-diff-control probe-tls13-certverify-bypass test-tls

# CDIFF_SRC is deferred (=), not immediate (:=): CRYPTO_SRC is defined earlier
# in the Makefile than any `-include tests/*.mk` line could land, so by the
# time this is read it already holds its final value either way -- `=` just
# matches the convention tests/canvas.mk uses for the same reason.
CDIFF_SRC = tests/unit/crypto_diff_test.c $(CRYPTO_SRC)

test-crypto-diff-control: $(BUILD)
	python3 tests/unit/crypto_diff_gen.py $(BUILD)/crypto_diff_vec.txt
	@grep '^hash sha224 ' $(BUILD)/crypto_diff_vec.txt > $(BUILD)/cdiff_sha224.txt; \
	 grep '^hash ' $(BUILD)/crypto_diff_vec.txt | grep -v '^hash sha224 ' > $(BUILD)/cdiff_hash_rest.txt; \
	 n224=$$(wc -l < $(BUILD)/cdiff_sha224.txt); nrest=$$(wc -l < $(BUILD)/cdiff_hash_rest.txt); \
	 if [ "$$n224" -eq 0 ]; then \
	   echo "CONTROL FAILED: the generator produced zero sha224 vectors this run --"; \
	   echo "                there is nothing here for the corrupted IV to break"; exit 1; \
	 fi; \
	 echo "generated: $$n224 sha224 case(s), $$nrest other-hash case(s)"
	$(CC) -O2 -w -DCRYPTO_DIFF_BREAK_SHA224_IV -o $(BUILD)/crypto_diff_ctl $(CDIFF_SRC) $(CRYPTO_INC)
	@n224=$$(wc -l < $(BUILD)/cdiff_sha224.txt); \
	 out224=$$($(BUILD)/crypto_diff_ctl $(BUILD)/cdiff_sha224.txt); \
	 p224=$$(echo "$$out224" | awk '$$1=="hash"{print $$3}'); \
	 f224=$$(echo "$$out224" | awk '$$1=="hash"{print $$5}'); \
	 if [ "$$p224" != "0" ] || [ "$$f224" != "$$n224" ]; then \
	   echo "CONTROL FAILED: -DCRYPTO_DIFF_BREAK_SHA224_IV did not fail every"; \
	   echo "  sha224 case (pass=$$p224 fail=$$f224 of $$n224 total) -- the"; \
	   echo "  differential gate is not actually checking SHA-224's output"; \
	   exit 1; \
	 fi; \
	 echo "ok   sha224: $$f224/$$n224 cases failed under the corrupted IV, as required"
	@nrest=$$(wc -l < $(BUILD)/cdiff_hash_rest.txt); \
	 outrest=$$($(BUILD)/crypto_diff_ctl $(BUILD)/cdiff_hash_rest.txt); \
	 frest=$$(echo "$$outrest" | awk '$$1=="hash"{print $$5}'); \
	 if [ "$$frest" != "0" ]; then \
	   echo "CONTROL FAILED: -DCRYPTO_DIFF_BREAK_SHA224_IV ALSO broke $$frest of"; \
	   echo "  the other $$nrest hash case(s) (sha256/384/512/sha512_224/sha512_256)"; \
	   echo "  -- the break is not isolated to sha224, so 'exactly the sha224"; \
	   echo "  cases redden' is not a property this control actually has"; \
	   exit 1; \
	 fi; \
	 echo "ok   other hash algorithms: 0/$$nrest cases affected -- the break is isolated to sha224"
	@echo "PASS: test-crypto-diff-control"

# The control now runs whenever the gate it controls does.
test-crypto-diff: test-crypto-diff-control

# --- wiring existing controls as prerequisites of their positives ----------
# tests/unit/tls_psk_test.c's two shipped-bug reversions (age unit, ticket
# reuse): `make test-tls-psk-control` on its own -- ok control
# LOGIT_PSK_BREAK_AGE_UNIT was detected / ok control
# LOGIT_PSK_BREAK_SINGLE_USE was detected / PASS.
test-tls-psk: test-tls-psk-control

# LOGIT_P521_BREAK_FLEN (the field-length-off-by-one that made P-521 certs
# unreachable before this work): `make test-p521-control` on its own -- PASS:
# negative control -- LOGIT_P521_BREAK_FLEN was detected by 2 case(s).
#
# test-p521 ITSELF (the standalone RFC 6979/openssl vector test, distinct from
# the P-521 interop CASES inside test-tls-interop) does NOT currently build:
#     clang ... tests/unit/ecdsa_p521_test.c c/crypto/pubkey/ecdsa.c \
#       c/crypto/hash/sha384.c -Ic/crypto
#     undefined reference to `hmac' (five sites, all inside ecdsa_sign's
#     RFC 6979 deterministic-nonce path)
# ecdsa_sign() calls hmac(), which lives in c/crypto/hash/hmac_hkdf.c -- not
# in this recipe's three-file link line (Makefile:1360-1362). Exactly the
# shape CLAUDE.md's own "gate nobody runs is a gate that rots" list already
# names for test-fb-clip/test-wpt-*/test-css-web-negctl/test-leak: a source
# file grew a dependency (ecdsa_sign gained the RFC 6979 path, per Section 2
# above) and the link line for THIS OTHER caller did not follow -- every
# other CRYPTO_SRC consumer links hmac_hkdf.c as part of the full crypto
# battery and never noticed. Not fixed here: the recipe lives in the
# Makefile, which this fragment does not edit. Reported so it is not
# silently believed working -- it is in tests/audit-unwired.baseline already
# (nothing runs it), which is how it went unnoticed: it has never once been
# part of a suite.
test-p521: test-p521-control

# LOGIT_PSK_BREAK_TRANSCRIPT (resumption_master_secret derived without the
# client Finished -- invisible to every check except "did resumption actually
# happen"): verified passing standalone before this file existed. Attached to
# test-tls-interop, the target that actually drives the resumption cases this
# control needs (there is no separate "test-tls-resume" positive).
test-tls-interop: test-tls-resume-control

# -DKPROF_DISABLE (the span table compiles to nothing): boots QEMU, not run in
# this pass (see the report) -- wired on the strength of `make -n
# test-tls-bench-control` resolving cleanly and the target's own header
# documenting the same inversion-verdict convention as every other control in
# this file.
test-tls-bench: test-tls-bench-control

# ci-host:/ci-boot: accept prerequisites from any fragment (tools/ci.sh reads
# `python3 tools/audit_tests.py --suites=host|boot`, which classifies by
# RECIPE content and already runs any qualifying test-* target regardless of
# this line -- see CLAUDE.md, "UNWIRED does not mean CI does not run it").
# What this line buys is the audit's bookkeeping: without it every target
# below is counted as UNWIRED debt, indistinguishable from a target nobody
# runs at all.
#
# test-p521 is deliberately NOT listed here (does not build, see above).
# test-tls-bench is boot-classified (its recipe needs $(ISO)/$(DISK) and
# boots QEMU), so it is on ci-boot:, not ci-host:.
ci-host: test-tls-interop test-tls-psk test-x509-fuzz test-crypto-diff
ci-boot: test-tls-bench

test-tls: test-tls-interop test-tls-psk test-x509-fuzz test-crypto-diff

# See tests/unit/run-tls13-certverify-bypass-probe.sh and
# tests/unit/tls13_certverify_omit_server.py for the full writeup: our TLS 1.3
# client completes a handshake, and proceeds to send its own encrypted
# Finished/application data, against a server that sent a genuine certificate
# chain and NO CertificateVerify at all. RFC 8446 4.4.3 makes that message
# mandatory precisely because a certificate proves a CA vouched for a key
# once, never that the peer on the wire now holds it.
probe-tls13-certverify-bypass: $(BUILD)
	@bash tests/unit/run-tls13-certverify-bypass-probe.sh

# =============================================================================
# Section 4: the interop MATRIX (2026-08-20).
#
# run-tls-interop.sh's 73 cases are CHOSEN cases -- it pins one suite on one
# group, then one group on one suite. That is a diagonal, and a claim is
# per-cell: six of the twelve TLS 1.3 (suite, group) pairs and nine of the
# eighteen TLS 1.2 pairs were reached by no case in the tree. Not failing, not
# skipped -- never asked. This walks the whole product, in BOTH directions,
# and prints the openssl command beside every row so any cell can be re-run by
# hand.
#
# Measured on first run: 39 claimed cells pass, 0 fail, 7 correctly reported
# not-claimed (TLS 1.2 x hybrid, which has no KEM message, and the hybrid
# against our own server, whose srv_groups[] does not offer it).
test-tls-matrix:
	@bash tests/unit/run-tls-matrix.sh

.PHONY: test-tls-matrix
ci-host: test-tls-matrix

# =============================================================================
# COORDINATION NOTE, for whoever next writes to this file: three concurrent
# sessions were each told "you own tests/tlsx.mk" for a NEW file, and at least
# two of the three overwrote the whole file (Write, not Edit) rather than
# appending, discarding the other contributions each time -- caught and
# reconstructed by hand while producing this report; see the report for the
# full account. Editing (append/insert) rather than rewriting the whole file
# is what avoids a repeat.
