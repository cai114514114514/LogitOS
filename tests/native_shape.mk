# tests/native_shape.mk -- platform functions that look native.
#
#   make test-native-fn      QuickJS's JS_MarkNativeFunction / JS_MarkNativeCode /
#                            JS_GetOwnPropertyQuiet on a bare engine (seconds)
#   make test-native-shape   the nativize pass (js/runtime/native_shape.c)
#                            through js_page_open with the whole platform,
#                            a worker realm, and shape.js against Chrome 153
#
# WHY. The browser's platform is largely JS preludes, and every function they
# handed a page stringified to its SOURCE, owned a `prototype`, could be
# `new`ed, and was often named '' -- which Function.prototype.toString, `in`,
# Reflect.construct and .name tell any anti-tamper script in one line each.
# tests/fixtures/webidl_shape (the dump that runs unchanged in Chrome) was the
# measurement: every NATIVE finding in webidl_shape_diff.py was this.
#
# THREE CONTROLS, each the most plausible wrong implementation rather than a
# wrecking ball, and each required to fail EXACTLY its own group:
#
#   test-native-fn-negctl     quickjs.c with -DJS_NATIVE_NEGCTL_KEEP_CONSTRUCT:
#                             the mark deletes `prototype` and keeps
#                             [[Construct]] -- which leaves `new fn()` working
#                             through Object.prototype. Every [construct]
#                             check must fail and nothing else.
#   test-native-fn-negctl-eval quickjs.c with -DJS_NATIVE_NEGCTL_MARK_EVAL:
#                             JS_MarkNativeCode's first version, which marked
#                             everything the realm had compiled -- including
#                             the page's <body onload="..."> that the cssom
#                             shim compiles with `new Function` AT INSTALL.
#                             Every [eval] check must fail and nothing else.
#   test-native-shape-negctl  native_shape.c with -DNATIVE_SHAPE_NEGCTL_SKIP:
#                             the pass does nothing. Every check of that pass
#                             [native] must fail; [idl]/[engine]/[page]/[works] --
#                             the platform still working after the pass
#                             deleted hundreds of `prototype`s, the page's own
#                             functions untouched -- must still pass.
#
# Original claim: "Every [native] check must fail" included readyState and
# sendBeacon. Correction (2026-09-30), measured by the actual skip binary:
# 48 checks, 25 of 27 [native] failed, zero other failures. The earlier IDL
# pass already gives those two computed getters/methods their name and lack
# of prototype, and 'use strip' supplies their native spelling. Their checks
# are retained as [idl], so this control now observes exactly the 25 checks
# dependent on the native pass, with both earlier-producer checks still green.
# A signal can leave the expected FAIL rows printed before aborting cleanup.
# Accept only the runner's normal exit 1 and its completed summary, matched
# against every observed row; an abort or unrelated JS exception is not the
# requested control even if every [native] label has already gone red.
#
# THE CONVENTION THE ENGINE HALF SIGNS UP FOR. A closure of platform code made
# AFTER the pass (a collection's item(), a stream reader's read(), a worker
# event's preventDefault) is born a WebIDL operation -- no `prototype`, no
# [[Construct]] -- unless a capital letter says constructor: the code's own
# name (`function Blob() {}`), or the binding's (`var C = function () {}`,
# `{ Named: function () {} }`). A constructor made after the pass by plain
# ASSIGNMENT (`o.Foo = function () {}`) would be born an operation; measured
# 2026-09-25, every `new`, `extends`, `.prototype =` and Reflect.construct
# target in the platform's preludes is capital-named or made at install
# (`grep -rhoE "new +[A-Za-z_$.]+" c/apps/gui/browser/js` finds no lowercase
# callee outside comments), and test-native-fn pins the price explicitly.
#
# THE HARNESS LINKS A SUBTRACTION from $(BROWSER_JS_SRC), the list
# browser.elf is built from, like tests/canvas.mk and tests/worker.mk: a js
# TU added to the browser is on this link line the same minute, and its
# functions are measured by the census below without anyone remembering.
# OUT, each with its reason:
#   app/main.c           the ring-3 shell: window management and a ring-3 main.
#   js/media/media.c,    int 0x80 syscalls (gui_blit/snd_write) and the whole
#   js/media/media_src.c c/lib/{media,video,audio} behind them.
#   js/dom/forms.c       needs dom/forms.c, which needs text_measure -- a
#                        ring-3 syscall this harness does not define.
# Everything else, js_worker.c and js_module.c included, links against
# tests/unit/loader_fakebfetch.c, the in-memory bfetch tests/worker.mk uses
# (and the test's own res_fetch, which refuses: no fixture is a resource).
#
# DEFERRED (=) where a variable is defined further down the Makefile
# (IMG_HOST_SRC, IMG_HOST_INC): this fragment is -included before them.

