# tests/settings2.mk -- gates for /bin/pref (the coreutil onto the "app."
# preference namespace) and the window-geometry consumer wired through it
# (c/apps/gui/textedit/textedit.c). See c/apps/coreutils/pref.c's header comment for
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


# Settings is split into private includes so the shared APP_RULE and its link
# recipe stay authoritative; each include must still invalidate its object.
$(BUILD)/apps/settings.o: $(wildcard c/apps/gui/settings/*.inc) c/apps/gui/settings/model.h

.PHONY: test-settings-draft test-settings-draft-negctl
test-settings-draft: test-settings-draft-negctl
	@python3 tests/unit/settings_model_test.py
test-settings-draft-negctl:
	@python3 tests/unit/settings_model_test.py --negative

# Runtime artwork is packaged with the OS, never loaded from a host-only path.
#
# ui-art-assets (design_language.md 3.7.4, 3.7.5): the application icons are GENERATED, not
# tracked. tools/mkicons draws the 13 masters (12 apps plus the Logit mark tile) from the SVG
# path data in tools/mkicons/glyphs_svg.inc with c/lib/gfx and the in-tree PNG encoder, into
# $(BUILD)/icons/<stem>.png; they are packed as /icons/<stem>.png, the names the Dock, Finder and
# Settings already load. fsroot/icons/settings.png and preview.png were removed with this change
# (mkfs.py aborts on a duplicate path). Gallery and Widgets get an icon too: test-only APPS, but
# the Dock and Finder draw them wherever they run.
MKICONS := $(BUILD)/tools/mkicons
MKICONS_SRC := tools/mkicons/mkicons.c tools/mkicons/svgpath.c tools/mkicons/glyphs_svg.inc c/include/ui/logit_apps.h
APP_ICON_STEMS := files settings browser clock monitor preview studio assistant terminal textedit gallery widgets logit
APP_ICON_FILES := $(addprefix $(BUILD)/icons/,$(addsuffix .png,$(APP_ICON_STEMS)))
# A host tool: it links c/lib/gfx (the coverage rasteriser the OS draws with) and the Rust PNG
# encoder (tests/pngenc.mk checks it). -ffp-contract=off keeps the art byte-identical per host.
$(MKICONS): $(MKICONS_SRC) $(GFX_SRC) $(GFX_HEADERS) tests/unit/rust_host_shim.c $(RUST_LIB_HOST)
	@mkdir -p $(dir $@)
	$(CC) -O2 -ffp-contract=off -Wall -Wextra -Wno-missing-field-initializers -Ic/include $(GFX_INC) -Itools/mkicons \
	    tools/mkicons/mkicons.c $(GFX_SRC) tests/unit/rust_host_shim.c $(RUST_LIB_HOST) -lm -o $@
$(BUILD)/icons/%.png: $(MKICONS)
	@mkdir -p $(dir $@)
	$(MKICONS) --only $* $(BUILD)/icons
FS_FILES += $(foreach icon,$(APP_ICON_FILES),$(icon):/icons/$(notdir $(icon)))
$(DISK): $(APP_ICON_FILES)

.PHONY: test-app-icons test-app-icons-negctl
test-app-icons: test-app-icons-negctl $(APP_ICON_FILES)
	@python3 tests/unit/app_icon_test.py
	@python3 -I tests/unit/ui_art_check.py --icon-set $(BUILD)/icons
test-app-icons-negctl: $(APP_ICON_FILES)
	@python3 tests/unit/app_icon_test.py --negative
	@python3 -I tests/unit/ui_art_check.py --icon-set $(BUILD)/icons --negative

test-settings-ui: test-settings-draft test-app-icons
