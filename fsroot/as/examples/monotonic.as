# aether: 3.0
# monotonic.as -- is SYS_MONOTONIC_MS a clock, or a constant?
#
# The interesting failure of a new time source is not "it returns garbage", it
# is "it returns 0 forever" or "its unit is ticks and someone called them
# milliseconds". A reading you print once cannot tell those apart. So this
# measures the new clock against an INDEPENDENT one -- the CMOS wall clock,
# which is a different device with a different divisor -- and prints both, for
# tests/boot/run-clock-test.sh to compare.
#
#   MONO <t0> <t1> <delta_ms> <wall_s> <spins>
#
# Expected: delta_ms is about wall_s * 1000. If the unit were ticks it would be
# 100x smaller; if the clock were dead, 0. <spins> is how many times the wait
# loop went round -- it separates "the clock stopped" (many spins, no delta)
# from "the wall clock jumped and the measurement never happened" (no spins),
# which look identical in the delta alone.
# Correction to the old "100x smaller" claim above: with a 100 Hz PIT,
# one tick is 10 ms, so returning ticks as milliseconds is a 10x error.
# The native guest gate executes that exact mistake as a negative control.

from abi import monotonic_ms, get_time, Time, sys_yield


# Seconds since midnight off the RTC. Deliberately NOT monotonic_ms(): asking a
# clock to grade itself proves nothing.
def wall_s(sample: Time) -> i64:
    # A failed read must not reuse the previous RTC sample: that would make
    # a working monotonic clock look stuck when the actual failure was I/O.
    if get_time(sample) < 0:
        raise IOError("Cannot read the independent RTC")
    return (i64(sample.hour) * 60 + i64(sample.minute)) * 60 + i64(sample.second)


def main() -> None:
    sample = Time()
    t0 = monotonic_ms()
    w0 = wall_s(sample)

    # Wait for the wall clock to advance four whole seconds. The RTC only exposes
    # whole seconds, so N reported seconds means the true elapsed time is somewhere
    # in (N-1, N+1) -- and the test's tolerance has to be that window. Four rather
    # than two only to shrink that quantization relative to the measurement: at N=2
    # the honest bound spans 1s..3s, which a clock running at half or double speed
    # can hide inside.
    #
    # The spin cap exists so a DEAD wall clock ends the run with bad numbers instead
    # of hanging a CI job forever: a test that hangs on failure reports nothing.
    spins = 0
    while true:
        w = wall_s(sample)
        if w < w0:
            w = w + 86400                 # RTC rolled past midnight mid-measurement
        if w - w0 >= 4:
            break
        spins = spins + 1
        if spins > 8000000:
            break
        sys_yield()

    t1 = monotonic_ms()
    wall = wall_s(sample) - w0
    if wall < 0:
        wall = wall + 86400

    print("MONO", t0, t1, t1 - t0, wall, spins)

    # The unit is milliseconds; the STEP is 10 ms, because the counter is derived
    # from a 100 Hz tick. Printed rather than hidden: a caller that measures a 3 ms
    # interval and reads 0 deserves to have been told why, and this is the number
    # that says so. (Both readings land on a 10 ms boundary -- if they ever do not,
    # the kernel has stopped deriving this from the PIT and every comment about
    # granularity in the ABI is now wrong.)
    print("MONO-STEP", t0 % 10, t1 % 10)