NATIVE_FN_SRC := tests/unit/native_fn_test.c $(QJS_SRC)
NATIVE_FN_CF  := $(JS_INC) -DCONFIG_VERSION='"host"'

NATIVE_SHAPE_JS_OUT := c/apps/gui/browser/app/main.c \
                       c/apps/gui/browser/js/media/media.c c/apps/gui/browser/js/media/media_src.c \
                       $(BROWSER_FORM_JS_SRC)
NATIVE_SHAPE_SRC = tests/unit/native_shape_test.c tests/unit/loader_fakebfetch.c \
                   $(filter-out $(NATIVE_SHAPE_JS_OUT),$(BROWSER_JS_SRC)) \
                   c/apps/gui/browser/css/engine.c c/apps/gui/browser/css/vars.c \
                   c/apps/gui/browser/css/interp.c \
                   c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
                   c/net/http/ws.c c/net/ssh/base64.c c/crypto/hash/sha1.c \
                   $(IMG_HOST_SRC) $(HTML_PARSER_SRC)
NATIVE_SHAPE_CF = $(BTEST_INC) $(CSS_INC) $(JS_INC) $(IMG_HOST_INC) -Iinclude/abi \
                  -Ic/net/http -Ic/net/ssh -Ic/crypto -Itests/unit \
                  -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

.PHONY: test-native-fn test-native-fn-negctl test-native-fn-negctl-eval test-native-shape test-native-shape-negctl \
        test-native-shape-san native-shape-link-check

# A control is a PREREQUISITE of its suite (tools/audit_tests.py NOT_CI drops
# every test-*-negctl on the assumption its positive runs it; this makes that
# true -- tests/license.mk is the worked example).
test-native-fn-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(NATIVE_FN_CF) -DJS_NATIVE_NEGCTL_KEEP_CONSTRUCT -o $(BUILD)/native_fn_negctl $(NATIVE_FN_SRC) -lm
	@rc=0; $(BUILD)/native_fn_negctl > $(BUILD)/native_fn_negctl.log 2>&1 || rc=$$?; \
	all=$$(grep -cE '^(ok +|FAIL )\[construct\]' $(BUILD)/native_fn_negctl.log); \
	bad=$$(grep -c '^FAIL \[construct\]' $(BUILD)/native_fn_negctl.log); \
	any=$$(grep -c '^FAIL' $(BUILD)/native_fn_negctl.log); \
	if [ $$rc -ne 0 ] && [ $$all -gt 0 ] && [ $$bad = $$all ] && [ $$any = $$bad ]; then \
		echo "negative control ok: keeping [[Construct]] fails all $$bad [construct] checks and nothing else"; \
	else cat $(BUILD)/native_fn_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: rc=$$rc, [construct] $$bad of $$all failed, $$any failures in all"; exit 1; fi

test-native-fn-negctl-eval:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(NATIVE_FN_CF) -DJS_NATIVE_NEGCTL_MARK_EVAL -o $(BUILD)/native_fn_negctl_eval $(NATIVE_FN_SRC) -lm
	@rc=0; $(BUILD)/native_fn_negctl_eval > $(BUILD)/native_fn_negctl_eval.log 2>&1 || rc=$$?; \
	all=$$(grep -cE '^(ok +|FAIL )\[eval\]' $(BUILD)/native_fn_negctl_eval.log); \
	bad=$$(grep -c '^FAIL \[eval\]' $(BUILD)/native_fn_negctl_eval.log); \
	any=$$(grep -c '^FAIL' $(BUILD)/native_fn_negctl_eval.log); \
	if [ $$rc -ne 0 ] && [ $$all -gt 0 ] && [ $$bad = $$all ] && [ $$any = $$bad ]; then \
		echo "negative control ok: marking eval-compiled code fails all $$bad [eval] checks and nothing else"; \
	else cat $(BUILD)/native_fn_negctl_eval.log; \
		echo "NEGATIVE CONTROL FAILED: rc=$$rc, [eval] $$bad of $$all failed, $$any failures in all"; exit 1; fi

test-native-fn: test-native-fn-negctl test-native-fn-negctl-eval
	@$(CC) -O1 -g -w $(NATIVE_FN_CF) -o $(BUILD)/native_fn_test $(NATIVE_FN_SRC) -lm
	@rc=0; $(BUILD)/native_fn_test > $(BUILD)/native_fn_test.log 2>&1 || rc=$$?; \
		grep -E '^(FAIL|native-fn:)' $(BUILD)/native_fn_test.log; exit $$rc

