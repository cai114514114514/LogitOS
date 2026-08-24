# --- SSH-2: host protocol gates + a device boot test ------------------------
#
# In its own .mk rather than in the Makefile for the reason tests/mem.mk gives:
# several lines share this tree, and a whole-file Makefile overwrite from a
# concurrent line has silently deleted other people's targets before. A
# separate file cannot be clobbered that way.
#
# THIS FRAGMENT IS NOT YET REACHABLE FROM `make`. Two lines are needed in the
# main Makefile and neither is added here, because the Makefile itself was
# out of scope for the session that wrote this (see the task's final report
# for the exact lines and why):
#   1. `-include tests/ssh.mk`                         (beside the other -includes)
#   2. sshd added to the coreutils APPS list, the way ping/syslogd were
# Until both land, every target below still works when invoked directly with
# the commands the report gives (`gcc ... -o build/ssh_wire_test ...`, or
# `python3 tests/unit/ssh_kex_gen.py ...`) -- this file records the SHAPE of
# the eventual `make` targets and is exercised that way, not through `make`.
#
#   make test-ssh-wire     RFC 4251 primitives (mpint/string/namelist/negotiate)
#   make test-ssh-packet   binary packet protocol, plaintext + aes128-ctr/
#                           hmac-sha2-256, negative controls (flipped MAC byte,
#                           flipped ciphertext byte, truncated wire, an
#                           oversized packet_length)
#   make test-ssh-kex      curve25519-sha256 kex + the RFC 4253 7.2 KDF against
#                           an INDEPENDENT oracle (ssh_kex_gen.py, Python's
#                           `cryptography`, no code shared with ssh_kex.c) --
#                           the negative control the task brief asked for:
#                           flip one byte of the session id into the KDF and
#                           every derived key must differ, and a packet framed
#                           under the WRONG derived keys must fail with
#                           SSH_PKT_E_MAC specifically, not merely "some error"
#   make test-ssh          all three host gates
#   make test-ssh-os       boot LogitOS, enroll a throwaway account, install a
#                           throwaway ed25519 pubkey (minted fresh under
#                           `mktemp -d`, never committed), start /bin/sshd,
#                           and require a REAL OpenSSH client to complete
#                           BOTH password and publickey auth to a real shell,
#                           plus the negative control: a wrong password must
#                           be refused, not accepted. See
#                           tests/boot/run-ssh-test.sh's own header for what
#                           it drives around c/apps/coreutils/sh.c's `-c`
#                           gap (out of this line's ownership) to get a real
#                           command's output back over the channel.

