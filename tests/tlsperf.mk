# TLS/crypto performance: the measurement, the gate, and the two controls.
#
# Kept in its own fragment rather than in the Makefile for the reason
# tests/prof.mk gives: several lines edit the Makefile at once, and a shared-file
# edit cannot be committed without swallowing theirs. The Makefile pulls it in
# with one line:
#
#     -include tests/tlsperf.mk
#
#   test-crypto-bench          per-primitive cost on the host, no network, no
#                              emulator. The RANKING is what this is for; the
#                              absolute numbers are about this laptop.
#   test-crypto-bench-gate     the one machine-checkable claim out of that:
#                              RSA-2048 verification must cost less than 4x an
#                              x25519 shared secret. Fast enough for CI.
#   test-crypto-bench-control  THE SAME GATE against rsa.c built with
#                              -DRSA_SLOW_CONTROL, which restores the pre-change
#                              Montgomery-form conversion. REQUIRED TO FAIL.
#   test-tls-bench             the per-phase breakdown of a real handshake, in
#                              the guest, over the real Internet. This is the
#                              number that says whether a handshake is CPU-bound
#                              at all -- and it says it is not.
#   test-tls-bench-control     the same harness against a kernel built with
#                              -DKPROF_DISABLE (make KPROF_OFF=1), so the spans
#                              compile to nothing. REQUIRED TO FAIL: without it,
#                              a harness that printed an empty table would look
#                              exactly like a handshake that cost nothing.

.PHONY: test-crypto-bench test-crypto-bench-gate test-crypto-bench-control \
        test-tls-bench test-tls-bench-control

# The bench links the same CRYPTO_SRC the vectors and the differential suite do,
# plus the real trust store -- the root-scan figure has to come from the bundle
# that ships, not from a stand-in.
BENCH_SRC := tests/unit/crypto_bench.c $(CRYPTO_SRC) c/crypto/trust/roots.c
BENCH_INC := $(CRYPTO_INC) -Ic/crypto/trust

test-crypto-bench: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_bench $(BENCH_SRC) $(BENCH_INC)
	$(BUILD)/crypto_bench

test-crypto-bench-gate: $(BUILD)
	$(CC) -O2 -Wall -Wextra -o $(BUILD)/crypto_bench $(BENCH_SRC) $(BENCH_INC)
	$(BUILD)/crypto_bench --gate

test-crypto-bench-control: $(BUILD)
	@$(CC) -O2 -w -DRSA_SLOW_CONTROL -o $(BUILD)/crypto_bench_slow $(BENCH_SRC) $(BENCH_INC)
	@if $(BUILD)/crypto_bench_slow --gate > $(BUILD)/crypto_bench_slow.log 2>&1; then \
	    echo "CONTROL FAILED: the pre-change RSA passed the gate -- the gate proves nothing"; \
	    cat $(BUILD)/crypto_bench_slow.log; exit 1; \
	 else \
	    echo "control ok: -DRSA_SLOW_CONTROL fails the gate as required --"; \
	    grep -E '^(gate|FAIL)' $(BUILD)/crypto_bench_slow.log; \
	 fi

test-tls-bench: $(ISO) $(DISK)
	@bash tests/boot/run-tls-bench.sh $(ISO) $(DISK) $(TLS_BENCH_URLS)

# Two-object rebuild and a relink, not a clean build, and the tree is put back
# on the way out whether the control passed or not -- same shape as
# tests/prof.mk's test-prof-control, for the same reason.
test-tls-bench-control: $(DISK)
	@touch c/net/tls/tls.c c/net/tls/tls12.c c/net/tls/x509.c c/kernel/core/kprof.c c/kernel/core/kdiag.c
	@$(MAKE) --no-print-directory KPROF_OFF=1 $(ISO) >/dev/null
	@rc=0; bash tests/boot/run-tls-bench.sh $(ISO) $(DISK) $(TLS_BENCH_URLS) \
	    > $(BUILD)/tls_bench_control.out 2>&1 || rc=$$?; \
	 touch c/net/tls/tls.c c/net/tls/tls12.c c/net/tls/x509.c c/kernel/core/kprof.c c/kernel/core/kdiag.c; \
	 $(MAKE) --no-print-directory $(ISO) >/dev/null; \
	 if [ "$$rc" = 0 ]; then \
	    echo "CONTROL FAILED: the harness passed against a kernel with no spans in it"; \
	    tail -20 $(BUILD)/tls_bench_control.out; exit 1; \
	 else \
	    echo "control ok: -DKPROF_DISABLE leaves the harness nothing to report --"; \
	    grep -E 'no span table|FAIL' $(BUILD)/tls_bench_control.out | head -4; \
	 fi
