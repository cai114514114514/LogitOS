# AEX_T_SIG -- the OPTIONAL Ed25519 signature record on a .aex, and the LOG
# BUT ALLOW policy around it. See c/kernel/exec/aex.c's comment above the CRC
# check for the decision; c/crypto/trust/aexsig.h for the scheme, the domain
# and why it must never be pkgsig.c's LPK_DOMAIN; tools/mkaex.py's
# aex_sig_record() for how a file gets signed; tools/aexsign.c for the host
# tool that does the actual Ed25519 math (the SAME C the kernel verifies
# with, never a second implementation).
#
# test-aex-sig is the host gate. It answers four questions and is built to
# WATCH each one fail if the mechanism regresses -- CLAUDE.md rule 5:
#   1. a real dev-root signature verifies                    -> AEX_SIG_OK
#   2. a payload tampered after signing, CRC repaired         -> AEX_SIG_INVALID
#   3. a real signature from a non-root key                   -> AEX_SIG_UNTRUSTED
#   4. a REAL .lpk signature replayed as an .aex one           -> AEX_SIG_INVALID
#      (the domain-separation control -- see tests/unit/aexsig_test.c)
# In every case aex_parse() still returns AEX_OK: this is observability, not
# a gate. See aex.c.

.PHONY: test-aex-sig

# --- the host signer ---------------------------------------------------------
$(BUILD)/aexsign: tools/aexsign.c c/crypto/trust/aexsig.c c/crypto/trust/pkgsig.c \
                  c/crypto/pubkey/ed25519.c c/crypto/hash/sha256.c c/crypto/hash/sha384.c \
                  c/crypto/trust/pkgroots.inc
	@mkdir -p $(BUILD)
	cc -O2 -Wall -Wextra -o $@ tools/aexsign.c c/crypto/trust/aexsig.c \
	   c/crypto/trust/pkgsig.c c/crypto/pubkey/ed25519.c c/crypto/hash/sha256.c \
	   c/crypto/hash/sha384.c -Ic/crypto -Ic/crypto/trust

# --- fixtures -----------------------------------------------------------------
# All four wrap the SAME input ELF ($(BUILD)/echo.elf, already built for
# /bin/echo) so the only thing that differs between them is the TLV region --
# never the program, which would confound "is this a signature question or an
# ELF question".
AEXSIG_DEV_SEED     := $(LPK_DEV_SEED)
AEXSIG_FOREIGN_SEED := $(LPK_FOREIGN_SEED)

$(BUILD)/aexsig_ok.aex: $(BUILD)/echo.elf tools/mkaex.py $(BUILD)/aexsign
	python3 tools/mkaex.py $(BUILD)/echo.elf $@ aexsigok - '*' 150 150 150 \
	    --sign-seed $(AEXSIG_DEV_SEED) --aexsign-bin $(BUILD)/aexsign

# Tampered AFTER signing, CRC repaired to match -- see aex_tamper.py's header
# for why a plain bit-flip would be refused at the (already mandatory) CRC
# gate and never reach the signature check at all.
$(BUILD)/aexsig_tampered.aex: $(BUILD)/aexsig_ok.aex tests/unit/aex_tamper.py
	python3 tests/unit/aex_tamper.py $(BUILD)/aexsig_ok.aex $@

# A REAL Ed25519 signature, from a key that is simply not one of the
# compiled-in roots -- see LPK_FOREIGN_SEED's own comment above for why this
# is the case that distinguishes "intact" from "trusted".
$(BUILD)/aexsig_foreign.aex: $(BUILD)/echo.elf tools/mkaex.py $(BUILD)/aexsign
	python3 tools/mkaex.py $(BUILD)/echo.elf $@ aexsigforeign - '*' 150 150 150 \
	    --sign-seed $(AEXSIG_FOREIGN_SEED) --aexsign-bin $(BUILD)/aexsign

# No --sign-seed at all -- still a VALID file; this is the common case for
# every .aex this tree builds today.
$(BUILD)/aexsig_unsigned.aex: $(BUILD)/echo.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/echo.elf $@ aexsigunsigned - '*' 150 150 150

AEXSIG_FIXTURES := $(BUILD)/aexsig_ok.aex $(BUILD)/aexsig_tampered.aex \
                    $(BUILD)/aexsig_foreign.aex $(BUILD)/aexsig_unsigned.aex

# --- the host test ------------------------------------------------------------
# Same shape as tests/exec.mk's EXEC_SRC: c/kernel/exec/{elf,aex}.c UNMODIFIED,
# plus the crypto aex.c now hard-depends on (see exec.mk's comment on why that
# is not weak). aex_parse() never touches the machine under elf.c (no ELF is
# actually loaded here, only the container and its signature), so
# tests/unit/exechost/space.c is not needed.
AEXSIG_SRC := c/kernel/exec/elf.c c/kernel/exec/aex.c c/drivers/block/crc32.c \
              c/crypto/trust/aexsig.c c/crypto/trust/pkgsig.c \
              c/crypto/pubkey/ed25519.c c/crypto/hash/sha256.c c/crypto/hash/sha384.c \
              tests/unit/aexsig_stub.c
AEXSIG_INC := -Itests/unit/exechost -Ic/kernel/exec -Ic/drivers/block -Ic/crypto \
              -Ic/crypto/trust -DLOGIT_HOSTTEST

$(BUILD)/aexsig_test: c/crypto/trust/pkgroots.inc
$(BUILD)/aexsig_test: tests/unit/aexsig_test.c $(AEXSIG_SRC) c/kernel/exec/elf.h \
                      c/kernel/exec/aex.h c/crypto/trust/aexsig.h c/crypto/trust/pkgsig.h
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(AEXSIG_INC) \
	   -o $@ tests/unit/aexsig_test.c $(AEXSIG_SRC)

test-aex-sig: $(BUILD)/aexsig_test $(AEXSIG_FIXTURES)
	@$(BUILD)/aexsig_test $(BUILD)/aexsig_ok.aex $(BUILD)/aexsig_tampered.aex \
	    $(BUILD)/aexsig_foreign.aex $(BUILD)/aexsig_unsigned.aex

ci-host: test-aex-sig
