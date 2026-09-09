# tests/crashhunt.mk -- the browser sudden-death (闪退) hunt's gates.
#
# WHAT THIS FRAGMENT GATES (the hunt's findings, 2026-08-30):
#
#   test-crashhunt-watch   the INSTRUMENT's own reflexes, host-side, seconds:
#                          the serial classifier that turns boot logs into
#                          SURVIVED / BROWSER-DIED-* / KERNEL-PANIC verdicts.
#                          Its negative control is a PREREQUISITE and is
#                          watched red by sabotage (see the recipe).
#   test-crashhunt-spin    the 45-second question, answered forever: a script
#                          that spins 120 s in one entry MUST be bitten by the
#                          js_page watchdog at ~45 s (JS_SLICE_MS_DEFAULT,
#                          js_page.c:240), the browser MUST survive it, and a
#                          timer registered BEFORE the bite MUST keep firing.
#                          The ratchet half: CH-SPIN-COMPLETED (spin ran to
#                          the end unbitten) must NEVER appear -- if the
#                          watchdog is ever disabled or sized past 120 s this
#                          gate goes red and says so.
#   test-crashhunt-survive the survival ratchet: the two stress shapes that
#                          are reproducible today (canvas surface churn, the
#                          biggest allocator pressure a page can make, and
#                          map-churn, the owner's real crash SHAPE) must come
#                          back SURVIVED. NOT a claim that the browser cannot
#                          crash -- the owner's js_map_get crash (rip
#                          js_map_get+0x90, cr2=0x10, /core.1 extracted from
#                          their own disk) did not reproduce under either, and
#                          the honest state is "gated what is reproducible,
#                          reported what is not".
#
# WHY THE SURVIVAL GATE EXISTS AT ALL when it can only go red: because
# "sometimes crashes" dies as a report the moment nobody can say whether
# TODAY's build still crashes on the shapes that were clean yesterday. A
# ratchet is how "the browser survived X on this date" stays true.
#
# The scenarios this gate does NOT run (idle control, dom-growth, js-heap,
# docwrite-edge, nav churn, specimen dwell) stay in the driver for hunts;
# each is another QEMU boot and CI already boots enough. The idle control
# was RUN during the hunt (263 guest-s, SURVIVED) and is documented in the
# report, not gated.
#
# SKIPS LOUDLY (one line naming the missing capability) rather than passing
# silently when QEMU or the artifacts are absent.

.PHONY: test-crashhunt-watch test-crashhunt-watch-negctl \
        test-crashhunt-spin test-crashhunt-survive

CH_DRIVER := tests/qmp/qmp_crashhunt.py
CH_FX     := tests/fixtures/crashhunt
CH_OUT    := $(BUILD)/crashhunt

# ---------------------------------------------------------------- the watcher
# Host-side, no QEMU: the Watcher class (in the driver) fed synthetic serial
# logs. The NEGCTL is the prerequisite and sabotages exactly the conflation
# the module's own comment warns about ("only a victim mark is a death"), by
# making the sabotaged watcher treat `[oom] pmm_alloc refused` (pressure) as
# death; the control REQUIRES the check to catch that, i.e. the sabotaged
# build must FAIL. Watched red before the positive was allowed to run.
test-crashhunt-watch-negctl:
	@mkdir -p $(CH_OUT)
	@CH_SABOTAGE=1 python3 $(CH_FX)/watcher_control.py $(CH_OUT)/negctl.log; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	    echo "NEGCTL-FAIL: the sabotaged watcher passed the pressure-is-not-death check"; \
	    echo "  -- nothing asks the instrument to tell an oom kill from oom noise."; \
	    exit 1; \
	elif [ $$rc -eq 3 ]; then \
	    echo "negctl: sabotage caught, and where predicted:"; \
	    tail -4 $(CH_OUT)/negctl.log | sed 's/^/       /'; \
	else \
	    echo "NEGCTL-FAIL: exit $$rc -- the control broke for an unrelated reason:"; \
	    tail -5 $(CH_OUT)/negctl.log; exit 1; \
	fi

test-crashhunt-watch: test-crashhunt-watch-negctl
	@mkdir -p $(CH_OUT)
	@python3 $(CH_FX)/watcher_control.py $(CH_OUT)/watch.log \
	    && echo "crashhunt-watch: classifier reflexes ok (fault/panic/victim=death," \
	       "watchdog/pressure=not-death, silence=no-death)" \
	    || { echo "FAIL: see $(CH_OUT)/watch.log"; cat $(CH_OUT)/watch.log; exit 1; }

# ----------------------------------------------------------------- boot gates
# One boot per scenario per run (the driver's rule: cross-scenario state
# manufactures crashes the scenarios did not cause). QEMU's presence is
# checked HERE, before the boot, so a skip names the capability and the
# command that would settle it.
# GNU Make 3.81 (this host) has no `define name =` form -- the `=` variant
# silently mis-names the variable and every $(call) expands empty, which is
# how a gate can "pass" without running its boot. Plain `define` only.
define crashhunt_boot
	@if ! command -v $(QEMU) >/dev/null 2>&1; then \
	    echo "SKIP-LOUDLY: $(QEMU) not on PATH -- cannot boot $(1); the command that would settle it: $(QEMU) --version"; \
	    exit 0; \
	 fi
	@mkdir -p $(CH_OUT)
	python3 $(CH_DRIVER) --iso $(ISO) --disk $(DISK) --out $(CH_OUT)/$(2).json \
	    --serial $(CH_OUT)/$(2).serial.txt $(3) $(4)
endef

test-crashhunt-spin:
	$(call crashhunt_boot,spin,spin,--scenario spin,--budget 420)
	@python3 $(CH_FX)/spin_verdict.py $(CH_OUT)/spin.json $(CH_OUT)/spin.serial.txt \
	    || { cat $(CH_OUT)/spin.json; exit 1; }

test-crashhunt-survive:
	$(call crashhunt_boot,canvas,canvas,--scenario canvas --iters 24,--budget 420)
	$(call crashhunt_boot,map,map,--scenario map --rounds 400,--budget 600)
	@python3 $(CH_FX)/survive_verdict.py $(CH_OUT)/canvas.json $(CH_OUT)/map.json \
	    || { cat $(CH_OUT)/canvas.json $(CH_OUT)/map.json; exit 1; }

ci-host: test-crashhunt-watch
ci-boot: test-crashhunt-spin test-crashhunt-survive