# The drift check canvas.mk carries, for the same reason: a name in the OUT
# list that the browser no longer links excludes nothing, and a RENAMED file's
# old name reads like a live decision while its new one is linked silently.
native-shape-link-check:
	@stale=""; for f in $(NATIVE_SHAPE_JS_OUT); do \
	    case " $(BROWSER_JS_SRC) " in *" $$f "*) ;; *) stale="$$stale $$f";; esac; \
	  done; \
	  if [ -n "$$stale" ]; then echo "native-shape-link-check: FAIL -- NATIVE_SHAPE_JS_OUT names files the browser no longer links:$$stale"; exit 1; fi; \
	  case " $(BROWSER_JS_SRC) " in *" c/apps/gui/browser/js/runtime/native_shape.c "*) ;; \
	    *) echo "native-shape-link-check: FAIL -- the browser does not link js/runtime/native_shape.c, so the pass never runs in the product"; exit 1;; esac

test-native-shape-negctl: native-shape-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(NATIVE_SHAPE_CF) -DNATIVE_SHAPE_NEGCTL_SKIP -o $(BUILD)/native_shape_negctl \
	    $(NATIVE_SHAPE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rc=0; $(BUILD)/native_shape_negctl > $(BUILD)/native_shape_negctl.log 2>&1 || rc=$$?; \
	all=$$(grep -cE '^(ok +|FAIL )\[native\]' $(BUILD)/native_shape_negctl.log); \
	bad=$$(grep -c '^FAIL \[native\]' $(BUILD)/native_shape_negctl.log); \
	any=$$(grep -c '^FAIL' $(BUILD)/native_shape_negctl.log); \
	rows=$$(grep -cE '^(ok +|FAIL )\[[a-z]+\]' $(BUILD)/native_shape_negctl.log); \
	idl=$$(grep -cE '^(ok +|FAIL )\[idl\]' $(BUILD)/native_shape_negctl.log); \
	idl_ok=$$(grep -cE '^ok +\[idl\]' $(BUILD)/native_shape_negctl.log); \
	if [ $$rc -eq 1 ] && grep -q '^\[native-shape\] SKIPPED' $(BUILD)/native_shape_negctl.log && \
	   [ $$all -gt 0 ] && [ $$bad = $$all ] && [ $$any = $$bad ] && \
	   [ $$idl -gt 0 ] && [ $$idl_ok = $$idl ] && \
	   ! grep -q '\[js exception\]' $(BUILD)/native_shape_negctl.log && \
	   grep -Fqx "native-shape: $$rows checks, $$bad failed ($$bad of $$all [native]; 0 other)" $(BUILD)/native_shape_negctl.log; then \
	  echo "negative control ok: all $$bad pass-dependent [native] checks fail; all $$idl [idl] and other checks pass, complete runner exits 1"; \
	else grep -E '^(FAIL|ok|census|native-shape|\[native-shape\])' $(BUILD)/native_shape_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: rc=$$rc, [native] $$bad of $$all failed, $$any failures in all"; exit 1; fi

test-native-shape: test-native-shape-negctl test-native-fn $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O1 -g -w $(NATIVE_SHAPE_CF) -o $(BUILD)/native_shape_test \
	    $(NATIVE_SHAPE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(CC) -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter \
		-isystem third_party/quickjs -Ic/apps/gui/browser c/apps/gui/browser/js/runtime/native_shape.c
	@rc=0; $(BUILD)/native_shape_test > $(BUILD)/native_shape_test.log 2>&1 || rc=$$?; \
		grep -E '^(FAIL|census|native-shape:|\[native-shape\])' $(BUILD)/native_shape_test.log; exit $$rc

# Exercise the owned JSValue handoff and its release in the same real page
# lifecycle, after both genuine controls and the normal suite have run.
# Darwin has no LeakSanitizer here; address and undefined-behaviour failures
# remain fatal. libcss_host.a/Rust stay the ordinary host prerequisite builds.
test-native-shape-san: test-native-shape
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(NATIVE_SHAPE_CF) -o $(BUILD)/native_shape_san \
		$(NATIVE_SHAPE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rc=0; ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		$(BUILD)/native_shape_san > $(BUILD)/native_shape_san.log 2>&1 || rc=$$?; \
		grep -E '^(FAIL|native-shape:)' $(BUILD)/native_shape_san.log; \
		if [ $$rc -ne 0 ] || grep -q '\[js exception\]' $(BUILD)/native_shape_san.log; then \
		  cat $(BUILD)/native_shape_san.log; exit 1; fi

ci-host: test-native-shape test-native-shape-san
