# tests/jsmicro.mk -- microtask and job-queue ordering, differentially against
# node.  One case = one .js file that PRINTS; stdout is diffed BYTE FOR BYTE.
#
# WHY THIS FRAGMENT EXISTS AT ALL, AND IT IS NOT A COMPLIMENT TO ANYONE.
# tests/jsmicro/ arrived on 2026-08-30 as 82 files -- 74 cases, a watched
# control, three characterisation cases, a host runner, a guest runner, a
# builder and a node oracle -- and NOTHING IN THE TREE NAMED IT.  No Makefile
# line, no .mk, no tool, no script.  It was the 22nd entry in the category
# CLAUDE.md already counts twenty-one of, on the day it was written, and it was
# found only because somebody asked whether the files should be deleted.
#
# It should not have been deleted.  Wired and run for the first time it scored
# 71 ok / 2 real differences with its control firing -- see below.  But a dead
# harness is worse than no harness, because it reads like coverage, so the
# choice was binary: wire it or delete it.  Leaving it was the only wrong
# answer.
#
# SECOND DEFECT, AND IT IS THE ONE THAT WOULD HAVE BITTEN THE NEXT PERSON.
# All three scripts defaulted to `build-jssem/...` -- a scratch tree that
# `make clean-scratch` deletes BY DESIGN.  So even a hand-run would have failed
# on a missing binary minutes after any cleanup, for a reason that has nothing
# to do with the code under test.  The defaults are $(BUILD) now and this
# fragment passes them explicitly.
#
# WHAT IT MEASURES THAT NOTHING ELSE HERE DOES.  tests/jssem.mk covers objects,
# weak refs and built-ins; tests/semantics.mk covers the DOM.  Neither can ask
# WHEN a callback runs.  Every modern framework batches its updates by queueing
# a microtask and assuming it runs after the current synchronous block and
# before anything else, so ordering is not a detail of this platform -- it is
# the mechanism the whole reactive tier stands on.  And ordering is testable as
# a pure string: a program that prints a tag as each callback fires produces an
# ORDER, and the order either matches node or does not.  No clock, no timing,
# no flake.
#
# THE ORACLE IS NODE AND THERE ARE NO EXPECTED-OUTPUT FILES, deliberately.  The
# harness's own header says why and it is the c/apps/libc argument word for
# word: an expectation file records what its author believed, and the reason
# this gate exists is that nobody's belief about microtask ordering is
# reliable.
#
# THE CONTROL IS cases/m99_CONTROL.js, whose output is deliberately made to
# differ.  run.sh FAILS if it does not appear in the difference list -- a
# differential that cannot report a difference is not an instrument.  Watched
# firing on the first wired run:
#     DIFF  m99_CONTROL     -native  +polyfilled
#     control: fired (this harness can report a difference)
#
# WHAT IT FOUND IMMEDIATELY, and both are real:
#   m31_asyncclose        node prints CLEANUP-throw BEFORE AFTER-throw; we
#                         print it after.  An async iterator's return() cleanup
#                         is one microtask turn late on the THROW path (the
#                         break path is correct).  A framework that releases a
#                         subscription in an async iterator's finally sees the
#                         release happen after code downstream assumed it had.
#   m53_async_recursion   node throws RangeError on stack exhaustion, we throw
#                         InternalError.  `catch (e) { if (e instanceof
#                         RangeError) ... }` takes the other branch.  Not
#                         mandated by the spec; universal in practice.
# Neither is fixed here.  This fragment makes them visible and repeatable,
# which is the thing that was missing.
#
# HOST IS THE ITERATION LOOP, GUEST IS THE RUN THAT COUNTS, and run-guest.sh
# states the reason: the host binary is arm64/darwin against a system libc and
# the guest is x86_64 freestanding against a mini-libc arena allocator with
# -DLOGIT_OS, -DCONFIG_STACK_CHECK and -DNDEBUG all live.  That difference is
# not theoretical -- without CONFIG_STACK_CHECK, js_check_stack_overflow is
# `return FALSE` and JS_SetMaxStackSize is inert, so m53 above cannot even
# reach its own question on a host built the ordinary way.  The host rule below
# therefore compiles with the BROWSER's -D set, exactly as tests/jssem.mk's
# js_sem_probe does.  No finding is reported from the host alone.
#
#   make test-jsmicro       the host differential (fast)
#   make test-jsmicro-os    the same cases on the machine, over serial
#
.PHONY: test-jsmicro test-jsmicro-os

JSMICRO_DIR   := tests/jsmicro
JSMICRO_CASES := $(sort $(wildcard $(JSMICRO_DIR)/cases/*.js)) \
                 $(sort $(wildcard $(JSMICRO_DIR)/chars/*.js))

# The browser's -D set, not the host's default. See the note above and
# tests/jssem.mk's identical rule -- two gates asking the same engine the same
# question have to ask the same binary.
$(BUILD)/micro_run: $(JSMICRO_DIR)/micro_run.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w -Ithird_party/quickjs -Ithird_party/libm \
	    -DCONFIG_VERSION='"logit-2024"' -DLOGIT_OS -DCONFIG_STACK_CHECK \
	    -DNDEBUG -o $@ $(JSMICRO_DIR)/micro_run.c $(QJS_SRC) -lm

test-jsmicro: $(BUILD)/micro_run
	@ENGINE=$(BUILD)/micro_run OUT=$(BUILD)/jsmicro-out \
	    bash $(JSMICRO_DIR)/run.sh

# On the machine. NOT verified by the line that wired this fragment -- the host
# half was run and is green-with-two-findings, the guest half is wired from the
# scripts' own documented interface and is stated as unrun rather than claimed.
# Whoever runs it first should replace this paragraph with what happened.
test-jsmicro-os: $(ISO) $(DISK)
	@BUILD=$(BUILD) bash $(JSMICRO_DIR)/build-guest.sh
	@ISO=$(ISO) DISK=$(DISK) OUT=$(BUILD)/jsmicro-guest \
	    bash $(JSMICRO_DIR)/run-guest.sh
