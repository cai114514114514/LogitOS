# --- power control: can this machine be ASKED to stop? -----------------------
#
# In its own fragment rather than in the Makefile, for the reason every other
# fragment header in this file gives: several lines share this tree and a
# whole-file Makefile overwrite from a concurrent line has silently deleted
# other people's targets before. The only tokens this feature needs in the
# shared Makefile are its `-include` and `poweroff reboot` in the CLI list.
#
#   make test-power          the whole gate: poweroff syncs + goes down + QEMU
#                            exits by itself; a second boot proves the write
#                            survived AND the journal had nothing to replay;
#                            a third boot proves reboot round-trips.
#   make test-power-negctl   THE NEGATIVE CONTROL. Same scratch-disk write,
#                            but SIGKILLed instead of asked to power off.
#                            Required to see a replay or an fsck finding on
#                            the next boot -- proof the no-replay assertion
#                            above is capable of failing, not a tautology.

test-power: $(ISO) $(DISK)
	bash tests/boot/run-power-test.sh $(ISO) $(DISK)

test-power-negctl: $(ISO) $(DISK)
	bash tests/boot/run-power-test.sh $(ISO) $(DISK) --negative

.PHONY: test-power test-power-negctl
