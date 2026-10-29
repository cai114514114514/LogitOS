# SPDX-License-Identifier: MIT
# Readonly Document.referrer on both wrapper families and real page lifetimes.
# This includes shipping WebAPI history; network-only dependencies are stubs.
DOCUMENT_REFERRER_SRC := tests/unit/document_referrer_test.c \
    $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) \
    c/apps/gui/browser/js/dom/domparser.c
DOCUMENT_REFERRER_DEP := $(DOCUMENT_REFERRER_SRC) $(HTML_PARSER_SRC) \
    c/apps/gui/browser/js/dom/dom_iface.inc $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
DOCUMENT_REFERRER_CF := $(DOMIFACE_CF)

.PHONY: test-document-referrer test-document-referrer-negctl
$(BUILD)/document_referrer_test: $(DOCUMENT_REFERRER_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOCUMENT_REFERRER_CF) -o $@ $(DOCUMENT_REFERRER_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(BUILD)/document_referrer_negctl: $(DOCUMENT_REFERRER_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOCUMENT_REFERRER_CF) -DDOCUMENT_NO_REFERRER -o $@ \
	    $(DOCUMENT_REFERRER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-document-referrer-negctl: $(BUILD)/document_referrer_negctl
	@rc=0; $(BUILD)/document_referrer_negctl > $(BUILD)/document_referrer_negctl.log 2>&1 || rc=$$?; \
	    rg '^FAIL: (direct navigation|Promise referrer.substr)' $(BUILD)/document_referrer_negctl.log; \
	    test $$rc -eq 1 && test "$$(rg -c '^FAIL: (direct navigation|Promise referrer.substr)' $(BUILD)/document_referrer_negctl.log)" -eq 2

test-document-referrer: test-document-referrer-negctl $(BUILD)/document_referrer_test
	@$(BUILD)/document_referrer_test

ci-host: test-document-referrer

DOCUMENT_REFERRER_GUEST_ISO ?= $(BUILD)/logit.iso
DOCUMENT_REFERRER_GUEST_DISK ?= $(BUILD)/disk.img


# The two Document wrapper families share this existing wired fragment. Focus
# additionally links the native forms owner; no frame scripts or network run.
DOCUMENT_FOCUS_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) \
    tests/unit/document_focus_test.c c/apps/gui/browser/js/dom/domparser.c
DOCUMENT_FOCUS_DEP = $(DOCUMENT_FOCUS_SRC) $(HTML_PARSER_SRC) \
    c/apps/gui/browser/js/dom/dom_iface.inc $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-document-focus test-document-focus-negctl
$(BUILD)/document_focus_test: $(DOCUMENT_FOCUS_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(DOCUMENT_FOCUS_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/document_focus_negctl: $(DOCUMENT_FOCUS_DEP)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOMIFACE_CF) -DDOCUMENT_NO_HAS_FOCUS -o $@ $(DOCUMENT_FOCUS_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-document-focus-negctl: $(BUILD)/document_focus_negctl
	@rc=0; $< > $(BUILD)/document_focus_negctl.log 2>&1 || rc=$$?; \
	    rg '^FAIL: unfocused document has a real method returning false' $(BUILD)/document_focus_negctl.log; \
	    test $$rc -eq 1
test-document-focus: test-document-focus-negctl $(BUILD)/document_focus_test
	@$(BUILD)/document_focus_test
ci-host: test-document-focus
