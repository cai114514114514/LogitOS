# Network measurement targets.
#
# Kept in their own fragment for the reason tests/nic.mk records: several
# agents edit the Makefile at once and a shared-file edit cannot be committed
# without swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/netperf.mk
#
# START HERE: tests/boot/netwire.py's header explains why `-netdev user` cannot
# measure a TCP window at all (SLIRP terminates the guest's connection, so the
# peer is one emulated hop away at sub-millisecond RTT), and what topology this
# uses instead.

.PHONY: test-net-bench test-net-ab netwire-selftest


# --- the instruments -----------------------------------------------------
# What the host-side wire can carry, before believing any number measured
# through it. Run this first when a throughput figure looks surprising.
netwire-selftest:
	@python3 tests/boot/netwire.py --selftest

# One build, one or more NICs, over the instrumented wire. A MEASUREMENT, not
# a gate. DELAY=<ms> injects a real one-way delay on the guest's own segment
# (RTT added is twice it), which is the thing SLIRP alone cannot provide.
#   make test-net-bench REPS=5 DELAY=25 BYTES=917504
#
# The per-arm serial log, wire report and QEMU-B log are ALWAYS kept, under
# $(BUILD)/net-bench (LOGDIR= overrides). The guest-side [http] and [net] rx
# path lines are in that serial log and nowhere else; they used to be deleted
# on every successful run. TRACE=1 additionally asks netwire for a per-packet
# timeline -- read it for shape only, it lowers the wire's own ceiling.
test-net-bench: $(ISO) $(DISK)
	@bash tests/boot/run-net-bench.sh $(ISO) $(DISK) \
	    --reps $(if $(REPS),$(REPS),5) --delay $(if $(DELAY),$(DELAY),0) \
	    --logdir $(if $(LOGDIR),$(LOGDIR),$(BUILD)/net-bench) \
	    $(if $(TRACE),--trace,) \
	    $(if $(BYTES),--bytes $(BYTES),) $(if $(NIC),--nic $(NIC),)


# Paired A/B between two ISOs. Build the other one however you like (a copy of
# the tree with the files under test reverted is what was used); this only
# needs the two images.
#   make test-net-ab BEFORE=/tmp/logit-before.iso REPS=9
test-net-ab: $(ISO) $(DISK)
	@test -n "$(BEFORE)" || { echo "usage: make test-net-ab BEFORE=<other.iso>"; exit 2; }
	@bash tests/boot/run-net-ab.sh --disk $(DISK) \
	    --arm before:$(BEFORE):$(if $(NIC),$(NIC),e1000) \
	    --arm after:$(ISO):$(if $(NIC),$(NIC),e1000) \
	    --reps $(if $(REPS),$(REPS),9) --delay $(if $(DELAY),$(DELAY),0)
