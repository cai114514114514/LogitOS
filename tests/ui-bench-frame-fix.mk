# tests/ui-bench-frame-fix.mk -- bench-gfx-frame's gallery_cost.elf link
# (tests/gfx.mk, survey E, design 9.6).
#
# The bench link carried no libc, so ld.lld refused it on every run:
#   openlogit_material.o  needs memset (liquid_glass_impl)
#   display_blit_ex.o     needs memcpy
#   gallery.c             needs snprintf
# gallery.elf is not affected. tools/agent_link.py adds the agent SDK's
# libc.a to every catalogued app link (tests/agent.mk) and gallery is
# catalogued. gallery_cost is not, so tests/gfx.mk now links the same
# $(BUILD)/agent/libc.a archive explicitly (BFF_LIBC). The archive only
# supplies members the link still needs.
#
#   make test-ui-bench-frame-fix          the gate: gallery_cost.elf and its
#                                         .aex link, and define memset,
#                                         memcpy and snprintf
#   make test-ui-bench-frame-fix-negctl   the same link WITHOUT the archive
#                                         (UI_BFF_NEGCTL=1, own build dir); it
#                                         MUST fail with 'undefined symbol: memset'
# bench-gfx-frame itself stays report-only (tests/gfx.mk).

.PHONY: test-ui-bench-frame-fix test-ui-bench-frame-fix-negctl

UIBFF_ELF  := $(BUILD)/gallery_cost.elf
UIBFF_NEG  := $(BUILD)-bffneg
UIBFF_NM   := $(firstword $(shell which llvm-nm 2>/dev/null) $(wildcard /opt/homebrew/opt/llvm/bin/llvm-nm /usr/local/opt/llvm/bin/llvm-nm))
UIBFF_SYMS := memset memcpy snprintf

test-ui-bench-frame-fix-negctl:
	@mkdir -p $(BUILD)
	@rc=0; $(MAKE) -j8 BUILD=$(UIBFF_NEG) UI_BFF_NEGCTL=1 $(UIBFF_NEG)/gallery_cost.elf > $(BUILD)/ui_bff_negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		tail -20 $(BUILD)/ui_bff_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: gallery_cost.elf linked without the libc archive"; exit 1; \
	elif grep -q 'undefined symbol: memset' $(BUILD)/ui_bff_negctl.log; then \
		echo "negative control ok: without the libc archive the link fails with undefined symbol: memset"; \
	else \
		tail -40 $(BUILD)/ui_bff_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'undefined symbol: memset' line"; exit 1; \
	fi

test-ui-bench-frame-fix: test-ui-bench-frame-fix-negctl $(UIBFF_ELF) $(BUILD)/gallery_cost.aex
	@test -n "$(UIBFF_NM)" || { echo "FAIL no llvm-nm on this host"; exit 1; }
	@for s in $(UIBFF_SYMS); do \
		if $(UIBFF_NM) $(UIBFF_ELF) | grep -qE " [TtWw] $$s\$$"; then echo "ok $$s defined in gallery_cost.elf"; \
		else echo "FAIL $$s not defined in gallery_cost.elf"; exit 1; fi; \
	done

ci-host: test-ui-bench-frame-fix