SSH_DIR      := c/net/ssh
SSH_SRC      := $(wildcard $(SSH_DIR)/*.c)

# The exact crypto objects c/net/ssh needs, expressed as source files rather
# than pulled from $(C_SRC)/$(RING3_NET): those lists carry the KERNEL's and
# the browser's include-path assumptions (c/apps/libc/include, notably, whose
# features.h shadows glibc's own -- see $(HOST_INCDIRS)'s own comment), and a
# host gcc build of a host test binary must NOT see that directory at all, or
# <stdint.h>'s __GLIBC_USE macro breaks for every TU that pulls it in
# (measured while standing this file up: every host .c here failed with
# "missing binary operator before token '('" until c/apps/libc/include was
# off the -I list). $(HOST_INCDIRS) already exists for exactly this reason;
# use it, not the full $(INCDIRS) sweep -- and even $(HOST_INCDIRS) is too
# wide for a build that links -lpthread against the HOST's own <pthread.h>
# (c/kernel/sched/sched.h shadows the system <sched.h>, which <pthread.h>
# needs for cpu_set_t), so the two host-linked-crypto helper binaries below
# use an explicit, minimal -I list instead.
SSH_CRYPTO_SRC := c/crypto/hash/sha256.c c/crypto/hash/sha384.c c/crypto/hash/hmac_hkdf.c \
                  c/crypto/aead/aes_modes.c c/crypto/aead/aes_dispatch.c \
                  c/crypto/aead/aesgcm.c c/crypto/aead/aes_ni.c c/kernel/cpu/cpufeat.c \
                  c/crypto/pubkey/ed25519.c c/crypto/pubkey/x25519.c c/crypto/kdf/pbkdf2.c

$(BUILD)/ssh_wire_test: tests/unit/ssh_wire_test.c $(SSH_DIR)/ssh_wire.c $(SSH_DIR)/ssh_wire.h
	@mkdir -p $(BUILD)
	$(CC) -Wall -Wextra -O2 -I$(SSH_DIR) -o $@ tests/unit/ssh_wire_test.c $(SSH_DIR)/ssh_wire.c

test-ssh-wire: $(BUILD)/ssh_wire_test
	@$(BUILD)/ssh_wire_test

SSH_HOST_MININC := -I$(SSH_DIR) -Ic/crypto -Ic/crypto/pubkey -Ic/crypto/hash -Ic/crypto/aead -Ic/kernel/cpu -Ic/crypto/kdf -Ic/apps/coreutils

$(BUILD)/ssh_packet_test: tests/unit/ssh_packet_test.c $(SSH_DIR)/ssh_wire.c $(SSH_DIR)/ssh_packet.c $(SSH_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -Wall -Wextra -O2 -w $(SSH_HOST_MININC) -o $@ \
	    tests/unit/ssh_packet_test.c $(SSH_DIR)/ssh_wire.c $(SSH_DIR)/ssh_packet.c $(SSH_CRYPTO_SRC)

test-ssh-packet: $(BUILD)/ssh_packet_test
	@$(BUILD)/ssh_packet_test

$(BUILD)/ssh_kex_vectors.txt: tests/unit/ssh_kex_gen.py
	@mkdir -p $(BUILD)
	python3 tests/unit/ssh_kex_gen.py $@

$(BUILD)/ssh_kex_test: tests/unit/ssh_kex_test.c $(SSH_SRC) $(SSH_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -Wall -Wextra -O2 -w $(SSH_HOST_MININC) -o $@ \
	    tests/unit/ssh_kex_test.c $(SSH_DIR)/ssh_wire.c $(SSH_DIR)/ssh_packet.c \
	    $(SSH_DIR)/ssh_kex.c $(SSH_DIR)/ssh_hostkey.c $(SSH_DIR)/base64.c $(SSH_CRYPTO_SRC)

test-ssh-kex: $(BUILD)/ssh_kex_test $(BUILD)/ssh_kex_vectors.txt
	@$(BUILD)/ssh_kex_test $(BUILD)/ssh_kex_vectors.txt

test-ssh: test-ssh-wire test-ssh-packet test-ssh-kex
	@echo "SSH-HOST-OK: wire + packet + kex, incl. the session-id KDF negative control"

# --- the ring-3 program ------------------------------------------------------
# Mirrors the pkgverify/login custom-link shape (a CLI coreutil, crt0_cli.asm,
# not the GUI APP_RULE), by hand rather than by macro: sshd is not in APPS
# (see this file's header), so it has no place to hang an APP_RULE call from.
# UCFLAGS/INCDIRS/ASM/LD are the Makefile's own variables, unchanged, so this
# rule tracks them if they change rather than pinning a stale copy of them.
# NOT $(SSH_CRYPTO_SRC): that list's aes_ni.c/cpufeat.c are for the HOST test
# binaries above, which need a REAL aes_backend_ni() to link against (they
# are plain host programs with no ring-3 stub of their own). sshd.c carries
# its own aes_backend_ni() stub (see its own comment beside the definition,
# and .sshwork/aes_ni_stub.c which records the identical host-side reason) --
# ring 3 has no business doing its own CPUID feature probing, so linking the
# real aes_ni.c here would be a silent DUPLICATE SYMBOL, which is exactly
# what happened the first time this rule was written (ld.lld: "duplicate
# symbol: aes_backend_ni") and is why this list is spelled out rather than
# reusing $(SSH_CRYPTO_SRC).
SSHD_CRYPTO_SRC := c/crypto/hash/sha256.c c/crypto/hash/sha384.c c/crypto/hash/hmac_hkdf.c \
                    c/crypto/pubkey/ed25519.c c/crypto/pubkey/x25519.c \
                    c/crypto/aead/aes_modes.c c/crypto/aead/aes_dispatch.c c/crypto/aead/aesgcm.c \
                    c/crypto/kdf/pbkdf2.c
SSHD_SRC := c/apps/coreutils/sshd.c $(SSH_SRC) $(SSHD_CRYPTO_SRC)
SSHD_OBJS := $(patsubst %.c,$(BUILD)/sshdobj/%.o,$(SSHD_SRC))

$(BUILD)/sshdobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Ic/apps/libc/include/uonly $(INCDIRS) -c $< -o $@

$(BUILD)/sshdobj/sshd_thread.o: c/apps/coreutils/sshd_thread.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/apps/sshd.crt0c.o: c/apps/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $< -o $@

$(BUILD)/sshd.elf: $(SSHD_OBJS) $(BUILD)/sshdobj/sshd_thread.o $(BUILD)/apps/sshd.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/sshd.crt0c.o $(SSHD_OBJS) $(BUILD)/sshdobj/sshd_thread.o

$(BUILD)/sshd.aex: $(BUILD)/sshd.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/sshd.elf $@ sshd - '*' 150 150 150

# --- the device gate ---------------------------------------------------------
# See this file's header for why this is a SEPARATE disk image rather than
# $(DISK): sshd is not on $(DISK) until the APPS-list line lands.
$(BUILD)/disk-ssh.img: $(DISK) $(BUILD)/sshd.aex tests/boot/mk_ssh_disk.py
	python3 tests/boot/mk_ssh_disk.py $(CURDIR) $@ $(BUILD)/sshd.aex

test-ssh-os: $(ISO) $(BUILD)/disk-ssh.img
	@bash tests/boot/run-ssh-test.sh $(ISO) $(BUILD)/disk-ssh.img
