# --- SSH attack battery: adversarial host gates + the in-guest hostile client
#
# Own fragment for the reason tests/mem.mk and tests/ssh.mk both give: several
# lines share this tree, and a whole-Makefile rewrite from a concurrent line
# has silently deleted other people's targets before. A separate file cannot
# be clobbered that way.
#
#   make test-ssh-attack-authkeys   authorized_keys garbage + the truncation
#                                  edges of the auth parsers (password over-
#                                  long = REFUSED not prefix-matched; user
#                                  over-long = REFUSED not collapsed; CRLF
#                                  authorized_keys still matches)
#   make test-ssh-attack-kexvec    the low-order-point attack, complete: the
#                                  4 canonical small-order u values COMPUTED
#                                  by an Edwards-subgroup generator in
#                                  ssh_attack_kex_gen.py (not quoted from a
#                                  table), plus every non-canonical spelling,
#                                  plus ordinary points in non-canonical
#                                  spellings that must still be ACCEPTED with
#                                  the RFC 7748 masked secret
#   make test-ssh-attack-fuzz      deterministic structure-aware mutations over
#                                  every c/net/ssh parser (SEED=/SCALE= to
#                                  explore; ASan+UBSan)
#   make test-ssh-attack-fuzz-negctl
#                                  the sabotage build (-DSSH_FUZZ_SABOTAGE
#                                  removes ssh_r_string's over-run refusal,
#                                  the single most load-bearing bound on the
#                                  pre-auth surface) MUST die to
#                                  'ERROR: AddressSanitizer' -- it is a
#                                  PREREQUISITE of the positive fuzz gate,
#                                  because a clean run proves nothing about a
#                                  sanitizer that was never armed
#   make test-ssh-attack-host      all four above, controls first
#   make test-ssh-attack-os        boots the OS, enrolls a throwaway account,
#                                  and runs tests/boot/ssh_attack_client.py --
#                                  a hostile raw-socket SSH-2 client sharing
#                                  no code with the server -- through ~20
#                                  attacks, each followed by a clean control
#                                  login. Prerequisite: test-ssh-os, so the
#                                  honest-client gate runs first and the
#                                  battery cannot pass against a server that
#                                  cannot even serve an honest client
#   make test-ssh-tamper-os        the on-path characterization: a tamper
#                                  proxy between a REAL OpenSSH client and
#                                  the guest sshd. kexreply/first-encrypted
#                                  byte flips must KILL the login;
#                                  inject-ignore PINS A SUCCESSFUL LOGIN --
#                                  that tolerance is the Terrapin exposure
#                                  c/net/ssh/ssh.h accepted on purpose by
#                                  refusing to half-offer strict kex; the
#                                  probe documents the cost, it does not
#                                  "fix" it
#
# The `ci-host:`/`ci-boot:` lines at the bottom also wire the ORIGINAL five
# ssh.mk gates, which audit_tests.py had carried as UNWIRED (NEW) since they
# landed -- per this tree's own rule, a gate nobody runs is a gate that rots.

SSH_ATK_DIR := c/net/ssh

# The host crypto set ssh.mk already argues for (see its own comment: the
# browser/libc include paths break host glibc builds). Identical list, so a
# file added to one needs adding to the other -- the subtraction-from-a-
# variable shape would be better but SSH_CRYPTO_SRC lives in another
# fragment's namespace and fragments must not reach into each other's
# variables without a stated reason.
SSH_ATK_CRYPTO_SRC := c/crypto/hash/sha256.c c/crypto/hash/sha384.c c/crypto/hash/hmac_hkdf.c \
                       c/crypto/aead/aes_modes.c c/crypto/aead/aes_dispatch.c \
                       c/crypto/aead/aesgcm.c c/crypto/aead/aes_ni.c c/kernel/cpu/cpufeat.c \
                       c/crypto/pubkey/ed25519.c c/crypto/pubkey/x25519.c c/crypto/kdf/pbkdf2.c

SSH_ATK_MININC := -I$(SSH_ATK_DIR) -Ic/crypto -Ic/crypto/pubkey -Ic/crypto/hash \
                  -Ic/crypto/aead $(KCPU_INC) -Ic/crypto/kdf -Ic/apps/coreutils

