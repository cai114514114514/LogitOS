# ============================================================================
# WebAssembly MVP: the binary decoder and the validator  (c/lib/wasm)
#
#   make wasm-fetch           get the official spec test suite (and wabt)
#   make test-wasm            built-in checks + the whole spec corpus
#   make test-wasm-negctl     TWO injected defects; each MUST redden the gate
#   make test-wasm-asan       the same corpus under ASan + UBSan
#   make test-wasm-clang      C compiled to wasm32 by clang, and validated
#   make test-wasm-clang-negctl   the same modules truncated; each MUST fail
#
# WHY THIS EXISTS AND WHAT THE ARGUMENT IS NOT.  It is not web compatibility.
# Measured over tests/fixtures: the string "WebAssembly" appears in TWO of 192
# distinct real bundles, both behind a working feature test, and ".wasm"
# appears zero times in the whole corpus.  The argument is CLAUDE.md
# structural gap #2 -- no PT_INTERP, no PT_DYNAMIC, no ET_DYN, zero
# relocations, no dlopen, one PDPT entry of user address space with every link
# base assigned by hand in the Makefile.  A wasm module has no relocations and
# no link base; every address in it is an index into its own tables or an
# offset into its own linear memory.  It is the one code format for which that
# gap does not exist.  And it is why this is an interpreter's front end rather
# than a port: V8 reserves 4-8 GiB up front so a bounds check becomes a guard
# page, and that reservation is gap #2 verbatim.
#
# THE ORACLE IS SOMEBODY ELSE'S.  Nothing in the corpus was written here.  The
# spec suite's thousands of MALFORMED and INVALID modules are pre-written
# negative controls by people who are not us, and the number this gate exists
# to print is WRONGLY ACCEPTED -- a validator that accepts a malformed module
# is the only failure in this phase that everything downstream inherits.  It
# is never folded into a percentage and it must be zero.
#
# THE ACCOUNTING REFUSES TO PASS FOR THE WRONG REASON (rule 5).  The live
# suite runs several proposals ahead of MVP, so a module can be refused
# because it is memory64 rather than because it is malformed.  Those land in
# a separate `out-of-scope` column and are explicitly NOT counted as
# rejections.  WASM_E_NOMEM / WASM_E_LIMIT land in an APPARATUS column and
# fail the gate outright: without that, a zero-byte arena would report every
# malformed case as correctly rejected.
#
# TWO NEGATIVE CONTROLS, ONE PER HALF OF THE FILE, and both are the
# implementation that LOOKS RIGHT rather than a deletion:
#
#   WASM_NEGCTL_LEB    drop the LEB128 length and range checks.  Every valid
#                      module still decodes -- the accumulate is unchanged --
#                      and the "integer representation too long" / "integer
#                      too large" families of binary-leb128.wast are silently
#                      accepted.  Measured 2026-08-30: 45 malformed accepted.
#   WASM_NEGCTL_STACK  delete ONE line: the check that a block ends with its
#                      operand stack back at the frame's height.  Every
#                      arithmetic rule, every branch rule and every valid
#                      module is untouched; leftover operands stop being an
#                      error.  Measured 2026-08-30: 93 invalid accepted.
#
# BOTH NUMBERS ROSE WHEN THE CORPUS WAS FIXED (44->45, 55->93), which is the
# argument for having watched them at all: wast2json refuses a whole .wast
# file over one line of syntax it does not know, 54 of 257 files went that way
# SILENTLY, and the corpus printed a large case count regardless.  Recovering
# them (convert the parseable prefix, sweep the tail for `(module binary)`)
# added 574 cases -- and the first run of the recovered corpus found a real
# validator bug, WASM_VT_UNKNOWN and "no result" being the same number.
#
# `test-wasm: test-wasm-negctl` on purpose.  CLAUDE.md: 61 controls in this
# tree are stranded because NOT_CI drops every test-*-negctl on the theory
# that its positive counterpart runs it, and nothing checked that.  The fix is
# this one line.
#
# THE CORPUS IS OPTIONAL AND THE GATE SAYS SO OUT LOUD.  With no
# $(WASM_CORPUS) the binary runs its built-in checks, prints the command that
# would settle it, and exits 0 -- it never reports a suite it did not run.
# The built-in set is deliberately small but is chosen so that BOTH negative
# controls still bite with no corpus at all; a control that depends on a
# download stops controlling anything the day the download fails.
# ============================================================================
.PHONY: test-wasm test-wasm-negctl test-wasm-asan test-wasm-clang
.PHONY: test-wasm-clang-negctl wasm-fetch wasm-corpus

