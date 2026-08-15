# tests/desktop.mk -- the desktop's own login, and the settings store behind it.
#
# Two things landed together because they are one question -- "does this machine
# know who is using it" -- answered at the two places it was still being answered
# "no":
#
#   THE GREETER. /bin/login authenticated the serial console; the window manager
#   launched the file manager before init ran at all. The desktop now comes up
#   LOCKED when /etc/passwd exists, and wm_launch() refuses to start anything but
#   the greeter until somebody authenticates.
#
#   THE PER-USER SETTINGS STORE. settings_commit() wrote root:root 0600
#   /etc/settings.conf with the calling process's credential, so a logged-in user
#   flipping dark mode was refused and the choice was gone at the next boot.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment exists: a whole-file Makefile overwrite from a concurrent line deletes
# targets written straight into it.
#
# Every target here has a RECIPE, which is what makes tools/audit_tests.py
# classify it and tools/ci.sh run it. The two negative controls match NOT_CI and
# are run BY their positive counterparts, which is the convention the audit
# expects -- and, more to the point, is what stops a green suite from meaning
# nothing.

.PHONY: test-usersettings test-usersettings-host test-usersettings-negctl \
        test-desktop-os test-greeter-ui test-greeter-negctl test-desktop

# ---------------------------------------------------------------------------
# HOST: the layering, the ownership bit, the /etc/passwd lookup.
# ---------------------------------------------------------------------------
# ASan + UBSan for the same reason the LogitFS and settings host tests use them:
# this code indexes a caller's buffer from field pointers a parser derived, and
# that is the shape of bug that passes every assertion.
US_CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-function \
             -fsanitize=address,undefined -fno-omit-frame-pointer
US_INC    := -Ic/kernel/core -Ic/fs -Ic/drivers/block -Iinclude/abi -Ic/kernel/mm \
             -Ic/apps/coreutils
US_SRC    := tests/unit/usersettings_test.c c/kernel/core/settings.c c/drivers/block/crc32.c

test-usersettings-host: test-usersettings-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(US_CFLAGS) $(US_INC) -o $(BUILD)/usersettings_test $(US_SRC)
	@$(BUILD)/usersettings_test

# THE NEGATIVE CONTROL, and note what it is NOT: it is not "remove the per-user
# store".
#
# Under -DSETTINGS_NEGCTL_USER_READ_SYSTEM every save is written to the
# per-user path and every read comes from the system one. Setting a value
# appears to work, the value is live for the rest of the session, the UI
# follows it, the commit returns 0 and the file on disk is exactly right --
# and it is gone at the next boot. That is today's bug wearing a fix's clothes,
# and it is indistinguishable from the real thing to any test that does not
# reload. Most of the suite still passes under it; the reload assertions are
# what go red, which is why they are written down as a claim of their own.
#
# This target SUCCEEDS when the suite FAILS.
test-usersettings-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(US_CFLAGS) $(US_INC) -DSETTINGS_NEGCTL_USER_READ_SYSTEM \
	    -o $(BUILD)/usersettings_negctl $(US_SRC)
	@if $(BUILD)/usersettings_negctl > $(BUILD)/usersettings_negctl.log 2>&1; then \
	    echo "CONTROL FAILED: a build that reads the SYSTEM store while writing the"; \
	    echo "                user's passed the suite -- the suite cannot see the bug"; \
	    cat $(BUILD)/usersettings_negctl.log; exit 1; \
	 else \
	    echo "negative control OK -- writing the user's file and reading the system"; \
	    echo "one fails these:"; \
	    grep -A1 FAIL $(BUILD)/usersettings_negctl.log | head -8; \
	    tail -1 $(BUILD)/usersettings_negctl.log; \
	 fi

test-usersettings: test-usersettings-host test-desktop-os

# ---------------------------------------------------------------------------
# ON THE MACHINE, ACROSS FOUR REAL BOOTS, no -snapshot.
# ---------------------------------------------------------------------------
# A setting that survives one boot proves nothing -- it could be a buffer
# nobody flushed. Four boots against one image, with a SECOND USER on the last
# one, because "per-user" is a claim about two people and cannot be tested with
# one. See the harness header for what each boot carries.
test-desktop-os: $(ISO) $(DISK)
	@bash tests/boot/run-desktop-test.sh $(ISO) $(DISK)

