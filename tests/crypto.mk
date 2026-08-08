# Signing, entropy and the trust model.
#
# A separate .mk rather than more lines in the root Makefile, and its own .PHONY
# declaration rather than an append to the big one at the top: several
# workstreams edit that line, and a separate declaration means a target list
# make is happy to see twice instead of a merge conflict.
#
#   test-ed25519           RFC 8032 vectors + openssl cross-check + 27 rejections
#   test-pwhash            RFC 7914 s11 PBKDF2 vectors + openssl + the record format
#   test-entropy           ON DEVICE: ring 3 reaches the kernel DRBG
#   test-entropy-control   the same, against the generator js_platform.c used to
#                          ship. MUST fail the FORK check (parent and child get
#                          byte-identical output). Not the cross-run check --
#                          that one the old generator passes; see the harness.
#   test-pkg               the signed-package format, host side
#   test-pkg-os            ON DEVICE: /bin/pkgverify accepts a real signed
#                          package and refuses five kinds of bad one
#   test-pkg-control       a verifier with the signature check removed. MUST
#                          accept a tampered package.
#   test-ocsp              stapled OCSP responses, host side, against responses
#                          produced by a real `openssl ocsp` responder
#   test-ocsp-control      the TLS wiring, against a client that ignores the
#                          staple. MUST fail the revoked case.

.PHONY: test-ed25519 test-pwhash test-entropy test-entropy-control \
        test-pkg test-pkg-os test-pkg-control test-ocsp

# --- Ed25519 --------------------------------------------------------------
# ed25519_gen.sh needs openssl; the RFC vectors are compiled in and run either
# way, so a machine without openssl still gets the published-vector coverage.
test-ed25519: $(BUILD)
	@bash tests/unit/ed25519_gen.sh $(BUILD)/ed25519_vectors.txt 12 || \
	   echo "(openssl unavailable -- RFC 8032 vectors only)"
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/ed25519_test tests/unit/ed25519_test.c \
	  c/crypto/pubkey/ed25519.c c/crypto/hash/sha384.c -Ic/crypto
	$(BUILD)/ed25519_test $(BUILD)/ed25519_vectors.txt

# --- PBKDF2 + the password record -----------------------------------------
test-pwhash: $(BUILD)
	@bash tests/unit/pbkdf2_gen.sh $(BUILD)/pbkdf2_vectors.txt 14 || \
	   echo "(openssl unavailable -- RFC 7914 vectors only)"
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/pwhash_test tests/unit/pwhash_test.c \
	  c/crypto/kdf/pbkdf2.c c/crypto/hash/hmac_hkdf.c c/crypto/hash/sha256.c \
	  c/crypto/hash/sha384.c -Ic/crypto
	$(BUILD)/pwhash_test $(BUILD)/pbkdf2_vectors.txt

# --- the entropy syscall, on the real machine ------------------------------
# Not a host test on purpose: the claim is "RING 3 reaches the KERNEL DRBG",
# and every part of that sentence is about the boundary between two address
# spaces. A host build would link the DRBG straight into the test and prove
# nothing about the syscall.
test-entropy: $(ISO) $(DISK)
	@bash tests/boot/run-entropy-test.sh $(ISO) $(DISK)

# The control. /bin/entropy is rebuilt against the xorshift128+ generator that
# c/apps/browser/js_platform.c really shipped (same seeding: whole-second wall
# clock + addresses), the disk is repacked with it, and the harness INVERTS its
# verdict -- so this target passes only when the FORK assertion goes red, which
# it does exactly: ENT_CHILD == ENT_PARENT, byte for byte.
# The crippled .aex is removed afterwards so an ordinary `make` cannot pick it up.
test-entropy-control: $(ISO)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/entropy.crt0c.o
	$(CC) $(UCFLAGS) -DENTROPY_CONTROL_XORSHIFT -c $(CLIDIR)/entropy.c -o $(BUILD)/apps/entropy.cli.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $(BUILD)/entropy.elf \
	  $(BUILD)/apps/entropy.crt0c.o $(BUILD)/apps/entropy.cli.o
	python3 tools/mkaex.py $(BUILD)/entropy.elf $(BUILD)/entropy.aex entropy - '*' 150 150 150
	@$(MAKE) --no-print-directory $(DISK)
	@ENTROPY_BREAK=1 bash tests/boot/run-entropy-test.sh $(ISO) $(DISK); rc=$$?; \
	 rm -f $(BUILD)/entropy.aex $(BUILD)/entropy.elf $(BUILD)/apps/entropy.cli.o; \
	 exit $$rc

# --- the signed-package format --------------------------------------------
PKG_SRC := c/crypto/trust/pkgsig.c c/crypto/pubkey/ed25519.c c/crypto/hash/sha256.c \
           c/crypto/hash/sha384.c
PKG_INC := -Ic/crypto -Ic/crypto/trust

test-pkg: $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -o $(BUILD)/pkg_test tests/unit/pkg_test.c $(PKG_SRC) $(PKG_INC)
	$(BUILD)/pkg_test

# The control: the same test against a verifier built with the SIGNATURE CHECK
# REMOVED. Everything else -- the magic, the version, the length fields, the
# digest -- still checks out, which is the point: a package verifier that
# validates the container and forgets the signature passes every structural
# test in the suite. Must FAIL.
test-pkg-control: $(BUILD)
	$(CC) -O1 -g -w -DPKGSIG_BREAK_NO_SIGCHECK \
	  -o $(BUILD)/pkg_ctl tests/unit/pkg_test.c $(PKG_SRC) $(PKG_INC)
	@if $(BUILD)/pkg_ctl > $(BUILD)/pkg_ctl.log 2>&1; then \
	    echo "CONTROL FAILED: a verifier that never checks the signature passed --"; \
	    echo "                the package tests prove nothing"; exit 1; \
	 else \
	    echo "control ok: the signature-free verifier was detected:"; \
	    grep '^FAIL' $(BUILD)/pkg_ctl.log | head -5; \
	 fi

test-pkg-os: $(ISO) $(DISK)
	@bash tests/boot/run-pkg-test.sh $(ISO) $(DISK)

# --- OCSP stapling --------------------------------------------------------
# The responses are produced by a real `openssl ocsp` responder over a chain
# this script makes, so the DER we parse is DER openssl wrote, not DER we wrote.
# sha1.c is not listed: it lives under c/crypto/hash and CRYPTO_SRC finds it.
OCSP_SRC := c/net/tls/ocsp.c c/net/tls/x509.c $(CRYPTO_SRC) c/crypto/trust/roots.c
test-ocsp: $(BUILD)
	@bash tests/unit/ocsp_gen.sh $(BUILD)/ocsp
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -o $(BUILD)/ocsp_test tests/unit/ocsp_test.c $(OCSP_SRC) \
	  -Ic/net/tls $(CRYPTO_INC) -Ic/crypto/trust
	$(BUILD)/ocsp_test $(BUILD)/ocsp

# The control for the TLS WIRING (test-ocsp covers the response parser).
# LOGIT_OCSP_BREAK_IGNORE makes tls_check_staple return 0 without looking --
# which is exactly what this stack did before, and what a client that sends
# status_request and never reads the answer does. Every other case in the
# interop suite stays green under it, including the "good staple" one; only the
# REVOKED case can tell the difference, and it must go red.
test-ocsp-control: $(BUILD)
	@TLS_INTEROP_BREAK=LOGIT_OCSP_BREAK_IGNORE TLS_INTEROP_ONLY=ocsp 	  bash tests/unit/run-tls-interop.sh
