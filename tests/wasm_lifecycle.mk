# Uses the actual js_page lifecycle plus real Wasm; the older Wasm API suite
# manually resets its runtime and therefore cannot measure this wiring seam.
WASM_LIFECYCLE_SRC := tests/unit/wasm_lifecycle_test.c \
    $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) c/apps/browser/js_wasm.c
WASM_LIFECYCLE_DEP := $(WASM_LIFECYCLE_SRC) tests/unit/dom_iface_test.c \
    tests/unit/wasm_js_modules.inc $(WASM_EXEC_SRC) c/apps/browser/js_wasm_prelude.inc \
    c/apps/browser/js_wasm.h c/apps/browser/js_page.h $(HTML_PARSER_SRC) $(QJS_SRC)

$(BUILD)/wiring/wasm_lifecycle_test: $(WASM_LIFECYCLE_DEP) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring
	@$(CC) -O1 -g -w $(DOMIFACE_CF) -Ic/lib/wasm -Itests/unit -o $@ \
	    $(WASM_LIFECYCLE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

.PHONY: test-wasm-lifecycle test-wasm-lifecycle-negctl
test-wasm-lifecycle: test-wasm-lifecycle-negctl $(BUILD)/wiring/wasm_lifecycle_test
	@$(BUILD)/wiring/wasm_lifecycle_test

# These independently reinstate the two old defects. Require the named failed
# case, not merely any nonzero exit (an unrelated linker/fixture crash is red).
test-wasm-lifecycle-negctl: $(WASM_LIFECYCLE_DEP) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/wiring
	@set -e; for defect in CLASS RESET; do \
	  bin=$(BUILD)/wiring/wasm_lifecycle_neg_$$defect; \
	  $(CC) -O1 -g -w $(DOMIFACE_CF) -Ic/lib/wasm -Itests/unit \
	    -DWASM_LIFECYCLE_NEG_$$defect -o $$bin $(WASM_LIFECYCLE_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	  if $$bin > $$bin.log 2>&1; then echo "FAIL: Wasm $$defect control stayed green"; exit 1; fi; \
	  case $$defect in CLASS) expected='reopen registers native class';; \
	    RESET) expected='close releases retained resources';; esac; \
	  grep "^FAIL WASM-LIFECYCLE $$expected" $$bin.log; \
	done
