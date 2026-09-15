DOM_WRAPPER_SRC = $(sort $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) c/apps/browser/js_domparser.c c/apps/browser/js_frame.c) tests/unit/dom_wrapper_lifetime_test.c
DOM_WRAPPER_DEPS = $(DOM_WRAPPER_SRC) tests/unit/select_state_test.c c/apps/browser/js_dom.h $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-dom-wrapper-lifetime test-dom-wrapper-lifetime-negctl
$(BUILD)/wiring-next/dom_wrapper_lifetime: $(DOM_WRAPPER_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-dom-wrapper-lifetime-negctl: $(DOM_WRAPPER_DEPS)
	@mkdir -p $(BUILD)/wiring-next
	@set -e; for ctl in JSDOM_WEAK_WRAPPERS JSDOM_NO_WRAPPER_GC_MARK; do \
	 $(CC) -O2 -w $(DOMIFACE_CF) -D$$ctl -o $(BUILD)/wiring-next/wrapper_$$ctl $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	 set +e; $(BUILD)/wiring-next/wrapper_$$ctl > $(BUILD)/wiring-next/wrapper_$$ctl.log 2>&1; rc=$$?; set -e; cat $(BUILD)/wiring-next/wrapper_$$ctl.log; test $$rc -eq 1; \
	 case $$ctl in JSDOM_WEAK_WRAPPERS) grep -q 'FAIL connected Symbol expando survives wrapper release' $(BUILD)/wiring-next/wrapper_$$ctl.log;; JSDOM_NO_WRAPPER_GC_MARK) grep -q 'FAIL detached wrapper cycles are collected' $(BUILD)/wiring-next/wrapper_$$ctl.log;; esac; done

test-dom-wrapper-lifetime: test-dom-wrapper-lifetime-negctl $(BUILD)/wiring-next/dom_wrapper_lifetime
	@$(BUILD)/wiring-next/dom_wrapper_lifetime