WASM_SRC      := c/lib/wasm/wasm_parse.c c/lib/wasm/wasm_valid.c
WASM_HDR      := c/lib/wasm/wasm.h c/lib/wasm/wasm_int.h
WASM_CF       := -std=c99 -O1 -g -Wall -Wextra -Ic/lib/wasm
WASM_SUITE    ?= build/wasm/testsuite
WASM_WABT     ?= build/wasm/wabt/bin/wast2json
WASM_CORPUS   ?= $(BUILD)/wasm-corpus
WASM_BASELINE := tests/wasm-spec.baseline
WASM_REV      := $(shell cat tools/wasm_revision.txt 2>/dev/null)

# tests/sysroot.mk's convention: look where Homebrew puts an LLVM that is not
# the system one.  The SYSTEM clang on this host has no wasm backend at all
# ("No available targets are compatible with triple wasm32"); the Homebrew one
# does.  Absent, test-wasm-clang SKIPS LOUDLY naming the install command.
WASM_CLANG ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang \
                                     /usr/local/opt/llvm/bin/clang))

$(BUILD)/wasm_test: tests/unit/wasm_test.c $(WASM_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_CF) -o $@ tests/unit/wasm_test.c $(WASM_SRC)

$(BUILD)/wasm_test_negleb: tests/unit/wasm_test.c $(WASM_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_CF) -DWASM_NEGCTL_LEB -o $@ tests/unit/wasm_test.c $(WASM_SRC)

$(BUILD)/wasm_test_negstack: tests/unit/wasm_test.c $(WASM_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_CF) -DWASM_NEGCTL_STACK -o $@ tests/unit/wasm_test.c $(WASM_SRC)

# ---- the corpus ------------------------------------------------------------
# Rebuilt whenever the suite is present.  wasm_corpus.py picks its own mode:
# wast2json when wabt is available (adds the assert_invalid cases written in
# the TEXT format, which is most of them -- block.wast alone is 155), and a
# no-dependency extraction of every `(module binary "...")` form otherwise,
# which is essentially the whole MALFORMED half.
wasm-corpus:
	@if [ -d "$(WASM_SUITE)" ]; then \
	   WAST2JSON="$(WASM_WABT)" python3 tools/wasm_corpus.py "$(WASM_SUITE)" "$(WASM_CORPUS)"; \
	 else \
	   echo "wasm-corpus: $(WASM_SUITE) is absent -- run: make wasm-fetch"; \
	 fi

wasm-fetch:
	@mkdir -p build/wasm
	@if [ -d "$(WASM_SUITE)/.git" ]; then \
	   echo "wasm-fetch: $(WASM_SUITE) already present"; \
	 else \
	   echo "wasm-fetch: cloning the WebAssembly spec test suite"; \
	   git clone --quiet https://github.com/WebAssembly/testsuite.git "$(WASM_SUITE)" || \
	     { echo "wasm-fetch: clone FAILED -- no corpus, and test-wasm will say so"; exit 1; }; \
	 fi
	@if [ -n "$(WASM_REV)" ]; then \
	   (cd "$(WASM_SUITE)" && git fetch --quiet origin $(WASM_REV) 2>/dev/null; \
	    git checkout --quiet $(WASM_REV)) && \
	   echo "wasm-fetch: testsuite pinned at $(WASM_REV)"; \
	 fi
	@if [ -x "$(WASM_WABT)" ]; then \
	   echo "wasm-fetch: wabt already present"; \
	 else \
	   echo "wasm-fetch: fetching wabt (OPTIONAL -- without it the corpus is"; \
	   echo "            binary-only: the malformed half, which is the control,"; \
	   echo "            but not the text-format assert_invalid cases)"; \
	   ( cd build/wasm && \
	     curl -sSLo wabt.tar.gz https://github.com/WebAssembly/wabt/releases/download/1.0.36/wabt-1.0.36-macos-14.tar.gz && \
	     tar xzf wabt.tar.gz && rm -rf wabt && mv wabt-1.0.36 wabt ) || \
	     echo "wasm-fetch: wabt not fetched; continuing binary-only"; \
	 fi
	@$(MAKE) --no-print-directory wasm-corpus

