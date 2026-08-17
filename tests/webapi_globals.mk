# tests/webapi_globals.mk -- the platform globals: encoding, and the event loop.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: concurrent sessions overwrite that file wholesale.
#
# WHAT THESE MEASURE, and the honest split. Two different kinds of target:
#
#   make test-encoding          a FLOOR on the WPT encoding/ subset. A ratchet,
#                               not a percentage to admire: it fails when
#                               something that decodes today stops decoding.
#   make test-encoding-negctl   the same suite with the label table cut out.
#                               MUST FAIL. See below -- this is the target that
#                               decides whether test-encoding means anything.
#   make test-eventloop         task/microtask/timer ordering, against a local
#                               corpus in tests/wpt-local. The vendored WPT
#                               subsets contain no html/webappapis/timers, so
#                               without this the ordering the whole harness
#                               rests on is asserted by reading js_page.c.
#
# WHY A FLOOR AND NOT A PER-SUBTEST RATCHET. tests/wpt.mk already owns the
# per-subtest expected-failure list for the whole corpus and it would be wrong
# to keep a second copy of the encoding slice of it -- two ratchets over the
# same subtests disagree the first time either is regenerated. This is the
# coarse guard that belongs to the code rather than to the corpus: it names one
# number, and that number is a count of subtests, not a rate, so growing the
# corpus cannot quietly satisfy it.

.PHONY: test-encoding test-encoding-negctl test-eventloop wg-local-root

WG_ROOT     ?= third_party/wpt
WG_LOCAL    := tests/wpt-local
# The floor. 11373 passed on 2026-08-08 (94.1% of 12086). The margin absorbs
# a corpus refresh that renames a handful of subtests; it does not absorb a
# decoder breaking, which costs hundreds at a time.
WG_ENC_FLOOR ?= 11200

# The runner is tests/wpt.mk's, and so are these two lists. Reusing them rather
# than copying: a second source list is a second thing to forget when a file is
# added, and the negative control MUST compile the same translation units as
# the thing it is controlling or it is not a control.
#
# DEFERRED (`=`, not `:=`) on purpose. This fragment and tests/wpt.mk are
# -include'd from the Makefile, another line owns that file, and nothing
# guarantees the order. With `:=` an include that landed first would capture an
# empty WPT_TEST_SRC and the negative control would silently link nothing --
# passing, which for a negative control is the failure mode that hides itself.
# Deferred expansion happens when the recipe runs, by which point both
# fragments have been read whatever order they arrived in.
#
# The coupling is real and worth stating: without tests/wpt.mk these targets
# have no runner and no source list.
WG_SRC = $(WPT_TEST_SRC)
WG_CF  = $(WPT_CF)

# --- wg-local-root: make tests/wpt-local a valid WPT root --------------------
# The runner insists on <root>/resources/testharness.js, so the local corpus
# needs the upstream resources/ and common/ beside it. Those are SYMLINKED AT
# BUILD TIME rather than committed: a committed symlink into third_party/ is
# one of the two things that reliably breaks a Windows checkout of this repo
# (the other being CRLF), and the fixtures fetch-size.any.js needs are
# generated here for the same reason -- a 267 KB blob in git to prove a
# 267 KB transfer is not worth the diff.
#
# Skips itself when the corpus is absent, exactly as the runner does.
wg-local-root:
	@if [ -d "$(WG_ROOT)/resources" ]; then \
	    ln -sfn "$(abspath $(WG_ROOT))/resources" $(WG_LOCAL)/resources; \
	    ln -sfn "$(abspath $(WG_ROOT))/common"    $(WG_LOCAL)/common; \
	 fi
	@mkdir -p $(WG_LOCAL)/platform/resources
	@python3 -c "import json,io; \
io.open('$(WG_LOCAL)/platform/resources/small.json','w').write(json.dumps([{'i':i} for i in range(50)])); \
io.open('$(WG_LOCAL)/platform/resources/big.json','w').write(json.dumps([{'i':i,'pad':'x'*200} for i in range(1200)]))"