.PHONY: test-dom-wrapper-lifetime-asan
test-dom-wrapper-lifetime-asan: test-dom-wrapper-lifetime
	@$(CC) -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/wiring-next/dom_wrapper_asan $(DOM_WRAPPER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wiring-next/dom_wrapper_asan

# Two live independent runtimes exercise the DOM state boundary; this does
# not install the still-singleton optional platform/network modules in child.
DOM_CONTEXT_SRC = tests/unit/dom_context_test.c $(JSDOM_HOST_SRC)
DOM_CONTEXT_DEPS = $(DOM_CONTEXT_SRC) c/apps/browser/js_dom.h c/apps/browser/js_dom_iface.inc tests/dom_wrapper_lifetime.mk $(BUILD)/libcss_host.a
.SECONDEXPANSION:
$(BUILD)/dom-context/current: $$(DOM_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(JSDOM_HOST_CF) -o $@ $(DOM_CONTEXT_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/dom-context/negative: $$(DOM_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/X(g_scroll_x) X(g_scroll_y)//' c/apps/browser/js_dom.c > $(BUILD)/dom-context/no-viewport.c
	@! cmp -s c/apps/browser/js_dom.c $(BUILD)/dom-context/no-viewport.c
	@$(CC) -O2 -w $(JSDOM_HOST_CF) -o $@ $(filter-out c/apps/browser/js_dom.c,$(DOM_CONTEXT_SRC)) $(BUILD)/dom-context/no-viewport.c $(BUILD)/libcss_host.a -lm
.PHONY: test-dom-context test-dom-context-negctl test-dom-context-asan test-dom-context-fields
test-dom-context-fields:
	@mkdir -p $(BUILD)/dom-context
	@clang -fsyntax-only -Xclang -ast-dump=json $(JSDOM_HOST_CF) c/apps/browser/js_dom.c > $(BUILD)/dom-context/fields.json
	@python3 tests/check_dom_context_fields.py $(BUILD)/dom-context/fields.json c/apps/browser/js_dom.c
test-dom-context-negctl: $(BUILD)/dom-context/negative
	@rc=0; $< > $(BUILD)/dom-context/negative.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q '^FAIL: parent viewport restored' $(BUILD)/dom-context/negative.log
test-dom-context: test-dom-context-negctl test-dom-context-fields $(BUILD)/dom-context/current
	@$(BUILD)/dom-context/current
test-dom-context-asan: test-dom-context
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(JSDOM_HOST_CF) -o $(BUILD)/dom-context/asan $(DOM_CONTEXT_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/dom-context/asan
ci-host: test-dom-context

PAGE_CONTEXT_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/page_context_test.c
PAGE_CONTEXT_DEPS = $(PAGE_CONTEXT_SRC) tests/unit/dom_iface_test.c tests/dom_wrapper_lifetime.mk $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/dom-context/page: $$(PAGE_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(PAGE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/dom-context/page-negative: $$(PAGE_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/s->g_page=\&s->queue;/s->g_page=\&page_default_queue;/' c/apps/browser/js_page.c > $(BUILD)/dom-context/shared-queue.c
	@! cmp -s c/apps/browser/js_page.c $(BUILD)/dom-context/shared-queue.c
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(filter-out c/apps/browser/js_page.c,$(PAGE_CONTEXT_SRC)) $(BUILD)/dom-context/shared-queue.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-page-context test-page-context-negctl test-page-context-asan test-page-context-fields
test-page-context-fields:
	@mkdir -p $(BUILD)/dom-context
	@clang -fsyntax-only -Xclang -ast-dump=json $(DOMIFACE_CF) c/apps/browser/js_page.c > $(BUILD)/dom-context/page-fields.json
	@python3 tests/check_dom_context_fields.py --kind page $(BUILD)/dom-context/page-fields.json c/apps/browser/js_page.c
test-page-context-negctl: $(BUILD)/dom-context/page-negative
	@rc=0; $< > $(BUILD)/dom-context/page-negative.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q '^FAIL: parent timer survives child queue initialization' $(BUILD)/dom-context/page-negative.log
test-page-context: test-page-context-negctl test-page-context-fields $(BUILD)/dom-context/page
	@$(BUILD)/dom-context/page
test-page-context-asan: test-page-context
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/dom-context/page-asan $(PAGE_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/dom-context/page-asan
ci-host: test-page-context

# Opt-in WebAPI children reuse the shipping request engine and page scheduler.
# Negative controls independently break state isolation and cookie-site policy.
PAGE_WEBAPI_CONTEXT_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/page_webapi_context_test.c
PAGE_WEBAPI_CONTEXT_DEPS = $(PAGE_WEBAPI_CONTEXT_SRC) tests/unit/dom_iface_test.c tests/dom_wrapper_lifetime.mk tests/check_dom_context_fields.py $(HTML_PARSER_SRC) $(QJS_SRC) $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/dom-context/webapi: $$(PAGE_WEBAPI_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(PAGE_WEBAPI_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/dom-context/webapi-neg-%: $$(PAGE_WEBAPI_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@case '$*' in navigation) sed 's/X(g_pending_nav) X(g_have_pending_nav)//' c/apps/browser/js_webapi.c > $@.c;; site) sed 's/g_embedded_site ? g_site_url : url/url/' c/apps/browser/js_webapi.c > $@.c;; policy) sed 's/if(!r->connect_policy)return 1;/return 1;/' c/apps/browser/js_webapi.c > $@.c;; *) exit 2;; esac
	@! cmp -s c/apps/browser/js_webapi.c $@.c
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(filter-out c/apps/browser/js_webapi.c,$(PAGE_WEBAPI_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-page-webapi-context test-page-webapi-context-negctl test-page-webapi-context-fields test-page-webapi-context-asan
test-page-webapi-context-fields:
	@mkdir -p $(BUILD)/dom-context
	@clang -fsyntax-only -Xclang -ast-dump=json $(DOMIFACE_CF) c/apps/browser/js_webapi.c > $(BUILD)/dom-context/webapi-fields.json
	@python3 tests/check_dom_context_fields.py --kind webapi $(BUILD)/dom-context/webapi-fields.json c/apps/browser/js_webapi.c
	@clang -fsyntax-only -Xclang -ast-dump=json $(DOMIFACE_CF) c/apps/browser/js_platform.c > $(BUILD)/dom-context/platform-fields.json
	@python3 tests/check_dom_context_fields.py --kind platform $(BUILD)/dom-context/platform-fields.json c/apps/browser/js_platform.c
$(BUILD)/dom-context/platform-neg-owner: $$(PAGE_WEBAPI_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/native_mo_notify, platform_current()/native_mo_notify, NULL/' c/apps/browser/js_platform.c > $@.c
	@! cmp -s c/apps/browser/js_platform.c $@.c
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(filter-out c/apps/browser/js_platform.c,$(PAGE_WEBAPI_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-page-webapi-context-negctl: $(BUILD)/dom-context/webapi-neg-navigation $(BUILD)/dom-context/webapi-neg-site $(BUILD)/dom-context/platform-neg-owner
	@rc=0; $(BUILD)/dom-context/webapi-neg-navigation > $(BUILD)/dom-context/webapi-neg-navigation.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: parent navigation state restored' $(BUILD)/dom-context/webapi-neg-navigation.log
	@rc=0; $(BUILD)/dom-context/webapi-neg-site > $(BUILD)/dom-context/webapi-neg-site.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: embedded request preserves ancestor cookie policy after history' $(BUILD)/dom-context/webapi-neg-site.log
	@rc=0; $(BUILD)/dom-context/platform-neg-owner > $(BUILD)/dom-context/platform-neg-owner.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: inactive child observer record delivered in child runtime' $(BUILD)/dom-context/platform-neg-owner.log

# Native policy controls change only the enforcing branch, leaving the same
# fixture, page evaluation and streaming transport in place.
$(BUILD)/dom-context/eval-policy-negative: $$(PAGE_WEBAPI_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/if (ctx->string_code_gen_disabled)/if (0)/' third_party/quickjs/quickjs.c > $@.c
	@! cmp -s third_party/quickjs/quickjs.c $@.c
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(PAGE_WEBAPI_CONTEXT_SRC) $(HTML_PARSER_SRC) $(filter-out third_party/quickjs/quickjs.c,$(QJS_SRC)) $@.c $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-page-webapi-policy-negctl
$(BUILD)/dom-context/selector-negative: $$(PAGE_WEBAPI_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/PAGE_PLATFORM_HAVE(js_select_install)/PAGE_HAVE(js_select_install)/' c/apps/browser/js_page.c > $@.c
	@! cmp -s c/apps/browser/js_page.c $@.c
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(filter-out c/apps/browser/js_page.c,$(PAGE_WEBAPI_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-page-child-selector-negctl
test-page-child-selector-negctl: $(BUILD)/dom-context/selector-negative
	@rc=0; $< > $(BUILD)/dom-context/selector-negative.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: child installs real selector consumers' $(BUILD)/dom-context/selector-negative.log
test-page-webapi-context-negctl: test-page-child-selector-negctl
test-page-webapi-context-negctl: test-page-webapi-policy-negctl
test-page-webapi-policy-negctl: $(BUILD)/dom-context/webapi-neg-policy $(BUILD)/dom-context/eval-policy-negative
	@rc=0; $(BUILD)/dom-context/webapi-neg-policy > $(BUILD)/dom-context/webapi-neg-policy.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: initial connect policy rejects before network' $(BUILD)/dom-context/webapi-neg-policy.log && grep -q '^FAIL: redirect connect policy rejects before following location' $(BUILD)/dom-context/webapi-neg-policy.log
	@rc=0; $(BUILD)/dom-context/eval-policy-negative > $(BUILD)/dom-context/eval-policy-negative.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: document eval policy covers direct indirect and all Function constructors' $(BUILD)/dom-context/eval-policy-negative.log
test-page-webapi-context: test-page-webapi-context-negctl test-page-webapi-context-fields $(BUILD)/dom-context/webapi
	@$(BUILD)/dom-context/webapi
test-page-webapi-context-asan: test-page-webapi-context
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/dom-context/webapi-asan $(PAGE_WEBAPI_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/dom-context/webapi-asan
ci-host: test-page-webapi-context
