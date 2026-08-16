# HTTP byte ranges in the browser's fetch path.
#
#   test-range           the real browser_rt.c + the real c/net/http/http1.c
#                        over a stubbed socket layer, against an in-memory
#                        origin that answers 206 / 200 / 416 / multipart on
#                        demand -- which no live server can be made to do in
#                        one run.
#   test-range-negctl    the same file against a browser_rt.c built with
#                        -DBFETCH_NO_RANGE_CHECK, i.e. the response side
#                        removed and only the request side left. REQUIRED to
#                        fail, and to fail on the checks the response side is
#                        about while the request-side checks still pass.
#   test-range-asan      the same under ASan/UBSan. A Content-Range is
#                        attacker-controlled arithmetic that decides how many
#                        bytes a caller will read out of a buffer.
#   test-range-wire      the claim on a REAL server: a range GET against a
#                        public URL, byte count asserted. Needs network; it is
#                        NOT a prerequisite of anything.
#
# The stub directory must come FIRST on the include path -- tests/unit/h2stub/
# logit.h shadows the ring-3 c/apps/logit.h, which is what lets the real
# browser_rt.c be compiled on the host. c/apps is deliberately NOT on the path:
# if it were, the real int-0x80 syscall wrappers would win and nothing would
# link. This is the same arrangement tests/http2.mk uses, and the stub is
# shared with it rather than copied.
#
# tests/unit/loader_fakebfetch.c is deliberately NOT in this link. It
# IMPLEMENTS bfetch.h in memory, so a range test built on it would assert that
# a fake honours a contract, and the `Range:` request line -- half of what a
# byte range is -- would never be produced at all.
RANGE_INC := -Itests/unit/h2stub -Iinclude/abi -Ic/apps/browser -Ic/net/http -Ic/lib/image
RANGE_SRC := tests/unit/range_test.c c/apps/browser/browser_rt.c \
             c/net/http/http1.c c/net/http/http2.c c/net/http/hpack.c \
             c/net/http/hpool.c c/net/http/url.c

.PHONY: test-range test-range-negctl test-range-asan test-range-wire

test-range:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(RANGE_INC) -o $(BUILD)/range_test $(RANGE_SRC)
	@$(BUILD)/range_test

# THE NEGATIVE CONTROL. -DBFETCH_NO_RANGE_CHECK builds the browser that sends a
# Range header and then believes whatever comes back -- which is the honest
# description of "add Range support" done the quick way, and is exactly what
# this unit exists not to be. It must FAIL, and it must fail on:
#   - 200-to-a-Range reported as such (without the check it looks like a slice);
#   - 416 as a named failure (without it, an empty 200-shaped success);
#   - multipart/byteranges refused (without it, MIME boundary text as payload);
#   - a 206 whose Content-Range disagrees with its body length.
# And it must still PASS the request-side checks -- if the control broke the
# Range header itself, its failures would prove nothing about the response side.
test-range-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(RANGE_INC) -DBFETCH_NO_RANGE_CHECK \
	    -o $(BUILD)/range_negctl $(RANGE_SRC)
	@if $(BUILD)/range_negctl > $(BUILD)/range_negctl.log 2>&1; then \
	    echo "FAIL: the negative control PASSED -- test-range does not measure the response side"; \
	    exit 1; fi
	@grep -q 'FAIL bfetch_range_result(id, &f, &l, &t) == BF_R_IGNORED' $(BUILD)/range_negctl.log || \
	    { echo "FAIL: the control failed, but a 200 answering a Range still got reported"; exit 1; }
	@grep -q 'FAIL settle(id) == BF_FAILED' $(BUILD)/range_negctl.log || \
	    { echo "FAIL: the control failed, but 416/multipart were still refused"; exit 1; }
	@grep -q 'FAIL not refused by name' $(BUILD)/range_negctl.log || \
	    { echo "FAIL: the control failed, but the dishonest 206s were still caught"; exit 1; }
	@grep -q 'ok   !strcmp(hv, "bytes=100-199")' $(BUILD)/range_negctl.log || \
	    { echo "FAIL: the control broke the request side, not just the verdict"; exit 1; }
	@echo "ok: the range negative control fails, and on the right checks"

test-range-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all \
	    $(RANGE_INC) -o $(BUILD)/range_asan $(RANGE_SRC)
	@$(BUILD)/range_asan

# ON A REAL SERVER. Everything above is an in-memory origin, which proves the
# parsing and the verdicts and proves nothing about whether the public web
# answers this request the way the fixture does. This asks a real host for two
# disjoint slices of one file over the host's own network stack and requires
# the bytes to match a whole-file download at those offsets. Not a prerequisite
# of anything: it needs the network, and a test suite that fails when the
# network is down is a test suite people stop running.
test-range-wire:
	@bash tests/unit/range_wire.sh
