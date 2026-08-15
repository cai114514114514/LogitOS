# tests/settings2.mk -- gates for /bin/pref (the coreutil onto the "app."
# preference namespace) and the window-geometry consumer wired through it
# (c/apps/gui/textedit.c). See c/apps/coreutils/pref.c's header comment for
# what this store is and tests/boot/run-pref-test.sh for what "test-pref"
# actually proves.
#
# Kept out of the main Makefile on purpose -- same reason as tests/libc.mk,
# tests/audio.mk, tests/h265.mk, tests/nic.mk: several agents edit that file
# concurrently and a shared-file edit cannot be committed without swallowing
# theirs. Unlike those, this fragment ALSO never touches the contended
# `CLI := sh echo ls cat ...` line itself (the one the M28 orchestration
# specifically flagged as contended): it registers pref onto the CLI build
# the same way tests/libc.mk registers /bin/libctest2 --
#
#   $(eval $(call CLI_RULE,pref))   -- invokes the exact macro the main
#                                       Makefile's own CLI loop uses, so
#                                       $(BUILD)/pref.elf / .aex get real
#                                       build rules without editing the loop.
#   CLI += pref                     -- APPENDS to the CLI list. The `CLI :=`
#                                       assignment and its $(foreach ...
#                                       $(eval ...)) loop have already run by
#                                       the time this fragment is read (this
#                                       -include sits near the bottom of the
#                                       Makefile, well after both), so this
#                                       append cannot retroactively add pref
#                                       to THAT loop -- it does not need to.
#                                       $(DISK)'s own mkfs.py line also does
#                                       `$(foreach c,$(CLI),$(BUILD)/$(c).aex
#                                       :/bin/$(c))`, but that reference is
#                                       inside a RECIPE body, and GNU make
#                                       expands a recipe's $(...) references
#                                       at RECIPE-RUN time -- after every
#                                       -include has already been processed
#                                       -- not at the line's own parse time.
#                                       So by the time that recipe actually
#                                       runs, $(CLI) already includes "pref"
#                                       and it lands at /bin/pref exactly as
#                                       if it had been on the list all along.
#   $(DISK): $(BUILD)/pref.aex      -- an EXTRA prerequisite-only rule for a
#                                       target the main Makefile already
#                                       defines with its own recipe. GNU make
#                                       merges prerequisites from multiple
#                                       rules naming the same target as long
#                                       as only one supplies a recipe, which
#                                       is the case here -- so this is enough
#                                       to make plain `make $(DISK)` build
#                                       pref.aex FIRST, which the mkfs.py
#                                       recipe above then needs to already
#                                       exist on disk.
#
# ONE Makefile line is still unavoidable for `make test-pref` to exist at
# all: this file is never read unless something says so. That line is:
#
#     -include tests/settings2.mk
#
# beside the other test fragments' -include lines (~Makefile:3463, next to
# `-include tests/settings.mk`). It is the only Makefile change this whole
# unit needs -- report it; do not make it here (the contended-file rule this
# comment opened with applies to the ONE LINE this file cannot add for
# itself, same as it does to the CLI list line).
#
# For verification without that line: build $(BUILD)/pref.aex by hand with
# the CLI_RULE recipe (Makefile ~405-430) and repack a scratch disk image
# with tools/mkfs.py, then run tests/boot/run-pref-test.sh against that ISO
# and disk directly -- exactly what this unit's own gate evidence did.

# INTEGRATION NOTE (2026-08-15): the three registration lines this comment
# block describes ($(eval $(call CLI_RULE,pref)) / CLI += pref /
# $(DISK): $(BUILD)/pref.aex) were REMOVED at integration, because the
# orchestrator had already put "pref" on the contended CLI list line itself
# in the power commit (9398246c2) -- the one edit this fragment was designed
# to avoid was made once, centrally, before this fragment landed. With pref
# on the main list, the main foreach loop already evals CLI_RULE for it and
# $(DISK)'s prerequisite expansion already contains pref.aex; repeating the
# eval here would define every pref rule twice ("overriding recipe"), and
# the += would list /bin/pref twice in the mkfs copy set. The mechanism
# described above stays documented because it is how the NEXT fragment-owned
# coreutil should register itself when the list line is contended again.

test-pref: $(ISO) $(DISK)
	@bash tests/boot/run-pref-test.sh $(ISO) $(DISK)
.PHONY: test-pref