# ---------------------------------------------------------------------------
# THE GREETER, DRIVEN LIKE A HUMAN.
# ---------------------------------------------------------------------------
# The boot harness above proves the desktop does not start until somebody
# authenticates, but it authenticates on the SERIAL CONSOLE -- so it says
# nothing about whether the greeter itself accepts a password or refuses one.
# This types into it over QMP, wrong password first.
test-greeter-ui: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_greeter.py $(ISO) $(DISK)

# THE GREETER'S NEGATIVE CONTROL. LOGIN_NEGCTL=1 compiles
# -DLOGIN_NEGCTL_ACCEPT_ANY into BOTH /bin/login and greeter.aex -- one flag,
# one definition of the check (acct_check_password in accounts.h), both doors.
# The prompt appears, the field masks, the timing prints, the desktop unlocks,
# a human sees exactly what they expect, and every password is correct.
#
# THE FLAG HAS TO BE ON THE MAKE THAT RUNS THE HARNESS. `make LOGIN_NEGCTL=1
# build/disk.img && make test-greeter-ui` silently tests the REAL build: the
# second make has no flag, LOGIN_STAMP names .flags-normal, and the disk is
# repacked without the define before the harness sees it. The login line
# documented that trap after writing it into its own instructions; this target
# exists so nobody has to remember it. It is a recipe that re-invokes make with
# the flag set, so there is no two-step to get wrong.
test-greeter-negctl:
	@echo "--- greeter negative control: every password is correct (must fail) ---"
	@if $(MAKE) --no-print-directory LOGIN_NEGCTL=1 test-greeter-ui \
	      > $(BUILD)/greeter_negctl.log 2>&1; then \
	    echo "CONTROL FAILED: a greeter that accepts EVERY password passed the suite"; \
	    tail -20 $(BUILD)/greeter_negctl.log; \
	    $(MAKE) --no-print-directory $(DISK) >/dev/null 2>&1; exit 1; \
	 else \
	    echo "negative control OK -- with every password accepted, this fails:"; \
	    grep -aE "FAIL|REFUS" $(BUILD)/greeter_negctl.log | head -6; \
	 fi
	@# Put the REAL login back in the image. Without this the next target to
	@# depend on $(DISK) gets the accept-anything build and passes for the
	@# wrong reason -- the flag is in the stamp's NAME, so an unflagged make
	@# does rebuild, but only if something asks it to.
	@$(MAKE) --no-print-directory $(DISK) >/dev/null

test-desktop: test-usersettings-host test-desktop-os test-greeter-ui

# ---------------------------------------------------------------------------
# THE LOOK GATE -- a second, unrelated unit appended to this fragment rather
# than given its own tests/*.mk file, because the -include line an orchestrator
# has to add is the scarce resource here (see the file header for why a
# whole-file overwrite is the failure mode every fragment in this tree exists
# to avoid): tests/desktop.mk is ALREADY -included by the Makefile, so adding
# to it costs nothing and duplicating that line for one more target would not.
# Nothing above this comment is touched or depended on by anything below it --
# the greeter/settings targets and this one measure two unrelated things about
# "the desktop" and happen to share a filename for that reason alone.
#
# WHAT THIS MEASURES, AND WHY IT IS A GATE AND NOT A SCREENSHOT: "it looks more
# like macOS" cannot be checked, disagreed with, or regressed against. Every
# claim tests/qmp/qmp_desktop_look.py makes is a NUMBER read off the guest's
# OWN scanout (over QMP, never the host window -- tools/shot.sh's header is the
# canonical statement of why that separation matters) -- how many pixels of
# shadow, how deep, where an edge actually is. Full detail, including the three
# known-broken pixel-level defects this gate deliberately still asserts (so a
# fix -- or a further break -- is a number moving, not a screenshot somebody
# eyeballed) is in that file's own header; this fragment only wires it up.
test-desktop-look: $(ISO) $(DISK)
	@bash tests/boot/run-desktop-look.sh $(ISO) $(DISK)

.PHONY: test-desktop-look
