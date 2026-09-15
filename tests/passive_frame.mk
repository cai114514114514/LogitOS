PASSIVE_FRAME_BASE = $(sort $(filter-out tests/unit/navigation_base_test.c,$(NAVIGATION_BASE_SRC)) c/apps/browser/js_url.c c/apps/browser/passive_frame.c c/apps/browser/iframe_policy.c) tests/unit/passive_frame_test.c
PASSIVE_FRAME_SRC = $(PASSIVE_FRAME_BASE) $(filter-out $(PASSIVE_FRAME_BASE),$(IMGCHK_SRC))
PASSIVE_FRAME_DEPS = $(PASSIVE_FRAME_SRC) $(RUNTIME_SCROLL_DEPS) c/apps/browser/embedded_window.inc tests/passive_frame.mk
PASSIVE_FRAME_DIR = $(BUILD)/passive-frame
$(PASSIVE_FRAME_DIR)/current: $(PASSIVE_FRAME_DEPS)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -o $@ $(PASSIVE_FRAME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PASSIVE_FRAME_DIR)/old: $(PASSIVE_FRAME_DEPS)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -DBROWSER_PASSIVE_NO_PAINT -o $@ $(PASSIVE_FRAME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-passive-frame test-passive-frame-negctl
test-passive-frame-negctl: $(PASSIVE_FRAME_DIR)/old
	@rc=0; $(PASSIVE_FRAME_DIR)/old paint > $(PASSIVE_FRAME_DIR)/old.log 2>&1 || rc=$$?; tail -18 $(PASSIVE_FRAME_DIR)/old.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(PASSIVE_FRAME_DIR)/old.log)" -eq 2 && grep -q '^FAIL: actual child document text reaches embedded paint' $(PASSIVE_FRAME_DIR)/old.log && grep -q '^FAIL: actual decoded child image reaches embedded paint' $(PASSIVE_FRAME_DIR)/old.log
test-passive-frame: test-passive-frame-negctl $(PASSIVE_FRAME_DIR)/current
	@for mode in paint allowed-csp xfo sandbox parent-csp unknown geometry meta-tighten remove bad-mime base-blocked static; do \
	 rc=0; $(PASSIVE_FRAME_DIR)/current $$mode > $(PASSIVE_FRAME_DIR)/$$mode.log 2>&1 || rc=$$?; tail -18 $(PASSIVE_FRAME_DIR)/$$mode.log; test $$rc -eq 0 || exit $$rc; done
ci-host: test-passive-frame