SSH_ATK_SRC := $(SSH_ATK_DIR)/ssh_wire.c $(SSH_ATK_DIR)/ssh_packet.c $(SSH_ATK_DIR)/ssh_kex.c \
               $(SSH_ATK_DIR)/ssh_auth.c $(SSH_ATK_DIR)/ssh_conn.c $(SSH_ATK_DIR)/ssh_hostkey.c \
               $(SSH_ATK_DIR)/base64.c

# LSan presence is a property of the linked runtime, not of uname -- probed
# once per $(BUILD) by tests/demux.mk's $(BUILD)/.asan-leaks and reused here
# for that file's own stated reason: two probes of one host property is two
# chances to disagree.
SSH_ATK_ASAN = -fsanitize=address,undefined
SSH_ATK_ASAN_ENV = ASAN_OPTIONS=detect_leaks=$$leaks:halt_on_error=1 \
                   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
SSH_ATK_LEAK_NOTE = if [ "$$leaks" = 0 ]; then \
	    echo "  [asan] LEAK DETECTION OFF -- this host's runtime has no LeakSanitizer"; \
	    echo "         (probe: $(BUILD)/.asan-leaks). Memory safety is still checked."; \
	fi

# --- authorized_keys + truncation edges -------------------------------------
$(BUILD)/ssh_attack_authkeys_test: tests/unit/ssh_attack_authkeys_test.c \
		$(SSH_ATK_DIR)/ssh_wire.c $(SSH_ATK_DIR)/ssh_auth.c $(SSH_ATK_DIR)/base64.c $(SSH_ATK_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g $(SSH_ATK_ASAN) -w $(SSH_ATK_MININC) -o $@ tests/unit/ssh_attack_authkeys_test.c \
	    $(SSH_ATK_DIR)/ssh_wire.c $(SSH_ATK_DIR)/ssh_auth.c $(SSH_ATK_DIR)/base64.c $(SSH_ATK_CRYPTO_SRC)

test-ssh-attack-authkeys: $(BUILD)/ssh_attack_authkeys_test $(BUILD)/.asan-leaks
	@leaks=`cat $(BUILD)/.asan-leaks`; $(SSH_ATK_LEAK_NOTE); \
	 $(SSH_ATK_ASAN_ENV) $(BUILD)/ssh_attack_authkeys_test

# --- hostile kex vectors ------------------------------------------------------
$(BUILD)/ssh_attack_kex_vectors.txt: tests/unit/ssh_attack_kex_gen.py
	@mkdir -p $(BUILD)
	python3 tests/unit/ssh_attack_kex_gen.py $@

$(BUILD)/ssh_attack_kex_test: tests/unit/ssh_attack_kex_test.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g $(SSH_ATK_ASAN) -w $(SSH_ATK_MININC) -o $@ \
	    tests/unit/ssh_attack_kex_test.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)

test-ssh-attack-kexvec: $(BUILD)/ssh_attack_kex_test $(BUILD)/ssh_attack_kex_vectors.txt $(BUILD)/.asan-leaks
	@leaks=`cat $(BUILD)/.asan-leaks`; $(SSH_ATK_LEAK_NOTE); \
	 $(SSH_ATK_ASAN_ENV) $(BUILD)/ssh_attack_kex_test $(BUILD)/ssh_attack_kex_vectors.txt

# --- the fuzzer and its sabotage control -------------------------------------
$(BUILD)/ssh_attack_fuzz: tests/unit/ssh_attack_fuzz.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g $(SSH_ATK_ASAN) -w $(SSH_ATK_MININC) -o $@ \
	    tests/unit/ssh_attack_fuzz.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)

$(BUILD)/ssh_attack_fuzz_neg: tests/unit/ssh_attack_fuzz.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -fsanitize=address -w -DSSH_FUZZ_SABOTAGE=1 $(SSH_ATK_MININC) -o $@ \
	    tests/unit/ssh_attack_fuzz.c $(SSH_ATK_SRC) $(SSH_ATK_CRYPTO_SRC)

