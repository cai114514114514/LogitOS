# tests/aui-split.mk -- aui.c is split into unity-included sub-modules
# (aui_paint.inc, aui_text.inc, aui_motion.inc, and since ui-aui-core
# aui_theme.inc, aui_button.inc, aui_controls.inc, aui_fields.inc,
# aui_lists.inc, aui_overlay.inc, aui_nav/form/data.inc). The stripped object
# code of aui.o and of the -DAUI_NO_AA / -DAUI_COST / -DAUI_SCROLL_INSTANT
# variants is pinned by hash, so any change to the toolkit's code, data or
# statics is visible and has to be re-recorded on purpose.
#
# THE HASHES LIVE IN tests/golden/aui_split.sha256 (`<variant> <sha256>` per
# line), a generated artifact: `make test-aui-split AUIS_UPDATE=1` rewrites it
# from the current tree. Re-record it in the commit that changes the code.
#
# A MOVE IS PROVEN BY ui_symdiff, NOT BY THE HASH. Moving a function between
# modules reorders .text and .data, so the stripped hash changes although no
# instruction did. `make aui-split-symdiff AUIS_BASE=<rev>` builds the same four
# variants from <rev> (git archive into $(BUILD)/aui-symdiff/base) and from the
# working tree, and tools/ui_symdiff.py compares them symbol by symbol with
# placement normalised away; it exits 1 on any difference.
#
# The control builds the SAME four objects with -DAUI_SPLIT_NEGCTL (aui_text.inc
# draws ".." instead of "...") and must fail the hash comparison.

AUIS_VARIANTS := aui:: aui_noaa::-DAUI_NO_AA aui_cost::-DAUI_COST aui_scroll::-DAUI_SCROLL_INSTANT
AUIS_STORE := tests/golden/aui_split.sha256
AUIS_DEPS := $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h c/lib/gfx/include/gfx.h $(AUI_INC_SRC) \
             $(wildcard c/include/ui/*.h)

# $(call auis_check,<outdir>,<extra -D>)
# AUIS_UPDATE=1 writes the store instead of comparing against it.
define auis_check
mkdir -p $(1); rc=0; : > $(1)/store.new; \
	for v in $(AUIS_VARIANTS); do \
	  name=$${v%%::*}; fl=$${v#*::}; \
	  $(CC) $(UCFLAGS) $$fl $(2) -c $(GUIDIR)/aui.c -o $(1)/$$name.o || exit 1; \
	  $(LLVM_OBJCOPY) --strip-debug -j .text -j .rodata -j .data -j .bss $(1)/$$name.o $(1)/$$name.stripped || exit 1; \
	  got=$$(shasum -a 256 $(1)/$$name.stripped | cut -d' ' -f1); \
	  echo "$$name $$got" >> $(1)/store.new; \
	  want=$$(awk -v n=$$name '$$1 == n { print $$2 }' $(AUIS_STORE) 2>/dev/null); \
	  if [ "$(AUIS_UPDATE)" = 1 ]; then echo "recorded objhash $$name.o $$got"; \
	  elif [ -z "$$want" ]; then echo "FAIL objhash $$name.o (no entry in $(AUIS_STORE))"; rc=1; \
	  elif [ "$$got" = "$$want" ]; then echo "ok objhash $$name.o"; \
	  else echo "FAIL objhash $$name.o (got $$got want $$want)"; rc=1; fi; \
	done; \
	if [ "$(AUIS_UPDATE)" = 1 ]; then cp $(1)/store.new $(AUIS_STORE) && echo "AUIS_UPDATE: $(AUIS_STORE) rewritten"; fi; \
	exit $$rc
endef

.PHONY: test-aui-split test-aui-split-negctl aui-split-symdiff

test-aui-split-negctl: $(AUIS_DEPS)
	@if [ "$(AUIS_UPDATE)" = 1 ]; then echo "negative control skipped: AUIS_UPDATE=1 is rewriting $(AUIS_STORE)"; exit 0; fi; \
	rm -rf $(BUILD)/aui-split-neg; \
	rc=0; ( $(call auis_check,$(BUILD)/aui-split-neg,-DAUI_SPLIT_NEGCTL) ) > $(BUILD)/aui-split-neg.log 2>&1 || rc=$$?; \
	cat $(BUILD)/aui-split-neg.log; \
	if [ $$rc -eq 0 ]; then echo "NEGATIVE CONTROL FAILED: the hashes match with the ellipsis changed"; exit 1; \
	elif ! grep -q '^FAIL objhash aui.o (got' $(BUILD)/aui-split-neg.log; then \
	  echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL objhash aui.o (got ...)' line"; exit 1; \
	else echo "negative control ok: FAIL objhash aui.o observed"; fi

test-aui-split: test-aui-split-negctl $(AUIS_DEPS)
	@rm -rf $(BUILD)/aui-split
	@$(call auis_check,$(BUILD)/aui-split,)

# Not a gate of its own (it needs a base revision): the proof a split commit
# quotes. AUIS_BASE defaults to HEAD, i.e. "the working tree moves nothing".
AUIS_BASE ?= HEAD
aui-split-symdiff:
	@rm -rf $(BUILD)/aui-symdiff && mkdir -p $(BUILD)/aui-symdiff/base $(BUILD)/aui-symdiff/a $(BUILD)/aui-symdiff/b
	@git archive $(AUIS_BASE) c include | tar -x -C $(BUILD)/aui-symdiff/base
	@for v in $(AUIS_VARIANTS); do name=$${v%%::*}; fl=$${v#*::}; \
	  ( cd $(BUILD)/aui-symdiff/base && $(CC) $(UCFLAGS) $$fl -c $(GUIDIR)/aui.c -o ../a/$$name.o ) || exit 1; \
	  $(CC) $(UCFLAGS) $$fl -c $(GUIDIR)/aui.c -o $(BUILD)/aui-symdiff/b/$$name.o || exit 1; \
	done
	@LLVM_OBJDUMP="$(firstword $(shell which llvm-objdump 2>/dev/null) $(wildcard /opt/homebrew/opt/llvm/bin/llvm-objdump /usr/local/opt/llvm/bin/llvm-objdump))" \
	  python3 tools/ui_symdiff.py --dirs $(BUILD)/aui-symdiff/a $(BUILD)/aui-symdiff/b aui.o aui_noaa.o aui_cost.o aui_scroll.o

ci-host: test-aui-split
