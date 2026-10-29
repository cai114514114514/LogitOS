# tests/weakmap.mk -- the WeakMap/FinalizationRegistry lifetime gate.
#
# Own fragment for the same reason as every other tests/*.mk: several lines of
# work share this tree and a whole-file Makefile overwrite must not be able to
# delete another line's targets.
#
#   make test-weakmap-fin           the gate: three poison shapes + churn
#   make test-weakmap-fin-control   the NEGATIVE CONTROL: the
#                                   js_finreg_unregister two-pass fix
#                                   mechanically reverted, and the gate binary
#                                   MUST die on the repro (watched red on
#                                   2026-09-09: exit 139 plain, and ASan
#                                   names js_finreg_unregister as the
#                                   heap-use-after-free site)
#
# WHAT THE GATE GUARDS. The chat.deepseek.com browser crash investigation of
# 2026-09-09 convicted one engine defect by ASan stack: js_finreg_unregister
# freed a token-matching cell's held_value inside its own list walk, the free
# cascaded synchronously (gc_phase NONE) through reset_weak_ref() and freed a
# LATER cell off the very list being walked -- the pre-saved el1 dangled and
# the next iteration read fre->token through freed memory. The fix is the
# two-pass shape (sever everything, then free); this gate holds both halves:
# the poison snippets build the exact cascade shapes, and the control proves
# the gate still fails without the fix.
#
# The re-read hardening in map_find_record (same investigation, different
# defect class: a one-load transient NULL observed on the real machine only)
# is deliberately NOT gated from JS: its branch is unreachable from a healthy
# engine by construction -- it answers a machine anomaly, and a host gate
# could only pass vacuously. Its observable is the site run itself.

# One jar: QJS_SRC is the tree's own engine source list (Makefile, above the
# fragment includes), so a file the engine grows a dependency on follows this
# gate's link line automatically -- the hand-copied-source-list trap CLAUDE.md
# documents is exactly what reusing it avoids.
$(BUILD)/weakmap_fin_test: tests/unit/weakmap_fin_test.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/unit/weakmap_fin_test.c $(QJS_SRC) -lm

# The control is a PREREQUISITE of the positive gate, not a sibling named on
# a ci- line: per AGENTS.md that names-but-never-runs shape is exactly how a
# control rots while looking wired.
test-weakmap-fin: test-weakmap-fin-control $(BUILD)/weakmap_fin_test
	@$(BUILD)/weakmap_fin_test

# THE NEGATIVE CONTROL. A python rewrite turns the two-pass fix back into
# exactly the interleaved single-loop shape that shipped before 2026-09-09;
# every anchor is counted (== 1) so a moved patch fails LOUDLY here instead
# of silently building a half-reverted engine. The rebuilt gate binary must
# die on the FinalizationRegistry repro (signal or nonzero exit); if it
# passes, the positive gate above is not testing the fix.
$(BUILD)/negctl_weakmap/quickjs.c: third_party/quickjs/quickjs.c
	@mkdir -p $(dir $@)
	@python3 -c 'import sys; \
src = open("third_party/quickjs/quickjs.c").read(); \
a1 = "            list_add_tail(&fre->reg_link, &to_free);\n            removed = TRUE;"; \
n1 = "            JS_FreeValue(ctx, fre->held_value);\n            js_free(ctx, fre);\n            removed = TRUE;"; \
a4 = "    init_list_head(&to_free);\n"; \
a5 = "    struct list_head to_free;\n"; \
a6 = "    list_for_each_safe(el, el1, &to_free) {\n        JSFinRecEntry *fre = list_entry(el, JSFinRecEntry, reg_link);\n        list_del(&fre->reg_link);\n        JS_FreeValue(ctx, fre->held_value);\n        js_free(ctx, fre);\n    }\n"; \
assert src.count(a1) == 1, "unregister fix anchor 1 moved"; \
assert src.count(a6) == 1, "unregister fix anchor 2 moved"; \
out = src.replace(a1, n1).replace(a6, "").replace(a4, "").replace(a5, ""); \
assert "to_free" not in out, "two-pass remnants left behind"; \
open("$(BUILD)/negctl_weakmap/quickjs.c", "w").write(out)'

test-weakmap-fin-control: $(BUILD)/negctl_weakmap/quickjs.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $(BUILD)/weakmap_fin_control \
	    tests/unit/weakmap_fin_test.c $(BUILD)/negctl_weakmap/quickjs.c \
	    $(filter-out third_party/quickjs/quickjs.c,$(QJS_SRC)) -lm
	@if $(BUILD)/weakmap_fin_control > $(BUILD)/weakmap_fin_control.log 2>&1; then \
	    echo "FAIL (control): the gate passes with js_finreg_unregister's fix reverted -- it is not testing the fix"; \
	    exit 1; \
	 else \
	    echo "PASS (control): reverted js_finreg_unregister dies on the repro as it must --"; \
	    echo "  $(BUILD)/weakmap_fin_control.log holds the death"; \
	 fi

.PHONY: test-weakmap-fin test-weakmap-fin-control
