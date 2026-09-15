# tests/tlsfix.mk -- the chain path-building fix and its gates.
# Owned by the tlsfix wave-1 agent (2026-08-30); the root Makefile's
# `-include tests/tlsfix.mk` line was pre-wired by commit 1461cd173 -- this
# fragment fills it in and does not touch the Makefile.
#
# WHAT THIS GATES, AND THE BUG IT CAME FROM
# ------------------------------------------
# jd.com's static CDNs misc.360buyimg.com and static.360buyimg.com send the
# GlobalSign intermediate TWICE in one flight -- [leaf, inter, inter] -- while
# storage.360buyimg.com and www.jd.com send [leaf, inter] for the very same
# *.jd.com certificate. x509_verify_chain required the flight to arrive as a
# strictly ordered chain (certs[i] signed by certs[i+1]), so link 2 of the
# duplicate flight compared inter's issuer against the COPY's subject and
# returned X509_E_UNTRUSTED: every subresource on those two CDNs died with
# "TLS refused: handshake or certificate verification failed" while the host
# and the third CDN worked (tests/scoreboard/roadmap-2026-08-30.md, jd row).
# x509.c now builds the path instead of assuming the order; these gates hold
# that fix with REAL captured flights, including the two that were refused.
#
#   test-tls-chain          the fixture gate: our verifier ACCEPTS the five
#                           captured chains (bing, jd, misc/static/storage
#                           .360buyimg.com) plus two synthesized disorder
#                           cases, and REJECTS a corrupted signature and a
#                           wrong-host leaf. Offline, deterministic, CI-safe.
#   test-tls-chain-negctl   the same driver built with
#                           -DLOGIT_X509_BREAK_TRUSTALL (x509.c trusts
#                           everything). The control PASSES only when the
#                           gate CATCHES the break, i.e. when both reject
#                           cases go wrong under it. A PREREQUISITE of the
#                           positive, per the stranded-controls rule: naming
#                           a control on a ci-host: line satisfies the audit
#                           and runs it never.
#   test-tls-chain-live     dials the six REAL hosts with the REAL client
#                           (the tls_interop_test.c bridge) and asserts every
#                           handshake completes. This is the gate that was
#                           watched RED before the fix:
#                             [tls] chain of 3 rejected for misc.360buyimg.com:
#                                   no path to a trusted root (-3)
#                             RESULT: FAIL (tls_step rc=-2)
#                           and green after ("chain of 2 verified", the path
#                           is deduplicated while the flight still carries
#                           3). It needs outbound :443, so it is NOT on
#                           ci-host: -- it skips loudly, naming the check,
#                           when the network is absent.

.PHONY: test-tls-chain test-tls-chain-negctl test-tls-chain-live

# Source lists are derived from the KERNEL's own link list, never hand-copied
# (AGENTS.md rule 3): a new TU under c/net/tls/ joins these gates the day it
# lands. tls_server.c is subtracted because it is the server role and pulls
# cert-generation TUs the client never calls. mlkem.c AND mlkem_rand.c are
# filtered in from C_SRC because the client's X25519MLKEM768 offer calls
# mlkem768_keygen/_encaps, which live in mlkem_rand.c (the seeded wrappers)
# -- mlkem.c alone links only the _derand/_decaps core, a split that cost one
# failed link to rediscover. CRYPTO_SRC (keccak aside) stops short of
# c/crypto/pq by design (its own comment says the pq randomness TUs belong to
# their own gates), so the two names are explicit here.
TLSFIX_TLS   := $(filter-out c/net/tls/tls_server.c,$(filter c/net/tls/%,$(C_SRC)))
TLSFIX_MLKEM := $(filter c/crypto/pq/mlkem.c c/crypto/pq/mlkem_rand.c,$(C_SRC))

# The fixture gate: x509.c + the real 130-root store + the crypto battery.
TLSFIX_CHAIN_SRC := tests/unit/tls_chain_test.c $(filter c/net/tls/x509.c,$(C_SRC)) \
                    $(CRYPTO_SRC) c/crypto/trust/roots.c