# ---- the gate --------------------------------------------------------------
test-wasm: test-wasm-negctl $(BUILD)/wasm_test
	@$(MAKE) --no-print-directory wasm-corpus 2>/dev/null || true
	@if [ -f "$(WASM_CORPUS)/manifest.tsv" ]; then \
	   $(BUILD)/wasm_test "$(WASM_CORPUS)" "$(WASM_BASELINE)"; \
	 else \
	   $(BUILD)/wasm_test; \
	 fi

# Both controls must FAIL.  Watched, not assumed: the recipe prints the
# WRONGLY-ACCEPTED count each one produced, so a control that stopped
# controlling anything is visible as a zero rather than as a silent pass.
test-wasm-negctl: $(BUILD)/wasm_test_negleb $(BUILD)/wasm_test_negstack
	@$(MAKE) --no-print-directory wasm-corpus 2>/dev/null >/dev/null || true
	@ok=1; \
	 for v in negleb negstack; do \
	   if [ -f "$(WASM_CORPUS)/manifest.tsv" ]; then \
	     out=$$($(BUILD)/wasm_test_$$v "$(WASM_CORPUS)" "$(WASM_BASELINE)" 2>&1); rc=$$?; \
	   else \
	     out=$$($(BUILD)/wasm_test_$$v 2>&1); rc=$$?; \
	   fi; \
	   n=$$(printf '%s\n' "$$out" | sed -n 's/^WRONGLY ACCEPTED: \([0-9]*\).*/\1/p'); \
	   b=$$(printf '%s\n' "$$out" | grep -c '^  FAIL '); \
	   if [ $$rc -eq 0 ]; then \
	     echo "wasm-negctl: $$v PASSED and must not have -- the control is dead"; ok=0; \
	   else \
	     echo "wasm-negctl: $$v reddens the gate (wrongly accepted: $${n:-0}, built-in failures: $$b)"; \
	   fi; \
	 done; \
	 [ $$ok = 1 ]

test-wasm-asan: tests/unit/wasm_test.c $(WASM_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_CF) -fsanitize=address,undefined -fno-sanitize-recover=all \
	   -o $(BUILD)/wasm_test_asan tests/unit/wasm_test.c $(WASM_SRC)
	@$(MAKE) --no-print-directory wasm-corpus 2>/dev/null || true
	@if [ -f "$(WASM_CORPUS)/manifest.tsv" ]; then \
	   $(BUILD)/wasm_test_asan "$(WASM_CORPUS)" "$(WASM_BASELINE)"; \
	 else \
	   $(BUILD)/wasm_test_asan; \
	 fi

