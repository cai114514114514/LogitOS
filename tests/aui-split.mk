# tests/aui-split.mk -- aui.c is split into unity-included sub-modules
# (aui_paint.inc, aui_text.inc, aui_motion.inc). The split must be
# behaviour-preserving AND layout-preserving: the stripped object code of aui.o
# and of the -DAUI_NO_AA / -DAUI_COST / -DAUI_SCROLL_INSTANT variants hashes the
# same as before the split. The hashes below were recorded from the PRE-split
# tree. Any change to the toolkit's code, data or statics changes them -- when
# that is intended, re-record them in the commit that changes the code.
#
# The control builds the SAME four objects with -DAUI_SPLIT_NEGCTL (aui_text.inc
# draws ".." instead of "...") and must fail the hash comparison.

AUIS_VARIANTS := aui:: aui_noaa::-DAUI_NO_AA aui_cost::-DAUI_COST aui_scroll::-DAUI_SCROLL_INSTANT
AUIS_H_aui        := 4886589a44cd24873b416ee176a8592216a4066f41ac5a0bb00090ea09b8cc0d
AUIS_H_aui_noaa   := ecd186c7a6e7a395e136afda5e00e70f849ebb133a687a9fc075dc6dfce51757
AUIS_H_aui_cost   := afa7d7d45fd08dcded22454ca6acf953b5e639a7ffebba62feeaab8180d123ca
AUIS_H_aui_scroll := ff2e05bd2efdaeffe4282e6a7b8e64ea418c8fbfccbfb90fb71b7c744791dc5c
AUIS_DEPS := $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h c/lib/gfx/include/gfx.h $(AUI_INC_SRC)

# $(call auis_check,<outdir>,<extra -D>,<expect: ok|fail>)
define auis_check
mkdir -p $(1); rc=0; \
	for v in $(AUIS_VARIANTS); do \
	  name=$${v%%::*}; fl=$${v#*::}; \
	  $(CC) $(UCFLAGS) $$fl $(2) -c $(GUIDIR)/aui.c -o $(1)/$$name.o || exit 1; \
	  $(LLVM_OBJCOPY) --strip-debug -j .text -j .rodata -j .data -j .bss $(1)/$$name.o $(1)/$$name.stripped || exit 1; \
	  got=$$(shasum -a 256 $(1)/$$name.stripped | cut -d' ' -f1); \
	  case $$name in aui) want=$(AUIS_H_aui);; aui_noaa) want=$(AUIS_H_aui_noaa);; \
	    aui_cost) want=$(AUIS_H_aui_cost);; aui_scroll) want=$(AUIS_H_aui_scroll);; esac; \
	  if [ "$$got" = "$$want" ]; then echo "ok objhash $$name.o"; \
	  else echo "FAIL objhash $$name.o (got $$got want $$want)"; rc=1; fi; \
	done; exit $$rc
endef

.PHONY: test-aui-split test-aui-split-negctl

test-aui-split-negctl: $(AUIS_DEPS)
	@rm -rf $(BUILD)/aui-split-neg
	@rc=0; ( $(call auis_check,$(BUILD)/aui-split-neg,-DAUI_SPLIT_NEGCTL) ) > $(BUILD)/aui-split-neg.log 2>&1 || rc=$$?; \
	cat $(BUILD)/aui-split-neg.log; \
	if [ $$rc -eq 0 ]; then echo "NEGATIVE CONTROL FAILED: the hashes match with the ellipsis changed"; exit 1; \
	elif ! grep -q '^FAIL objhash aui.o' $(BUILD)/aui-split-neg.log; then \
	  echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL objhash aui.o' line"; exit 1; \
	else echo "negative control ok: FAIL objhash aui.o observed"; fi

test-aui-split: test-aui-split-negctl $(AUIS_DEPS)
	@rm -rf $(BUILD)/aui-split
	@$(call auis_check,$(BUILD)/aui-split,)

ci-host: test-aui-split
