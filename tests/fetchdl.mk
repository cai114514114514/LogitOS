# tests/fetchdl.mk -- the first-byte deadline (browser_rt.c's bfetch).
#
# WHAT THIS GATES. browser_rt.c's BF_REQ_MS (60 s) is a per-attempt IDLE
# deadline, reset on every hop, and its own comment defends that: a page load
# has long gaps between phases and a request that is already receiving bytes
# should not be punished for a slow remainder. That defence says nothing
# about a request that has sent its headers and received not one response
# byte -- a server that accepts the handshake and then never writes holds
# one of its origin's TWO connection slots (hpool_config(&g_pool, 6, 2, ...))
# in total silence for the full minute, and anything queued behind it on
# that origin waits too. BF_FIRSTBYTE_MS (12 s) is a second, shorter clock
# that applies only before the first response byte and is disarmed forever
# the instant one arrives -- see its comment in browser_rt.c for the
# evidence the 12 s figure is chosen from and the argument that it cannot
# become a third factor in the BF_HOPS x BF_REQ_MS trap the file already
# names (t_start / BF_TOTAL_MS still bounds everything, unchanged).
#
# ONE TARGET, ITS NEGATIVE CONTROL A PREREQUISITE (AGENTS.md rule: a control
# not wired as a prerequisite of its positive runs never):
#
#   test-fetch-firstbyte          guest: a page with two same-origin scripts,
#                                  stall.js (accepts the connection, never
#                                  writes) and slow5.js (first byte at 5 s).
#                                  stall.js must be named in a "[browser]
#                                  fetch stalled: no response after 12 s:
#                                  ...stall.js" line and cut within ~12 s of
#                                  guest time; slow5.js must NOT be cut (the
#                                  honest negative -- an alive-but-slow
#                                  request is not the same bug as a dead one)
#                                  and the page's own load event (SF-END)
#                                  must still fire.
#   test-fetch-firstbyte-negctl    guest: the SAME page against a browser
#                                  built -DBFETCH_NO_FIRSTBYTE_DEADLINE
#                                  (browser-nofirstbyte.aex, its own disk
#                                  image, mirroring tests/webaccel.mk's waoff
#                                  pattern). stall.js must NEVER be named in
#                                  a "fetch stalled" line and must take close
#                                  to the full BF_REQ_MS (60 s) idle deadline
#                                  to fail -- a fast failure here would mean
#                                  the positive gate is not measuring this
#                                  feature.
#
# WATCHED RED, recorded here verbatim (2026-09-02, browser-nofirstbyte.aex,
# -DBFETCH_NO_FIRSTBYTE_DEADLINE -- i.e. exactly what test-fetch-firstbyte-negctl
# runs on purpose): stall.js was never named in any "fetch stalled" line;
# "[wa] t=76010 (+60740) idle reqs=3 dials=2 reuses=1" and "[browser] script
# LOST: http://10.0.2.2:PORT/stall.js: timed out (status 0)" show the ordinary
# BF_REQ_MS idle deadline firing at a real 60740 ms of GUEST time (browser_rt.c's
# own "[wa] ... nav" / "[wa] ... loadend" stamps, t_loadend - t_nav -- see
# qmp_fetchdl.py's module docstring for why THAT pair and not "[wm] perf
# t=", which this driver tried first and which silently stopped printing for
# the whole 60 s stall because the browser held the netlock across it).
# WATCHED GREEN with BF_FIRSTBYTE_MS compiled in (ordinary $(DISK) build):
# "[browser] fetch stalled: no response after 12 s: http://10.0.2.2:PORT/
# stall.js", slow5.js never named, SF-END printed, load finished in ~15 s of
# guest time by the same t_loadend - t_nav measurement.
#
# WHAT THIS DOES NOT GATE: TLS, HTTP/2 multiplexing, or redirect-hop
# behaviour -- those are http1_test.c/hpool_test.c/range.mk's own gates.
# This fragment is the one property that matters here: a request that never
# answers cannot hold an origin's connection slot indefinitely while a
# request that DOES answer, slowly, is left alone.

.PHONY: test-fetch-firstbyte test-fetch-firstbyte-negctl

# --- guest: the positive gate, ordinary $(DISK) build -----------------------
test-fetch-firstbyte: test-fetch-firstbyte-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_fetchdl.py --iso $(ISO) --disk $(DISK) \
	    --out $(BUILD)/fetchdl-os.json

# --- guest negctl: the browser with BF_FIRSTBYTE_MS compiled OUT ------------
# Same shape as tests/webaccel.mk's browser-waoff.elf: rebuild ONLY
# browser_rt.o with the guard macro defined, relink browser.aex against every
# other object unchanged, pack it onto its own disk image.
$(BUILD)/nofirstbyte/c/apps/browser/browser_rt.o: c/apps/browser/browser_rt.c \
        c/apps/browser/http_cache.c c/apps/browser/http_cache.h \
        c/apps/browser/bfetch.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(CSS_INC) -DBFETCH_NO_FIRSTBYTE_DEADLINE -c $< -o $@

NOFIRSTBYTE_OBJ := $(filter-out $(BUILD)/browserobj/c/apps/browser/browser_rt.o,$(BROWSER_OBJ)) \
                   $(BUILD)/nofirstbyte/c/apps/browser/browser_rt.o

$(BUILD)/browser-nofirstbyte.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(NOFIRSTBYTE_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(NOFIRSTBYTE_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-nofirstbyte.aex: $(BUILD)/browser-nofirstbyte.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nofirstbyte.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-fetch-firstbyte-negctl: $(ISO) $(BUILD)/browser-nofirstbyte.aex
	@$(MAKE) DISK=$(BUILD)/disk-nofirstbyte.img BROWSER_AEX=$(BUILD)/browser-nofirstbyte.aex $(BUILD)/disk-nofirstbyte.img
	python3 tests/qmp/qmp_fetchdl.py --iso $(ISO) --disk $(BUILD)/disk-nofirstbyte.img \
	    --out $(BUILD)/fetchdl-os-negctl.json --expect-off

# NOT on ci-boot, deliberately, same reasoning as tests/webaccel.mk's guest
# pair: this is two full QEMU boots (the negctl one waits out a real 60 s
# idle deadline on top of boot time) and ci-boot already carries its own
# fixed set. Run by hand across any change to browser_rt.c's bfetch state
# machine -- a green run that has not been shown red against
# -DBFETCH_NO_FIRSTBYTE_DEADLINE is not evidence (AGENTS.md rule 5).
