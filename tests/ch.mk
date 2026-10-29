# --- ch: the chat window, and the SSE reader behind it -----------------------
#
# Own fragment for the reason every fragment above it gives: several lines share
# this tree and a whole-file Makefile overwrite from a concurrent one has
# silently deleted other people's targets more than once. The tokens this needs
# in the shared Makefile are listed at the bottom of this file and there are
# three of them.
#
#   make test-ch-host     GATE A. The SSE reader against a recorded stream,
#                         replayed whole, one byte at a time, and once for every
#                         two-way split of it. Seconds, no emulator, no key,
#                         no network.
#   make test-ch-negctl   THE NEGATIVE CONTROL. The same gate against a reader
#                         that assumes every read begins on a line boundary --
#                         the one everybody writes first. Required to FAIL, and
#                         required to fail for that reason only: the whole-input
#                         case still passes there.
#   make test-ch          GATE B. Boots LogitOS, serves a canned SSE response
#                         from the host, drives the app, and photographs the
#                         window TWICE DURING the stream to show the text
#                         growing. No key: the mock endpoint needs none.
#   make test-ch-refusal  The no-config path: /etc/ai.conf absent, and the
#                         window must say so, specifically, instead of hanging
#                         or coming up blank.

CH_SRC   := c/apps/gui/ch/ch.c c/apps/gui/ch/ch_sse.c
CH_OBJ   := $(patsubst c/apps/gui/ch/%.c,$(BUILD)/apps/%.o,$(CH_SRC))
CH_AEX   := $(BUILD)/ch.aex
CH_H1OBJ := $(BUILD)/r3netobj/c/net/http/http1.o

# ch.aex is packed under /bin, NOT in the LogitFS root, and that is deliberate.
# The Dock's order is the order the .aex files land in the root, and
# tests/qmp/qmp_ui.py's NAPPS/BROWSER_SLOT/GALLERY_SLOT/SETTINGS_SLOT are
# indices into it -- so a twelfth root .aex moves every icon that four existing
# QMP drivers click by number. /bin is the same place /bin/login and
# /sbin/greeter.aex live: a real GUI app that the Dock does not scan. It is
# launched with SYS_OPEN_PATH (the Finder's own file association -- see
# ends_aex() in c/kernel/gui/wm.c), which is what fsroot/as/examples/chlaunch.as
# does and what the Terminal does when you click a .aex.
#
# If it should be in the Dock instead, the change is this line's dest (`ch.aex`
# rather than `/bin/ch.aex`) plus NAPPS 11 -> 12 in tests/qmp/qmp_ui.py -- and
# it must be packed AFTER settings, or BROWSER_SLOT/GALLERY_SLOT/SETTINGS_SLOT
# all shift.
CH_PACK := $(CH_AEX):/bin/ch.aex c/apps/gui/ch/ai.conf.example:/etc/ai.conf.example

# --- the app --------------------------------------------------------------
# Not APP_RULE: that rule compiles exactly one .c and links crt0 + aui + gfx.
# This one is two .c files and it links mini-libc and the real HTTP/1.1 client
# (c/net/http/http1.c, already built freestanding by the $(BUILD)/r3netobj rule
# that check-ring3-net and /bin/h2check use). Preview and Terminal have the same
# shape for the same reason.
#
# 0x44000000 is the link base. Every GUI app gets a distinct one inside the
# private user region 0x40000000-0x7FFFFFFF; 0x44 was the one gap left between
# terminal (0x43) and browser (0x45).
# These objects used to be side effects of ch.elf's link recipe: updating an
# HTTP or graphics object therefore compiled both Chat sources again. Real
# targets let the compiler's .d files own header edges; the product missing-.d
# guard recovers old objects that predate that dependency information.
$(CH_OBJ): $(BUILD)/apps/%.o: c/apps/gui/ch/%.c
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@

$(BUILD)/ch.elf: $(CH_OBJ) $(APPDIR)/crt0.asm \
                 $(AUI_OBJ) $(GFX_OBJ) $(CH_H1OBJ) $(LIBC_OBJS)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/ch.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x44000000 -o $@ --start-group \
	    $(BUILD)/apps/ch.crt0.o $(CH_OBJ) \
	    $(AUI_OBJ) $(GFX_OBJ) $(CH_H1OBJ) $(LIBC_OBJS) --end-group

$(CH_AEX): $(BUILD)/ch.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/ch.elf $@ Chat - '@' 120 180 220






# --- WHAT THE SHARED MAKEFILE NEEDS, and it is four lines --------------------
#
# Everything else lives in this file. The split follows tests/jsperf.mk, which
# defines $(JSBENCH_PACK) for the $(DISK) recipe the same way: a recipe is
# expanded when it RUNS, so a variable an -include'd fragment defines at the
# bottom of the Makefile still reaches a recipe written near the top. A
# PREREQUISITE list is not -- it is expanded when the rule is read -- which is
# why line 1 below has to be in the Makefile itself and cannot move here.
#
#   1. beside SETTINGS_AEX (Makefile ~411), so the name exists before the
#      $(DISK) rule is parsed:
#          CH_AEX := $(BUILD)/ch.aex
#      (this fragment re-states it, harmlessly and identically, so that a
#      `make -f` wrapper that includes only Makefile + this file still builds.)
#
#   2. in the $(DISK) prerequisite list (Makefile ~973), append:
#          $(CH_AEX)
#
#   3. in the $(DISK) mkfs recipe, as its own continuation line next to
#      $(JSBENCH_PACK) (Makefile ~999):
#          $(CH_PACK) \
#
#   4. at the bottom, with the other fragments:
#          -include tests/ch.mk
#
# Nothing else changes: no APP_RULE line (this app is two .c files and links
# mini-libc + http1.c, so it has its own rule above, exactly as Preview and
# Terminal do), no entry in $(APPS) (that list is the Dock's order and this app
# is packed under /bin), and no new CLI.
#
# OPTIONAL, for CI: add `test-ch-host test-ch-negctl` to whichever host sweep
# tests/ci.mk runs. They need no emulator and take under a second.
