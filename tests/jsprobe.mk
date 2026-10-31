# tests/jsprobe.mk -- the JS probe: what a real page asked this engine for
# and did not get (c/apps/gui/browser/js/runtime/probe.c).
#
#   make probe-site NAME=chatgpt URL=https://chatgpt.com/
#
# builds a whole second tree in $(BUILD)-probe with -DJS_PROBE_TRACE (the
# QuickJS miss/throw hooks and probe.c exist only there; the product build
# compiles neither), boots ONE site with tests/qmp/qmp_site.py, and writes the
# page's [probe] lines -- every platform lookup that found nothing and every
# error the engine created, caught or not, with counts -- to
# $(BUILD)/probe/$(NAME).probe.txt.
#
# An INSTRUMENT, not a gate, like qmp_site.py itself: it exits 0 whether the
# site worked or not. test-probe below is the gate that the instrument works.
ifeq ($(PROBE),1)
JS_CF         += -DJS_PROBE_TRACE
BROWSER_JS_CF += -DJS_PROBE_TRACE
UCFLAGS       += -DJS_PROBE_TRACE
# The browser's own JS objects take a TARGET-SPECIFIC `JS_CF := $(BROWSER_JS_CF)`
# (Makefile, beside BROWSER_JS_OBJ), expanded where it is written -- before this
# fragment is read. The first probe build compiled QuickJS with the hooks and
# the browser without probe.c's body, and printed nothing at all.
$(BROWSER_JS_OBJ): JS_CF += -DJS_PROBE_TRACE
# The same trap a second time: the network frame loader is a BROWSER_OBJ
# whose `UCFLAGS := $(BROWSER_JS_CF)` was also expanded before this line, so
# its js_probe_install(ctx, "frame:") compiled out and every embedded
# document -- the challenge widget's included -- was invisible to the probe.
$(BUILD)/browserobj/c/apps/gui/browser/frame/passive.o: UCFLAGS += -DJS_PROBE_TRACE
endif
PROBE_BUILD := $(BUILD)-probe

.PHONY: probe-build probe-site test-probe
probe-build:
	@$(MAKE) --no-print-directory BUILD=$(PROBE_BUILD) PROBE=1 \
		$(PROBE_BUILD)/logit.iso $(PROBE_BUILD)/disk.img

probe-site: probe-build
	@test -n "$(NAME)" -a -n "$(URL)" || { echo "usage: make probe-site NAME=x URL=https://..."; exit 2; }
	@mkdir -p $(BUILD)/probe
	python3 tests/qmp/qmp_site.py --iso $(PROBE_BUILD)/logit.iso --disk $(PROBE_BUILD)/disk.img \
		--name $(NAME) --url $(URL) --out $(BUILD)/probe/$(NAME).json --shots $(BUILD)/probe --keep
	@grep -a '^\[probe\]' $(BUILD)/probe/$(NAME).serial.txt > $(BUILD)/probe/$(NAME).probe.txt || true
	@grep -q '^\[probe\] installed' $(BUILD)/probe/$(NAME).probe.txt || { \
		echo "probe-site: NO '[probe] installed' line -- the instrument did not run, so zero misses means nothing"; exit 1; }
	@echo "probe: $$(grep -c 'miss ' $(BUILD)/probe/$(NAME).probe.txt) miss lines, $$(grep -c 'throw ' $(BUILD)/probe/$(NAME).probe.txt) throw lines -> $(BUILD)/probe/$(NAME).probe.txt"

# test-jsprobe (host): the instrument itself, on a bare QuickJS with a fake
# platform. Misses on platform singletons and on INSTANCES of platform
# constructors are named; `in` and typeof misses on window are named; a CAUGHT
# "not an object" names the C function that threw it; misses on a script's own
# objects and on arrays are not reported. The control (-DPROBE_NEGCTL_NO_WALK)
# never follows prototypes, so the instance miss loses its attribution -- and
# exactly that check must fail.
JSPROBE_CF  := -DJS_PROBE_TRACE $(JS_INC) -Ic/apps/gui/browser -DCONFIG_VERSION='"host"'
JSPROBE_SRC := tests/unit/jsprobe_test.c c/apps/gui/browser/js/runtime/probe.c $(QJS_SRC)
.PHONY: test-jsprobe test-jsprobe-negctl test-jsprobe-owner-negctl
# The second control: the old reset, which released probe references only for
# the first installer and then all of them through its context. An embedded
# document ("frame:", its own runtime) then keeps objects alive after its
# context is freed -- exactly the one check that must fail, and the test stops
# before running the old wrong-runtime release.
test-jsprobe-owner-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JSPROBE_CF) -DPROBE_NEGCTL_OWNER_ONLY_RESET -o $(BUILD)/jsprobe_owner_negctl $(JSPROBE_SRC) -lm
	@rc=0; ./$(BUILD)/jsprobe_owner_negctl > $(BUILD)/jsprobe_owner_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -eq 1 ] && grep -q "^FAIL an embedded document's reset releases every probe reference into its own runtime" $(BUILD)/jsprobe_owner_negctl.log && \
	   [ "$$(grep -c '^FAIL' $(BUILD)/jsprobe_owner_negctl.log)" = 1 ]; then \
		echo "negative control ok: the owner-only reset leaks an embedded document's references"; \
	else cat $(BUILD)/jsprobe_owner_negctl.log; echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-jsprobe: test-jsprobe-owner-negctl
test-jsprobe-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JSPROBE_CF) -DPROBE_NEGCTL_NO_WALK -o $(BUILD)/jsprobe_negctl $(JSPROBE_SRC) -lm
	@rc=0; $(BUILD)/jsprobe_negctl > $(BUILD)/jsprobe_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 0 ] && grep -q '^FAIL a miss on an INSTANCE is attributed' $(BUILD)/jsprobe_negctl.log && \
	   [ "$$(grep -c '^FAIL' $(BUILD)/jsprobe_negctl.log)" = 1 ]; then \
		echo "negative control ok: without the prototype walk only the instance attribution fails"; \
	else cat $(BUILD)/jsprobe_negctl.log; echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-jsprobe: test-jsprobe-negctl
	@$(CC) -O1 -g -w $(JSPROBE_CF) -o $(BUILD)/jsprobe_test $(JSPROBE_SRC) -lm
	@$(CC) -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -DJS_PROBE_TRACE \
		-isystem third_party/quickjs -Ic/apps/gui/browser \
		c/apps/gui/browser/js/runtime/probe.c tests/unit/jsprobe_test.c
	@rc=0; $(BUILD)/jsprobe_test > $(BUILD)/jsprobe_test.log 2>&1 || rc=$$?; \
		grep -E '^(FAIL|jsprobe:)' $(BUILD)/jsprobe_test.log; exit $$rc
ci-host: test-jsprobe
