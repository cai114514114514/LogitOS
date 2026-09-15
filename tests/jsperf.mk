# --- the JavaScript engine's own measurement + language-coverage targets ----
#
# In its own .mk rather than in the Makefile because several lines share this
# tree and a stale Makefile snapshot has silently deleted other people's
# targets more than once (see the "re-add ... lost to a stale Makefile
# snapshot" commits). A separate file cannot be clobbered by a whole-file
# overwrite. The ONE token this needs inside the main Makefile is
# $(JSBENCH_PACK) in the $(DISK) recipe -- recipes expand at execution time, so
# a variable this fragment defines still reaches it, and if the fragment ever
# goes missing the variable is empty and the disk builds exactly as before.
#
#   make bench-js               compile the real bundles on the HOST
#   make bench-js-os            compile them ON THE MACHINE, over serial
#   make test-js-syntax         the language gate (38 checks)
#   make test-js-syntax-control the same gate against stock QuickJS: MUST FAIL
#   make test-js-dynimport     dynamic import(), which is how a code-split app loads
#   make test-js-stack         err.stack names the error (+ -control)

JSPERF_DIR := tests/fixtures/jsperf
JSPERF_HOST_FIXTURES := $(sort $(wildcard $(JSPERF_DIR)/*.js) $(wildcard $(JSPERF_DIR)/*.mjs))

# Which fixtures ride on the disk image. Not all of them: the guest set is the
# three that answer different questions -- the negative control (42 KB, did not
# compile at all before the lexer fix), a typical webpack chunk (191 KB), and
# the worst case anyone actually ships (1.55 MB, an ES module). Packing the
# other six would add 1.3 MB to every disk image to re-measure the same slope.
JSPERF_GUEST_FIXTURES := $(JSPERF_DIR)/baidu-polyfill.js \
                         $(JSPERF_DIR)/deepseek-6559.js \
                         $(JSPERF_DIR)/kimi-index.mjs
JSPERF_GUEST_PATHS := $(foreach f,$(JSPERF_GUEST_FIXTURES),/jsperf/$(notdir $(f)))

JSBENCH_PACK := $(BUILD)/jsbench.aex:/bin/jsbench \
                $(foreach f,$(JSPERF_GUEST_FIXTURES),$(f):/jsperf/$(notdir $(f)))
$(DISK): $(BUILD)/jsbench.aex $(JSPERF_GUEST_FIXTURES)

# One more file rides along, for test-js-callee-os below. It is packed through
# this variable rather than a new one because $(JSBENCH_PACK) is the ONE token
# this fragment already owns inside the root Makefile's $(DISK) recipe, and
# adding a second would mean editing a file three other lines are editing.
JSCALLEE_FIXTURE := tests/fixtures/jscallee/shapes.js
JSBENCH_PACK     += $(JSCALLEE_FIXTURE):/jscallee/shapes.js
$(DISK): $(JSCALLEE_FIXTURE)

# --- /bin/jsbench: the SAME js_bench.c, built for the machine ---------------
# Same program on both sides on purpose -- the guest differs from the host in
# the two ways that break engines (mini-libc's arena allocator and -msse2), and
# a benchmark that is a different program on each side cannot be compared
# across them. Links like /bin/as: mini-libc + crt0_cli at the common CLI base.
# It gets its own big-arena malloc for the same reason browser.aex does: the
# default 24 MiB arena is not a JS heap, and a 1.55 MB module's bytecode plus
# atoms does not fit in it.
$(BUILD)/jsbenchobj/malloc_big.o: c/apps/libc/src/malloc.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DARENA_SIZE=100663296u -c $< -o $@

$(BUILD)/jsbench.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/tests/unit/js_bench.o \
                      $(BUILD)/jsbenchobj/malloc_big.o $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/jsbench.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/jsbench.crt0c.o $(BUILD)/jsobj/tests/unit/js_bench.o \
	    $(ENGINE_OBJ) $(BUILD)/jsbenchobj/malloc_big.o --end-group

$(BUILD)/jsbench.aex: $(BUILD)/jsbench.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/jsbench.elf $@ jsbench - '?' 150 150 150

# --- bench-js: the host number ---------------------------------------------
BENCH_JS_ITERS ?= 7
$(BUILD)/js_bench: tests/unit/js_bench.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/js_bench.c $(QJS_SRC) -lm

bench-js: $(BUILD)/js_bench
	@$(BUILD)/js_bench -n $(BENCH_JS_ITERS) $(JSPERF_HOST_FIXTURES)

# --- bench-js-os: the number that matters ----------------------------------
# The host number is a sanity check; this is the machine. Runs under TCG, so
# report it as a TCG number and read the median, not the minimum.
BENCH_JS_OS_ITERS ?= 5
bench-js-os: $(ISO) $(DISK)
	@bash tests/unit/js_bench_os.sh $(ISO) $(DISK) $(BENCH_JS_OS_ITERS) $(JSPERF_GUEST_PATHS)

# --- test-js-syntax: what the engine will and will not accept ---------------
# The gate for the vendored-QuickJS patches. Ends by compiling the real 42 KB
# baidu.com polyfill bundle byte for byte, because a reduced test case only
# convinces if the original passes too.
$(BUILD)/js_syntax_test: tests/unit/js_syntax_test.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/js_syntax_test.c $(QJS_SRC) -lm

test-js-syntax: $(BUILD)/js_syntax_test $(BUILD)/js_hash_test
	@$(BUILD)/js_syntax_test $(JSPERF_DIR)/baidu-polyfill.js
	@$(BUILD)/js_hash_test

# The atom-hash patch must compute bit-for-bit the number the code it replaced
# computed -- a merely-as-good hash would split the atom table between the
# narrow and wide string paths and fail nothing until much later. Includes
# quickjs.c directly because hash_string8 is static.
$(BUILD)/js_hash_test: tests/unit/js_hash_test.c third_party/quickjs/quickjs.c
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/js_hash_test.c third_party/quickjs/cutils.c \
	    third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c \
	    third_party/quickjs/libbf.c -lm

# THE NEGATIVE CONTROL. Rebuilds the same gate against a quickjs.c with the
# hex-literal patch mechanically reverted -- one sed, restoring exactly the
# upstream condition -- and REQUIRES it to fail. If this ever passes, the gate
# above is not testing the patch.
$(BUILD)/negctl/quickjs.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@sed 's/BOOL allow_radix_fraction = (radix == 10);/BOOL allow_radix_fraction = TRUE; \/* negative control: upstream QuickJS *\//' $< > $@
	@grep -q 'allow_radix_fraction = TRUE; /\* negative control' $@ || \
	    { echo "FAIL: the negative-control sed matched nothing -- the patch it reverts has moved"; exit 1; }

test-js-syntax-control: $(BUILD)/negctl/quickjs.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_syntax_control \
	    tests/unit/js_syntax_test.c $(BUILD)/negctl/quickjs.c \
	    third_party/quickjs/cutils.c third_party/quickjs/libregexp.c \
	    third_party/quickjs/libunicode.c third_party/quickjs/libbf.c -lm
	@if $(BUILD)/js_syntax_control $(JSPERF_DIR)/baidu-polyfill.js > $(BUILD)/js_syntax_control.log 2>&1; then \
	    echo "FAIL: stock QuickJS passed the syntax gate -- the gate cannot fail, so it proves nothing"; \
	    exit 1; \
	 else \
	    echo "PASS (control): stock QuickJS fails the gate as it must --"; \
	    grep -c '^FAIL:' $(BUILD)/js_syntax_control.log | sed 's/^/  /;s/$$/ checks fail without the patch, including the real baidu polyfill/'; \
	 fi

# --- test-js-propeq-control --------------------------------------------------
# A THIRD control over the same file, over a DIFFERENT patch: LOGIT-PROP-EQ-FIX
# (js_parse_property_name's '='/';' disambiguation for a field/binding literally
# named "get"/"set"/"async" -- see the comment at that marker in quickjs.c and
# the block comment above the checks in js_syntax_test.c). Deleting both marker
# lines (one sed, both occurrences -- get/set and async share the same shape)
# restores exactly the upstream condition and MUST fail exactly 4 checks that
# name a field "get"/"set"/"async" or reduce the real bundles, while the baidu
# polyfill and every other check in the file keep passing -- a control that
# reverted the whole file would not say which patch these four checks are
# measuring.
#
# EXACTLY 4 is asserted: the three "field literally named get/set/async"
# checks plus the get/set-shape value check (the exact ember/preact-kr-
# observable/react-kr-observable reduction). The destructuring-default check
# (`var {get=1,set=2}={}`) is deliberately NOT one of the four and must keep
# PASSING here -- binding-pattern property names are parsed with
# allow_method=FALSE, so they never reach the get/set special case this patch
# touches at all, and a control that counted it would be measuring a second,
# unrelated code path as if it were this one. The sixth new check (`get x
# y(){}` still rejected) must also keep PASSING -- it is what stops "accept
# the token after get/set unconditionally" from satisfying the other four.
$(BUILD)/negctl/quickjs_propeq.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@sed '/LOGIT-PROP-EQ-FIX/d' $< > $@
	@[ "$$(grep -c 'LOGIT-PROP-EQ-FIX' $<)" = "2" ] || \
	    { echo "FAIL: expected exactly 2 LOGIT-PROP-EQ-FIX markers in quickjs.c (get/set + async) -- the patch it reverts has moved"; exit 1; }
	@grep -q 'LOGIT-PROP-EQ-FIX' $@ && \
	    { echo "FAIL: the negative-control sed left a marker behind"; exit 1; } || true

test-js-propeq-control: $(BUILD)/negctl/quickjs_propeq.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_propeq_control \
	    tests/unit/js_syntax_test.c $(BUILD)/negctl/quickjs_propeq.c \
	    third_party/quickjs/cutils.c third_party/quickjs/libregexp.c \
	    third_party/quickjs/libunicode.c third_party/quickjs/libbf.c -lm
	@$(BUILD)/js_propeq_control $(JSPERF_DIR)/baidu-polyfill.js > $(BUILD)/js_propeq_control.log 2>&1; \
	 n=`grep -c '^FAIL:' $(BUILD)/js_propeq_control.log`; \
	 if [ "$$n" != "4" ]; then \
	   echo "FAIL (control): expected exactly 4 checks to fail without the get/set/async '='/';' fix, got $$n"; \
	   grep '^FAIL:' $(BUILD)/js_propeq_control.log; exit 1; \
	 else \
	   echo "PASS (control): reverting LOGIT-PROP-EQ-FIX fails exactly 4 checks --"; \
	   echo "  (the ember/preact-kr-observable/react-kr-observable shape, get with no"; \
	   echo "   initializer, the async field, and the Signal value check) --"; \
	   echo "  everything else, including the real baidu polyfill, the destructuring"; \
	   echo "  default (a different code path), and the still-rejected 'get x y(){}',"; \
	   echo "  keeps passing"; \
	 fi

# --- test-js-dynimport: the half of ES modules a code-split app actually uses -
# kimi.com is 12.77 MB of JavaScript in 134 files; its entry module makes 98
# `import("./chunk.js")` calls and the page carries no import map. Static
# `import` working and `import()` not would load the entry chunk and then
# quietly do nothing. See the header of tests/unit/js_dynimport_test.c.
$(BUILD)/js_dynimport_test: tests/unit/js_dynimport_test.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/js_dynimport_test.c $(QJS_SRC) -lm

test-js-dynimport: $(BUILD)/js_dynimport_test
	@$(BUILD)/js_dynimport_test

# --- test-js-stack: err.stack has to say WHAT went wrong, not only where ----
# Upstream QuickJS builds err.stack from the frames alone, so every error
# reporter written against Chrome -- React's boundary, Sentry, every
# `catch (e) { log(e.stack) }` -- printed a stack with no error in it. The
# expected strings in the test were measured in a real Chrome, not remembered.
$(BUILD)/js_stack_test: tests/unit/js_stack_test.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/js_stack_test.c $(QJS_SRC) -lm

test-js-stack: $(BUILD)/js_stack_test test-js-callee-control test-js-callee-atom-control
	@$(BUILD)/js_stack_test

# THE NEGATIVE CONTROL. One sed turns the prepend off -- restoring exactly
# upstream's frames-only stack -- and this REQUIRES the gate above to fail.
$(BUILD)/negctl/quickjs_nostack.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@sed 's|.*/\* LOGIT-STACK-PREPEND \*/|    if (0) {  /* negative control: upstream, frames only */|' $< > $@
	@grep -q 'negative control: upstream, frames only' $@ || \
	    { echo "FAIL: the negative-control sed matched nothing -- the patch it reverts has moved"; exit 1; }

test-js-stack-control: $(BUILD)/negctl/quickjs_nostack.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_stack_control \
	    tests/unit/js_stack_test.c $(BUILD)/negctl/quickjs_nostack.c \
	    third_party/quickjs/cutils.c third_party/quickjs/libregexp.c \
	    third_party/quickjs/libunicode.c third_party/quickjs/libbf.c -lm
	@if $(BUILD)/js_stack_control > $(BUILD)/js_stack_control.log 2>&1; then \
	    echo "FAIL: a frames-only stack passed the gate -- the gate cannot fail, so it proves nothing"; \
	    exit 1; \
	 else \
	    echo "PASS (control): a frames-only err.stack fails the gate as it must --"; \
	    grep -c '^FAIL:' $(BUILD)/js_stack_control.log | sed 's/^/  /;s/$$/ checks fail without the message line/'; \
	 fi

# --- test-js-callee-control ------------------------------------------------
# The second control over the same file: it reverts the callee NAMING (the
# LOGIT-NAME-CALLEE guard) and leaves the stack prepend alone, so the checks
# that redden are the six about the message and nothing else. Separate from
# test-js-stack-control on purpose -- that one reverts the prepend and reddens
# a different set, and one control turning off both would not say which patch
# either group of checks was measuring.
#
# EXACTLY 21 is asserted (it was 6 until 2026-08-30, when the message learned
# to name eight more call shapes and to print the callee's VALUE). Two checks
# added with the patch must keep PASSING here: a call that works, and an error
# thrown from inside a real function. They are what stops "rewrite every failed
# call's message" from satisfying the other twenty-one.
$(BUILD)/negctl/quickjs_nocallee.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@sed 's|.*/\* LOGIT-NAME-CALLEE \*/|                    if (0)  /* negative control: bare "not a function" */|' $< > $@
	@grep -q 'negative control: bare' $@ || \
	    { echo "FAIL: the negative-control sed matched nothing -- the patch it reverts has moved"; exit 1; }

test-js-callee-control: $(BUILD)/negctl/quickjs_nocallee.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_callee_control \
	    tests/unit/js_stack_test.c $(BUILD)/negctl/quickjs_nocallee.c \
	    third_party/quickjs/cutils.c third_party/quickjs/libregexp.c \
	    third_party/quickjs/libunicode.c third_party/quickjs/libbf.c -lm
	@$(BUILD)/js_callee_control > $(BUILD)/js_callee_control.log 2>&1; \
	 n=`grep -c '^FAIL:' $(BUILD)/js_callee_control.log`; \
	 if [ "$$n" != "21" ]; then \
	   echo "FAIL (control): expected exactly 21 checks to fail without the naming, got $$n"; \
	   grep '^FAIL:' $(BUILD)/js_callee_control.log; exit 1; \
	 else \
	   echo "PASS (control): a bare 'not a function' fails 21 checks as it must"; \
	 fi

# --- test-js-callee-atom-control -------------------------------------------
# The THIRD control over the same file, and the one that measures the
# 2026-08-30 change on its own. test-js-callee-control above reverts the whole
# message; this one reverts ONLY the name recovery -- js_callee_atom() is
# replaced by JS_ATOM_NULL at all three call sites, which is exactly the
# engine Google's page reported against ("not a function (the callee is a
# number)"). What must redden is the naming checks and NOTHING ELSE: the
# value-precision checks and the two "names nothing rather than guessing"
# checks must keep PASSING, because they do not depend on the scan and are
# what stops "print any atom you can find" from satisfying the rest.
#
# EXACTLY 13 is asserted -- the thirteen shapes the scan names.
$(BUILD)/negctl/quickjs_noatom.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@sed 's|.*/\* LOGIT-CALLEE-ATOM \*/|                            JS_ATOM_NULL);  /* negative control: no name recovered */|' $< > $@
	@grep -q 'negative control: no name recovered' $@ || \
	    { echo "FAIL: the negative-control sed matched nothing -- the patch it reverts has moved"; exit 1; }

test-js-callee-atom-control: $(BUILD)/negctl/quickjs_noatom.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/js_callee_atom_control \
	    tests/unit/js_stack_test.c $(BUILD)/negctl/quickjs_noatom.c \
	    third_party/quickjs/cutils.c third_party/quickjs/libregexp.c \
	    third_party/quickjs/libunicode.c third_party/quickjs/libbf.c -lm
	@$(BUILD)/js_callee_atom_control > $(BUILD)/js_callee_atom_control.log 2>&1; \
	 n=`grep -c '^FAIL:' $(BUILD)/js_callee_atom_control.log`; \
	 if [ "$$n" != "13" ]; then \
	   echo "FAIL (control): expected exactly 13 naming checks to fail without js_callee_atom(), got $$n"; \
	   grep '^FAIL:' $(BUILD)/js_callee_atom_control.log; exit 1; \
	 else \
	   echo "PASS (control): without the bytecode scan, 13 call shapes go back to naming nothing"; \
	 fi

# --- test-js-callee-os: the same question, ON THE MACHINE -------------------
# THE HOST BINARY IS NOT THE BROWSER, and this message is built out of a
# bytecode scan and snprintf -- neither of which is the same code on the two
# sides. js_stack_test links $(QJS_SRC) for arm64/darwin against the system
# libc; /bin/jssem links $(ENGINE_OBJ), the literal object files browser.elf
# links, for x86_64-elf against mini-libc. Rule 1: a survey run in the probe
# measures the probe.
#
# Rides on /bin/jssem rather than a binary of its own because jssem already
# does exactly this job -- eval a file, install `print`, and nothing else.
JSCALLEE_LOG ?= $(BUILD)/js-callee-os.log
test-js-callee-os: $(ISO) $(DISK)
	@JSSEM_GUEST_LOG=$(JSCALLEE_LOG) bash tests/jssem/run-guest.sh \
	    $(ISO) $(DISK) /jscallee/shapes.js >/dev/null
	@grep -E '^(FAIL|CALLEE-OS-RESULT)' $(JSCALLEE_LOG) || true
	@r=`grep -o 'CALLEE-OS-RESULT ok=[0-9]* fail=[0-9]*' $(JSCALLEE_LOG) | tail -1`; \
	 case "$$r" in \
	   "") echo "FAIL: the guest never printed a verdict -- transcript at $(JSCALLEE_LOG)"; exit 1;; \
	   *fail=0) echo "PASS: $$r  (browser engine, in the guest)";; \
	   *) echo "FAIL: $$r -- transcript at $(JSCALLEE_LOG)"; exit 1;; \
	 esac
.PHONY: bench-js bench-js-os test-js-syntax test-js-syntax-control test-js-propeq-control test-js-dynimport test-js-stack test-js-stack-control test-js-callee-control test-js-callee-atom-control test-js-callee-os

# Both controls are PREREQUISITES of test-js-stack, not siblings on this line:
# naming a control on ci-host: satisfies the stranded-control audit and still
# runs it never, which is worse than being stranded because it looks fixed.
ci-host: test-js-stack
ci-boot: test-js-callee-os
# Standards parsing regression exposed by a live editor module.
include tests/regexp_class_escape.mk