# ---- the second oracle: a real toolchain -----------------------------------
# The spec suite is hand-written by people who are testing a specification.
# clang is a compiler emitting whatever its backend feels like, and the two
# distributions do not overlap much: large function bodies, dozens of locals,
# dense br_table, every load and store width, data segments.  Free, because
# the input is C we already know how to write.
WASM_FIX := $(wildcard tests/fixtures/wasm/*.c)

# The compile step is its own target so that BOTH the gate and its control can
# depend on it.  Written the other way round -- `test-wasm-clang-negctl:
# test-wasm-clang` -- the control runs the gate instead of the gate running the
# control, NOT_CI drops the control, and it runs never.  That is exactly the
# stranded shape CLAUDE.md counts 57 of, and tools/audit_tests.py named this
# one on the day it was written.
.PHONY: wasm-clang-build
wasm-clang-build:
	@if [ -z "$(WASM_CLANG)" ]; then \
	   echo "test-wasm-clang: SKIPPED -- no clang with a wasm32 backend."; \
	   echo "  The SYSTEM clang on this host answers 'No available targets are"; \
	   echo "  compatible with triple wasm32'.  Settle it with:  brew install llvm"; \
	   echo "  (this gate looks in /opt/homebrew/opt/llvm/bin and"; \
	   echo "   /usr/local/opt/llvm/bin, the same two places tests/sysroot.mk does)"; \
	   exit 0; \
	 fi
	@mkdir -p $(BUILD)/wasm-clang
	@rm -f $(BUILD)/wasm-clang/*.wasm $(BUILD)/wasm-clang/manifest.tsv
	@printf '# mode\tclang-wasm32 (%s)\n' "$(WASM_CLANG)" > $(BUILD)/wasm-clang/manifest.tsv
	@for f in $(WASM_FIX); do \
	   b=$$(basename $$f .c); \
	   $(WASM_CLANG) --target=wasm32 -mcpu=mvp -O2 -nostdlib -ffreestanding \
	      -Wl,--no-entry -Wl,--export-all -o $(BUILD)/wasm-clang/$$b.wasm $$f || exit 1; \
	   printf 'accept\t%s.wasm\t%s:1\t\n' "$$b" "$$b.c" >> $(BUILD)/wasm-clang/manifest.tsv; \
	 done

test-wasm-clang: test-wasm-clang-negctl $(BUILD)/wasm_test wasm-clang-build
	@if [ ! -f $(BUILD)/wasm-clang/manifest.tsv ]; then exit 0; fi
	@$(BUILD)/wasm_test $(BUILD)/wasm-clang

# The control for the gate above, and it is a real question rather than a
# formality: those modules are the only ones in this tree that a compiler
# produced, so "we accept them" is worth nothing unless we also refuse them
# when they are damaged.  Every prefix at 25/50/75% must be rejected.
test-wasm-clang-negctl: $(BUILD)/wasm_test wasm-clang-build
	@if [ -z "$(WASM_CLANG)" ] || [ ! -f $(BUILD)/wasm-clang/manifest.tsv ]; then \
	   echo "test-wasm-clang-negctl: SKIPPED with its positive counterpart"; exit 0; fi
	@mkdir -p $(BUILD)/wasm-clang-cut
	@rm -f $(BUILD)/wasm-clang-cut/*.wasm $(BUILD)/wasm-clang-cut/manifest.tsv
	@printf '# mode\tclang-wasm32 truncated\n' > $(BUILD)/wasm-clang-cut/manifest.tsv
	@for w in $(BUILD)/wasm-clang/*.wasm; do \
	   b=$$(basename $$w .wasm); sz=$$(wc -c < $$w); \
	   for pct in 25 50 75; do \
	     head -c $$((sz * pct / 100)) $$w > $(BUILD)/wasm-clang-cut/$$b-$$pct.wasm; \
	     printf 'malformed\t%s-%s.wasm\t%s.c:%s%%\t\n' "$$b" "$$pct" "$$b" "$$pct" \
	       >> $(BUILD)/wasm-clang-cut/manifest.tsv; \
	   done; \
	 done
	@$(BUILD)/wasm_test $(BUILD)/wasm-clang-cut

# ============================================================================
# THE INTERPRETER AND THE JAVASCRIPT API
#
#   make test-wasm-exec        the interpreter: built-in checks + the spec
#                              suite's assert_return / assert_trap corpus
#   make test-wasm-exec-negctl ONE opcode's immediate decoding broken
#   make test-wasm-js          the WebAssembly JavaScript API, RUN not linked
#   make test-wasm-js-negctl   THREE injected defects; each MUST redden it
#   make test-wasm-js-asan     the same, under ASan + UBSan
#   make test-wasm-modules     the embedded .wasm bytes still match their .wat
#
# WIRING THESE WAS THE FIRST FINDING OF THE SESSION THAT ADDED THEM.
# c/lib/wasm/wasm_exec.c (1,322 lines), tests/unit/wasm_exec_test.c (947) and
# tests/fixtures/wasmdiff/ were all written, committed to the working tree, and
# named by NO make target: WASM_SRC above is wasm_parse.c + wasm_valid.c, so
# the interpreter was a translation unit nothing built and nothing ran.  That
# is CLAUDE.md rule 4 exactly -- "a gate nobody runs is a gate that rots" --
# and it had never been run even once.  Building it by hand the first time:
# 14,020 of 14,020 spec assertions passed, 0 wrong, 0 apparatus.  A correct
# interpreter nobody could tell was correct.
#
# THE JS GATE'S ONE JOB IS TO NOT BE THE 531/11152 SHAPE.  Every check in
# tests/unit/wasm_js_test.c is JavaScript evaluated in a real QuickJS context
# after js_wasm_install() has run, so there is no path a no-op implementation
# survives -- and the three controls below are the proof rather than the claim:
#
#   JS_WASM_NEGCTL_I64     take i64 through a double instead of BigInt, so a
#                          Number is accepted "helpfully".  Every small value
#                          still round-trips.  3 checks redden.
#   JS_WASM_NEGCTL_DETACH  on a grow, drop our cached ArrayBuffer instead of
#                          DETACHING it.  Reads as an optimisation; every
#                          check about current contents still passes; what it
#                          loses is the view a page took before the grow,
#                          which keeps reading the pre-grow block.  3 redden.
#   WASM_NEGCTL_MEMP       drop wasm_exec.c's moved-base write-back.  2 redden.
#
# AND THE THIRD ONE IS WHERE THIS FRAGMENT EARNED ITS KEEP.  It was first
# gated by the JavaScript check "a grow INSIDE wasm keeps an imported Memory
# coherent", which passes -- and passes just as green with the write-back
# deleted, because js_wasm.c's jw_mem_live asks the INSTANCE for the live base
# rather than trusting its own copy, so the JS path is immune by construction.
# Measured at 57/57 with the defect in.  A control that cannot be watched
# failing is worse than no control, so the check moved to the C level, where
# the contract actually lives: a host holding struct wasm_hostimport and
# reading .mem after the module grew itself.  It reddens now, and the defect
# it names was measured in this tree before it was fixed.
# ============================================================================
.PHONY: test-wasm-exec test-wasm-exec-negctl test-wasm-js test-wasm-js-negctl
.PHONY: test-wasm-js-asan test-wasm-modules

WASM_EXEC_SRC := $(WASM_SRC) c/lib/wasm/wasm_exec.c
# -fno-math-errno: wasm_exec.h says why.  Without it clang emits a libcall to
# sqrt() for the errno path on --target=x86_64-elf (measured; darwin/arm64
# does not), which is a libc dependency in a file whose whole claim is that it
# has none.
WASM_EXEC_CF  := $(WASM_CF) -fno-math-errno

$(BUILD)/wasm_exec_test: tests/unit/wasm_exec_test.c $(WASM_EXEC_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_EXEC_CF) -o $@ tests/unit/wasm_exec_test.c $(WASM_EXEC_SRC)

$(BUILD)/wasm_exec_negimm: tests/unit/wasm_exec_test.c $(WASM_EXEC_SRC) $(WASM_HDR)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_EXEC_CF) -DWASM_NEGCTL_IMM -o $@ tests/unit/wasm_exec_test.c \
	   $(WASM_EXEC_SRC)

test-wasm-exec: test-wasm-exec-negctl $(BUILD)/wasm_exec_test
	@$(MAKE) --no-print-directory wasm-corpus 2>/dev/null >/dev/null || true
	@if [ -f "$(WASM_CORPUS)/runs.tsv" ]; then \
	   $(BUILD)/wasm_exec_test "$(WASM_CORPUS)"; \
	 else \
	   $(BUILD)/wasm_exec_test; \
	 fi

test-wasm-exec-negctl: $(BUILD)/wasm_exec_negimm
	@$(MAKE) --no-print-directory wasm-corpus 2>/dev/null >/dev/null || true
	@if [ -f "$(WASM_CORPUS)/runs.tsv" ]; then \
	   out=$$($(BUILD)/wasm_exec_negimm "$(WASM_CORPUS)" 2>&1); rc=$$?; \
	 else \
	   out=$$($(BUILD)/wasm_exec_negimm 2>&1); rc=$$?; \
	 fi; \
	 b=$$(printf '%s\n' "$$out" | grep -c '^  FAIL '); \
	 if [ $$rc -eq 0 ]; then \
	   echo "wasm-exec-negctl: WASM_NEGCTL_IMM PASSED and must not have -- the control is dead"; \
	   exit 1; \
	 fi; \
	 echo "wasm-exec-negctl: WASM_NEGCTL_IMM reddens the gate (built-in failures: $$b)"

# ---- the JavaScript API ----------------------------------------------------
# The browser's own QuickJS, and c/lib/wasm.  No LibCSS, no DOM: this gate is
# the API and the interpreter and nothing else, so a failure here cannot be
# somebody else's.
# NOTE the absence of $(WASM_EXEC_SRC): js_wasm.c #INCLUDES the three c/lib/wasm
# translation units textually (see its header for why -- the js_*.c wildcard and
# nine fragments' narrow -I lists), so naming them here too is a duplicate
# symbol, not a belt-and-braces.  They are still listed as PREREQUISITES below
# so a change to the interpreter rebuilds this gate.
WASM_JS_SRC := tests/unit/wasm_js_test.c c/apps/browser/js_wasm.c
WASM_JS_DEP := $(WASM_JS_SRC) $(WASM_EXEC_SRC) c/apps/browser/js_wasm.h \
               c/apps/browser/js_wasm_prelude.inc tests/unit/wasm_js_modules.inc
WASM_JS_CF  := -O1 -g -w -Ic/apps/browser -Ic/lib/wasm -Itests/unit $(JS_INC) \
               -DCONFIG_VERSION='"host"' -fno-math-errno

$(BUILD)/wasm_js_test: $(WASM_JS_DEP) $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_JS_CF) -o $@ $(WASM_JS_SRC) $(QJS_SRC) -lm

test-wasm-js: test-wasm-js-negctl test-wasm-modules $(BUILD)/wasm_js_test
	@$(BUILD)/wasm_js_test

# Each control must FAIL, and the count it produces is PRINTED rather than
# assumed -- a control that stopped controlling anything shows up as a zero.
test-wasm-js-negctl: $(WASM_JS_DEP) $(QJS_SRC)
	@mkdir -p $(BUILD)
	@ok=1; \
	 for c in JS_WASM_NEGCTL_I64 JS_WASM_NEGCTL_DETACH WASM_NEGCTL_MEMP; do \
	   $(CC) $(WASM_JS_CF) -D$$c -o $(BUILD)/wasm_js_$$c $(WASM_JS_SRC) \
	      $(QJS_SRC) -lm || { echo "wasm-js-negctl: $$c did not BUILD"; ok=0; continue; }; \
	   out=$$($(BUILD)/wasm_js_$$c 2>&1); rc=$$?; \
	   n=$$(printf '%s\n' "$$out" | grep -c '^  FAIL '); \
	   if [ $$rc -eq 0 ]; then \
	     echo "wasm-js-negctl: $$c PASSED and must not have -- the control is dead"; \
	     ok=0; \
	   else \
	     echo "wasm-js-negctl: $$c reddens the gate ($$n checks fail)"; \
	   fi; \
	 done; \
	 [ $$ok = 1 ]

# THE SUBJECT IS INSTRUMENTED AND QUICKJS IS NOT, deliberately and not for
# convenience.  QuickJS trips UBSan on its own arithmetic -- `left shift of 171
# by 24 places cannot be represented in type int` at quickjs.c:33368, which is
# a real diagnostic about vendored third-party code this line of work does not
# own and must not edit.  Sanitizing it would abort the run before a single
# check executed, so the gate would report a QuickJS defect as a WebAssembly
# one and be useless for both.  Compiling it clean keeps the subject
# (js_wasm.c + c/lib/wasm) fully instrumented -- ASan's heap checks still cover
# every allocation those files make, including the ones QuickJS hands back --
# while the noise stays out.  What this gate therefore does NOT cover is
# QuickJS's own internals, and that is stated rather than implied.
$(BUILD)/wasm_js_qjs_clean.o: third_party/quickjs/quickjs.c
	@mkdir -p $(BUILD)
	@$(CC) $(WASM_JS_CF) -c -o $@ $<

test-wasm-js-asan: $(WASM_JS_DEP) $(QJS_SRC)
	@mkdir -p $(BUILD)/wasmjsasan
	@for f in $(QJS_SRC); do \
	   o=$(BUILD)/wasmjsasan/$$(basename $$f .c).o; \
	   $(CC) $(WASM_JS_CF) -c -o $$o $$f || exit 1; \
	 done
	@$(CC) $(WASM_JS_CF) -fsanitize=address,undefined -fno-sanitize-recover=all \
	   -o $(BUILD)/wasm_js_asan $(WASM_JS_SRC) $(BUILD)/wasmjsasan/*.o -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/wasm_js_asan

# The embedded module bytes against the .wat they claim to come from.  SKIPS
# LOUDLY without wat2wasm rather than passing -- a check that cannot run is not
# a check that passed.
test-wasm-modules:
	@python3 tools/wasm_js_modules.py --check

# ---- THE GATE THAT MATTERS: a real page in the real browser -----------------
#
#   make test-wasm-page          a page fetches four .wasm modules and runs them
#   make test-wasm-page-negctl   a TRUNCATED module must be refused
#
# WHY, WHEN test-wasm-js ALREADY PASSES 64 CHECKS.  That gate links QuickJS and
# js_wasm.c into a host binary and drives them directly, which proves the
# arithmetic and nothing about the browser.  Everything between the two is
# untested by it: browser.aex actually containing js_wasm.o, js_page.c actually
# calling js_wasm_install, the global actually surviving into a page's script
# context, fetch() actually delivering the bytes, and Response.arrayBuffer()
# actually being what instantiateStreaming falls back to.  "The API exists" is
# the 531/11152 shape.  This is the gate that is not.
#
# EVERY NUMBER IS CHOSEN BY THE HARNESS ON EVERY RUN.  The fixture server bakes
# three random values into the page and the driver asserts the results it
# computed itself -- a + b through an exported function, 4*b through a JS
# closure CALLED FROM wasm, a*10^9+1 as a BigInt, a magic word written by wasm
# and read back through an ArrayBuffer aliasing the module's linear memory.  A
# page printing canned strings cannot pass, and neither can a WebAssembly
# object that merely exists.
#
# Measured 2026-08-30 on the first run: 27 checks, all ok.
#
# `test-wasm-page: test-wasm-page-negctl` on purpose -- CLAUDE.md counts 61
# stranded controls in this tree, all of them stranded by the assumption that a
# positive counterpart runs its control and nothing checking that.
.PHONY: test-wasm-page test-wasm-page-negctl

test-wasm-page: test-wasm-page-negctl $(ISO) $(DISK)
	@python3 tests/qmp/qmp_wasm_page.py $(ISO) $(DISK)

test-wasm-page-negctl: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_wasm_page.py $(ISO) $(DISK) --negctl

# ---- the suites -------------------------------------------------------------
# Declared here rather than left for somebody to remember.  Note what is NOT
# on these lines: every *-negctl, because each is already a PREREQUISITE of its
# positive and naming it here as well would be the stranding pattern CLAUDE.md
# describes -- satisfying the audit while running it never.
#
# test-wasm-js-asan IS on the line: it is the sanitizer run over the C written
# for this feature, it takes about four seconds, and new C is exactly what an
# ASan gate is for.  27 of this tree's *-asan targets sit in the unwired
# baseline instead, which is the dominant convention and the worse one.
#
# test-wasm-clang is host and deliberately absent: it needs a clang with a
# wasm32 backend, skips loudly without one, and a suite entry that usually
# skips trains people to read "SKIPPED" as "ran".  test-wasm-asan is absent
# because it re-runs the whole 5,235-module corpus under ASan.  Both are in
# tests/audit-unwired.baseline as deliberate debt; run them by name.
ci-host: test-wasm test-wasm-exec test-wasm-js test-wasm-modules test-wasm-js-asan
ci-boot: test-wasm-page