# --- test-encoding ----------------------------------------------------------
test-encoding: $(BUILD)/wpt_test
	@n=$$($(BUILD)/wpt_test --root $(WG_ROOT) --subset encoding -b /dev/null 2>/dev/null \
	      | sed -n 's/^WPT: \([0-9]*\)\/.*/\1/p'); \
	 t=$$($(BUILD)/wpt_test --root $(WG_ROOT) --subset encoding -b /dev/null 2>/dev/null \
	      | sed -n 's/^WPT: [0-9]*\/\([0-9]*\).*/\1/p'); \
	 if [ -z "$$n" ]; then echo "test-encoding: no corpus at $(WG_ROOT) -- not a regression"; exit 0; fi; \
	 echo "test-encoding: $$n/$$t encoding subtests pass (floor $(WG_ENC_FLOOR))"; \
	 if [ "$$n" -lt "$(WG_ENC_FLOOR)" ]; then \
	    echo "test-encoding: FAILED -- below the floor. Something that decoded stopped."; \
	    exit 1; \
	 fi

# --- test-encoding-negctl: the negative control -----------------------------
# -DENC_NEGCTL_IGNORE_LABEL cuts ONE thing out of js_encoding.inc: the
# label -> encoding lookup. Everything else -- the decoders, the driver, the
# BOM sniff, encodeInto, the 27 index tables -- is compiled exactly as it
# ships. Every TextDecoder then believes it is UTF-8, which is precisely the
# implementation this line replaced and precisely the one that renders every
# real page correctly.
#
# So the suite MUST fall below the floor here. If it does not, test-encoding is
# measuring the presence of the corpus rather than the correctness of the
# decoder, and its number should not be quoted.
test-encoding-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@# $(GFX_SRC): a CONTROL is a copy of the target it controls, and it
	@# drifts. The positive target above was fixed when svg.c started
	@# building its shapes with gfx_path/gfx_fill; this line was not, and
	@# nothing noticed because no suite reaches a negative control.
	@$(CC) -O2 -w $(WG_CF) -DENC_NEGCTL_IGNORE_LABEL \
	    -o $(BUILD)/wpt_encnegctl $(WG_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@n=$$($(BUILD)/wpt_encnegctl --root $(WG_ROOT) --subset encoding -b /dev/null 2>/dev/null \
	      | sed -n 's/^WPT: \([0-9]*\)\/.*/\1/p'); \
	 if [ -z "$$n" ]; then echo "test-encoding-negctl: no corpus -- skipped"; exit 0; fi; \
	 echo "test-encoding-negctl: label table removed -> $$n subtests pass (floor $(WG_ENC_FLOOR))"; \
	 if [ "$$n" -ge "$(WG_ENC_FLOOR)" ]; then \
	    echo "test-encoding-negctl: FAILED -- the suite still meets its floor with"; \
	    echo "  the label table cut out, so it is not measuring the decoder."; \
	    exit 1; \
	 else \
	    echo "test-encoding-negctl: ok -- the suite collapses without the table."; \
	 fi

# --- test-eventloop ---------------------------------------------------------
# tests/wpt-local/platform/event-loop-order.any.js, run by the same runner. It
# is a WPT-format file in a WPT-format root, so it costs no second harness.
#
# fetch-size.any.js lives in the same directory and is deliberately NOT run
# here: its large-resource case fails, and that failure is a finding about the
# RUNNER's drain loop (it advances a virtual clock to the next timer deadline
# while a fetch is mid-transfer), not about this code. A known-red case inside
# a gate would only teach people to ignore the gate.
test-eventloop: $(BUILD)/wpt_test wg-local-root
	@if [ ! -e $(WG_LOCAL)/resources/testharness.js ]; then \
	    echo "test-eventloop: no corpus at $(WG_ROOT) -- skipped"; exit 0; fi; \
	 $(BUILD)/wpt_test --root $(WG_LOCAL) --subset platform \
	    --only platform/event-loop-order -b /dev/null -v 20 2>&1 \
	    | grep -E '^  (FAIL|HARNESS)' || true; \
	 $(BUILD)/wpt_test --root $(WG_LOCAL) --subset platform \
	    --only platform/event-loop-order -b /dev/null 2>/dev/null \
	    | sed -n 's/^WPT: \([0-9]*\)\/\([0-9]*\).*/\1 \2/p' | { read got tot; \
	      if [ -z "$$tot" ] || [ "$$tot" -eq 0 ]; then \
	        echo "test-eventloop: FAILED -- ZERO subtests ran. 0/0 is not a pass:"; \
	        echo "  --only filters within the runner's default subset list, so a"; \
	        echo "  local root needs --subset too or it silently matches nothing."; \
	        exit 1; \
	      elif [ "$$got" != "$$tot" ]; then \
	        echo "test-eventloop: FAILED -- $$got/$$tot"; exit 1; \
	      else echo "test-eventloop: ok -- $$got/$$tot"; fi; }
