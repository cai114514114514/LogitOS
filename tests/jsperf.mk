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

.PHONY: bench-js bench-js-os test-js-syntax test-js-syntax-control