# The control is a PREREQUISITE of the positive gate below, not a sibling on
# some ci- line -- naming it beside the positive satisfies the audit and runs
# it never, which is worse because it looks fixed (AGENTS.md's own words).
# The guard is the tightened one demux.mk bled for: 'ERROR: AddressSanitizer'
# is what a FINDING prints; the runtime's own option-refusal contains the bare
# word and once read as a caught bug.
test-ssh-attack-fuzz-negctl: $(BUILD)/ssh_attack_fuzz_neg $(BUILD)/.asan-leaks
	@leaks=`cat $(BUILD)/.asan-leaks`; $(SSH_ATK_LEAK_NOTE); \
	 if $(SSH_ATK_ASAN_ENV) $(BUILD)/ssh_attack_fuzz_neg \
	        >$(BUILD)/ssh_attack_fuzz_neg.log 2>&1; then \
	    echo "NEGCTL-FAIL: the injected ssh_r_string over-run did not trip ASan --"; \
	    echo "  the fuzzer's clean runs are proving nothing."; exit 1; \
	 elif grep -q 'is not supported on this platform' $(BUILD)/ssh_attack_fuzz_neg.log; then \
	    echo "NEGCTL-FAIL: the ASan RUNTIME refused its own options and died before main"; \
	    head -3 $(BUILD)/ssh_attack_fuzz_neg.log | sed 's/^/       /'; exit 1; \
	 elif grep -q 'ERROR: AddressSanitizer' $(BUILD)/ssh_attack_fuzz_neg.log; then \
	    echo "negctl: the injected string-length over-read is caught by AddressSanitizer"; \
	    grep -m1 'ERROR: AddressSanitizer' $(BUILD)/ssh_attack_fuzz_neg.log | sed 's/^/       /'; \
	 else \
	    echo "NEGCTL-FAIL: the sabotage build died without an ASan finding (what stopped it?)"; \
	    head -3 $(BUILD)/ssh_attack_fuzz_neg.log | sed 's/^/       /'; exit 1; \
	 fi

test-ssh-attack-fuzz: $(BUILD)/ssh_attack_fuzz $(BUILD)/.asan-leaks test-ssh-attack-fuzz-negctl
	@leaks=`cat $(BUILD)/.asan-leaks`; $(SSH_ATK_LEAK_NOTE); \
	 $(SSH_ATK_ASAN_ENV) SCALE=$${SCALE:-20000} SEED=$${SEED:-1} $(BUILD)/ssh_attack_fuzz

test-ssh-attack-host: test-ssh-attack-authkeys test-ssh-attack-kexvec test-ssh-attack-fuzz
	@echo "SSH-ATTACK-HOST-OK: authkeys + kex vectors + fuzz, sabotage control green first"

# --- in-guest batteries --------------------------------------------------------
# The battery scripts take the ISO and the sshd-bearing disk from the same
# variables test-ssh-os uses; SSH_ATTACK_PORT (battery) and SSH_TAMPER_PORT
# default off test-ssh-os's own SSH_TEST_PORT so concurrent boots do not
# collide with each other or with another agent's run-ssh-test.sh.
test-ssh-attack-os: test-ssh-os $(ISO) $(BUILD)/disk-ssh.img
	@SSH_ATTACK_PORT=$${SSH_ATTACK_PORT:-2301} \
	 bash tests/boot/run-ssh-attack-test.sh $(CURDIR)/$(ISO) $(CURDIR)/$(BUILD)/disk-ssh.img

test-ssh-tamper-os: $(ISO) $(BUILD)/disk-ssh.img
	@SSH_TAMPER_PORT=$${SSH_TAMPER_PORT:-2302} \
	 bash tests/boot/run-ssh-tamper-test.sh $(CURDIR)/$(ISO) $(CURDIR)/$(BUILD)/disk-ssh.img

# --- wiring (AGENTS.md rule 4: a gate nobody runs rots) -----------------------
# These five ssh.mk targets were UNWIRED (NEW) in tools/audit_tests.py since
# they landed; these two lines are the whole fix, and they live in THIS
# fragment rather than the shared Makefile for the same clobber-avoidance
# reason as everything above (canvas.mk's ci-host line is the precedent).
ci-host: test-ssh test-ssh-wire test-ssh-packet test-ssh-kex test-ssh-attack-host

ci-boot: test-ssh-os test-ssh-attack-os test-ssh-tamper-os