TLSFIX_INC := $(CRYPTO_INC) -Ic/crypto/trust -Ic/net/tls
# The live gate additionally links the whole client (tls.c tls12.c tls_psk.c
# ocsp.c) through the interop bridge, which provides host TCP/timer/kprintf
# stand-ins with the kernel contracts tls.c assumes.
TLSFIX_LIVE_SRC := tests/unit/tls_interop_test.c $(TLSFIX_TLS) $(TLSFIX_MLKEM) \
                   $(CRYPTO_SRC) c/crypto/trust/roots.c

TLSFIX_FIXTURES := tests/fixtures/tls

# The ten cases. Each line: <pem> <sni-host> <expect> [mutations]. The two
# "3 certs -> path 2" lines are the live bug; --dup and --swap12 are the
# same defect synthesized onto a clean flight, so the gate does not depend on
# the CDN still sending the duplicate the day it is run; the two reject lines
# are what the negative control must be able to catch.
test-tls-chain: test-tls-chain-negctl $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -o $(BUILD)/tls_chain_test $(TLSFIX_CHAIN_SRC) $(TLSFIX_INC) || { \
	  echo "FAIL: could not build tls_chain_test"; exit 1; }
	@rc=0; \
	for case in \
	  "$(TLSFIX_FIXTURES)/www.bing.com.chain.pem www.bing.com accept" \
	  "$(TLSFIX_FIXTURES)/www.jd.com.chain.pem www.jd.com accept" \
	  "$(TLSFIX_FIXTURES)/misc.360buyimg.com.chain.pem misc.360buyimg.com accept" \
	  "$(TLSFIX_FIXTURES)/static.360buyimg.com.chain.pem static.360buyimg.com accept" \
	  "$(TLSFIX_FIXTURES)/storage.360buyimg.com.chain.pem storage.360buyimg.com accept" \
	  "$(TLSFIX_FIXTURES)/www.jd.com.chain.pem www.jd.com accept --dup 1" \
	  "$(TLSFIX_FIXTURES)/www.bing.com.chain.pem www.bing.com accept --swap12" \
	  "$(TLSFIX_FIXTURES)/www.bing.com.chain.pem www.bing.com reject --flip-sig 0" \
	  "$(TLSFIX_FIXTURES)/www.bing.com.chain.pem www.bing.com reject --flip-sig 1" \
	  "$(TLSFIX_FIXTURES)/www.jd.com.chain.pem www.jd.com reject --wrong-host" \
	; do \
	  echo "$$case" | xargs $(BUILD)/tls_chain_test || rc=1; \
	done; \
	[ $$rc -eq 0 ] && echo "PASS: test-tls-chain (10 cases)" || \
	  echo "FAIL: test-tls-chain"; exit $$rc

# WATCHED RED 2026-08-30 (the break build, both reject cases):
#     chain of 3 (path 3) for www.bing.com: trusted (0)
#     FAIL www.bing.com.chain.pem: expected reject, got trusted (0)
#     chain of 2 (path 2) for not-the-sni.example: trusted (0)
#     FAIL www.jd.com.chain.pem: expected reject, got trusted (0)
# i.e. under the break the gate refuses nothing, and these two exits going
# nonzero are exactly what proves the reject assertions have teeth.
test-tls-chain-negctl: $(BUILD)
	@$(CC) -O1 -g -w -DLOGIT_X509_BREAK_TRUSTALL \
	  -o $(BUILD)/tls_chain_break $(TLSFIX_CHAIN_SRC) $(TLSFIX_INC) || { \
	  echo "FAIL: could not build the control binary"; exit 1; }
	@rc=0; \
	for case in \
	  "$(TLSFIX_FIXTURES)/www.bing.com.chain.pem www.bing.com reject --flip-sig 0" \
	  "$(TLSFIX_FIXTURES)/www.jd.com.chain.pem www.jd.com reject --wrong-host" \
	; do \
	  if echo "$$case" | xargs $(BUILD)/tls_chain_break >/dev/null 2>&1; then \
	    echo "CONTROL FAILED: under LOGIT_X509_BREAK_TRUSTALL a reject case"; \
	    echo "  passed -- the fixture gate cannot detect a verifier that trusts"; \
	    echo "  everything, and its accept assertions are the only ones standing."; \
	    echo "  case: $$case"; rc=1; \
	  else \
	    echo "control ok: the break was caught -- $$case"; \
	  fi; \
	done; \
	[ $$rc -eq 0 ] && echo "PASS: test-tls-chain-negctl (2 controls fired)"; exit $$rc