$(PASSIVE_FRAME_DIR)/active-neg-%: $(PASSIVE_FRAME_DEPS)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	@case '$*' in input) flag=PF_TEST_NO_INPUT;; messages) flag=PF_TEST_NO_MESSAGES;; size) flag=PF_TEST_SMALL_SCRIPT;; wake) flag=PF_TEST_NO_MESSAGE_WAKE;; *) exit 2;; esac; \
	 $(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -D$$flag -o $@ $(PASSIVE_FRAME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-active-frame-negctl test-active-frame
test-active-frame-negctl: $(PASSIVE_FRAME_DIR)/active-neg-input $(PASSIVE_FRAME_DIR)/active-neg-messages $(PASSIVE_FRAME_DIR)/active-neg-size $(PASSIVE_FRAME_DIR)/active-neg-wake
	@for kind in input messages; do rc=0; $(PASSIVE_FRAME_DIR)/active-neg-$$kind active > $(PASSIVE_FRAME_DIR)/active-neg-$$kind.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q '^FAIL: native child click and bidirectional message repaint embedded pixels' $(PASSIVE_FRAME_DIR)/active-neg-$$kind.log || exit 1; done
	@rc=0; $(PASSIVE_FRAME_DIR)/active-neg-size active-large > $(PASSIVE_FRAME_DIR)/active-neg-size.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: native child click and bidirectional message repaint embedded pixels' $(PASSIVE_FRAME_DIR)/active-neg-size.log
	@rc=0; $(PASSIVE_FRAME_DIR)/active-neg-wake active > $(PASSIVE_FRAME_DIR)/active-neg-wake.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: queued window messages wake an otherwise idle browser' $(PASSIVE_FRAME_DIR)/active-neg-wake.log
test-active-frame: test-active-frame-negctl $(PASSIVE_FRAME_DIR)/current
	@$(PASSIVE_FRAME_DIR)/current active
	@$(PASSIVE_FRAME_DIR)/current active-large
ci-host: test-active-frame

# Keep the old no-native-module fixture usable, and explicitly exercise the
# shipping Window packet path with a transferred port and actual child paint.
FRAME_PORT_SRC = $(PASSIVE_FRAME_SRC) c/apps/browser/js_ports.c
FRAME_PORT_DEP = $(PASSIVE_FRAME_DEPS) c/apps/browser/js_ports.c c/apps/browser/js_ports.h
$(PASSIVE_FRAME_DIR)/ports: $(FRAME_PORT_DEP)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -o $@ $(FRAME_PORT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PASSIVE_FRAME_DIR)/ports-no-messages: $(FRAME_PORT_DEP)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -DPF_TEST_NO_MESSAGES -o $@ $(FRAME_PORT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-ports test-frame-ports-negctl test-frame-ports-san
test-frame-ports-negctl: $(PASSIVE_FRAME_DIR)/ports-no-messages
	@rc=0; $(PASSIVE_FRAME_DIR)/ports-no-messages active-ports > $(PASSIVE_FRAME_DIR)/ports-no-messages.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: native child click and bidirectional message repaint embedded pixels' $(PASSIVE_FRAME_DIR)/ports-no-messages.log
test-frame-ports: test-frame-ports-negctl test-native-ports $(PASSIVE_FRAME_DIR)/ports
	@$(PASSIVE_FRAME_DIR)/ports active-ports
	@$(PASSIVE_FRAME_DIR)/ports active
test-frame-ports-san: test-frame-ports
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(PASSIVE_FRAME_DIR)/ports-san $(FRAME_PORT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(PASSIVE_FRAME_DIR)/ports-san active-ports
ci-host: test-frame-ports

$(PASSIVE_FRAME_DIR)/ports-lost-tab-policy: $(FRAME_PORT_DEP)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -DPF_TEST_LOST_TAB_POLICY -o $@ $(FRAME_PORT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-frame-tab-policy test-frame-tab-policy-negctl test-frame-tab-policy-san
test-frame-tab-policy-negctl: $(PASSIVE_FRAME_DIR)/ports-lost-tab-policy
	@rc=0; $(PASSIVE_FRAME_DIR)/ports-lost-tab-policy active-ports-restore > $(PASSIVE_FRAME_DIR)/ports-lost-tab-policy.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: native child click and bidirectional message repaint embedded pixels' $(PASSIVE_FRAME_DIR)/ports-lost-tab-policy.log
test-frame-tab-policy: test-frame-tab-policy-negctl $(PASSIVE_FRAME_DIR)/ports
	@$(PASSIVE_FRAME_DIR)/ports active-ports-restore
test-frame-tab-policy-san: test-frame-tab-policy
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $(PASSIVE_FRAME_DIR)/tab-policy-san $(FRAME_PORT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(PASSIVE_FRAME_DIR)/tab-policy-san active-ports-restore
ci-host: test-frame-tab-policy

PASSIVE_TRANSPORT_SRC = $(filter-out tests/unit/range_test.c,$(RANGE_SRC)) tests/unit/passive_frame_transport_test.c c/net/http/cookies.c
.PHONY: test-passive-frame-transport test-passive-frame-transport-negctl
test-passive-frame-transport-negctl:
	@mkdir -p $(PASSIVE_FRAME_DIR)
	@$(CC) -O2 -w $(RANGE_INC) -DBFETCH_EMBED_OWNER_ONLY -o $(PASSIVE_FRAME_DIR)/transport-old $(PASSIVE_TRANSPORT_SRC)
	@rc=0; $(PASSIVE_FRAME_DIR)/transport-old > $(PASSIVE_FRAME_DIR)/transport-old.log 2>&1 || rc=$$?; tail -12 $(PASSIVE_FRAME_DIR)/transport-old.log; test $$rc -eq 1 && grep -q '23 checks, 4 failures' $(PASSIVE_FRAME_DIR)/transport-old.log
test-passive-frame-transport: test-passive-frame-transport-negctl
	@$(CC) -O2 -w $(RANGE_INC) -o $(PASSIVE_FRAME_DIR)/transport $(PASSIVE_TRANSPORT_SRC)
	@$(PASSIVE_FRAME_DIR)/transport
test-passive-frame: test-passive-frame-transport

# This is the same full browser fixture under instrumentation. Darwin has no
# leak sanitizer; ASan/UBSan still validates child removal and teardown access.
$(PASSIVE_FRAME_DIR)/san: $(PASSIVE_FRAME_DEPS)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(PASSIVE_FRAME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-passive-frame-san
test-passive-frame-san: test-passive-frame test-active-frame $(PASSIVE_FRAME_DIR)/san
	@for mode in paint geometry meta-tighten remove base-blocked active active-large; do \
	 ASAN_OPTIONS=detect_leaks=0 $(PASSIVE_FRAME_DIR)/san $$mode > $(PASSIVE_FRAME_DIR)/san-$$mode.log 2>&1 || { tail -60 $(PASSIVE_FRAME_DIR)/san-$$mode.log; exit 1; }; tail -2 $(PASSIVE_FRAME_DIR)/san-$$mode.log; done

# Device negative: same linked browser, replacing only the nested paint call.
# The loader and child network requests remain live so a blank result cannot
# be explained by a fixture that failed to load its child document.
$(PASSIVE_FRAME_DIR)/guest-paint-old.o: c/apps/browser/browser_paint.c $(BROWSER_INTERFACE_DEPS)
	@mkdir -p $(PASSIVE_FRAME_DIR)
	$(CC) $(UCFLAGS) $(CSS_INC) -DBROWSER_PASSIVE_NO_PAINT -c $< -o $@
$(PASSIVE_FRAME_DIR)/guest-old.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o $(PASSIVE_FRAME_DIR)/guest-paint-old.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(filter-out $(BUILD)/browserobj/c/apps/browser/browser_paint.o,$(BROWSER_OBJ)) $(PASSIVE_FRAME_DIR)/guest-paint-old.o $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(PASSIVE_FRAME_DIR)/guest-old.aex: $(PASSIVE_FRAME_DIR)/guest-old.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Browser - 'B' 120 130 240 --stack-pages 2048
# Disk paths must be explicit private images prepared by the caller; these
# targets never package, write, or launch the user's default disk.
PASSIVE_FRAME_GUEST_OUT ?= $(PASSIVE_FRAME_DIR)
.PHONY: test-passive-frame-guest test-passive-frame-guest-negctl
test-passive-frame-guest-negctl: $(PASSIVE_FRAME_DIR)/guest-old.aex
	@test -n "$(PASSIVE_FRAME_GUEST_OLD_DISK)" -a -n "$(PASSIVE_FRAME_GUEST_ISO)"
	python3 tests/qmp/passive_frame_guest.py --iso $(PASSIVE_FRAME_GUEST_ISO) --disk $(PASSIVE_FRAME_GUEST_OLD_DISK) --out $(PASSIVE_FRAME_GUEST_OUT)/guest-old --expect-no-paint
test-passive-frame-guest: test-passive-frame-guest-negctl
	@test -n "$(PASSIVE_FRAME_GUEST_DISK)"
	python3 tests/qmp/passive_frame_guest.py --iso $(PASSIVE_FRAME_GUEST_ISO) --disk $(PASSIVE_FRAME_GUEST_DISK) --out $(PASSIVE_FRAME_GUEST_OUT)/guest-current