# The live differential. Network-dependent on purpose: it exists to be run by
# a human at the moment a "TLS refused" report lands, against the wire as it
# is RIGHT NOW. A host with no outbound :443 must SKIP LOUDLY (one line naming
# the missing capability and the command that would settle it) rather than
# fail -- a red network target on CI is a gate people learn to ignore, and a
# silent green is worse. Distinguishable because the bridge prints
# "RESULT: FAIL (connect)" for a DIAL failure and nothing else in that shape.
test-tls-chain-live: $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -o $(BUILD)/tls_chain_live $(TLSFIX_LIVE_SRC) $(TLSFIX_INC) \
	  -Ic/net/core -Ic/net/transport -Ic/drivers/timer $(KCORE_INC) || { \
	  echo "FAIL: could not build tls_chain_live"; exit 1; }
	@if ! $(BUILD)/tls_chain_live www.bing.com 443 www.bing.com 2>&1 | grep -q "RESULT: PASS"; then \
	  if $(BUILD)/tls_chain_live www.bing.com 443 www.bing.com 2>&1 | grep -q "(connect)"; then \
	    echo "SKIP: this host has no outbound TLS to www.bing.com:443 -- the live gate"; \
	    echo "      needs the real wire. Settle the network with:"; \
	    echo "          curl -sI --max-time 10 https://www.bing.com/"; \
	    echo "      then rerun: make BUILD=build-tls test-tls-chain-live"; \
	    echo "      The OFFLINE fixture gate (make test-tls-chain) is unaffected."; \
	    exit 0; \
	  fi; \
	  echo "FAIL (live): www.bing.com reached the network but did not complete -- full output:"; \
	  $(BUILD)/tls_chain_live www.bing.com 443 www.bing.com 2>&1 | tail -5; exit 1; \
	fi
	# cloudflare.com carries the PQ hybrid: it negotiates X25519MLKEM768
	# (MEASURED 2026-08-30: suite 0x1301, group X25519MLKEM768, chain of 3
	# verified, GET completed) while the jd CDNs take the plain-x25519 half
	# of the dual key_share with no HRR and bing answers TLS 1.2 -- so one
	# run of this gate exercises all three ClientHello shapes the client can
	# end up committed to. www.google.com also speaks the hybrid but is
	# unreachable from this network (RESULT: FAIL (connect)), which is
	# exactly the dial-failure shape the SKIP branch above exists for.
	@rc=0; \
	for h in cloudflare.com www.jd.com misc.360buyimg.com static.360buyimg.com storage.360buyimg.com; do \
	  if ! $(BUILD)/tls_chain_live $$h 443 $$h 2>&1 | grep -q "RESULT: PASS"; then \
	    echo "FAIL (live): $$h did not complete a handshake -- full output:"; \
	    $(BUILD)/tls_chain_live $$h 443 $$h 2>&1 | tail -5; rc=1; \
	  else \
	    echo "ok   (live): $$h handshake + GET completed"; \
	  fi; \
	done; \
	echo "ok   (live): www.bing.com handshake + GET completed"; \
	[ $$rc -eq 0 ] && echo "PASS: test-tls-chain-live (6 hosts)" || exit $$rc

# Reachability for the audit. The LIVE gate is deliberately absent: ci hosts
# have no outbound :443, and a network target on ci-host: is a gate that rots
# red for reasons that have nothing to do with the code under test.
ci-host: test-tls-chain
