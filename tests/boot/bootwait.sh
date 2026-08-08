# Wait for the serial shell to exist, instead of guessing how long that takes.
#
# WHAT THIS FIXES, AND WHY A BIGGER `sleep` IS THE SAME BUG
# --------------------------------------------------------
# Every boot harness in this tree drives the machine by piping a script into
# QEMU's stdin:
#
#     { sleep 5; printf 'do this\ndo that\n'; sleep 12; } | qemu ... >"$LOG"
#
# The `sleep 5` is a guess at how long the kernel needs to reach `/bin/sh`.
# Bytes that arrive before the shell exists are DROPPED -- there is nothing
# reading the serial port yet -- so when the guess is wrong the harness reports
# that its commands "never finished", with nothing on the console to say why and
# nothing that points at the boot. That failure has already happened once: the
# machine now needs roughly twenty-five seconds to reach a prompt (SMP bring-up,
# PCI enumeration, the filesystem's mount-time fsck, ~2.2 MB of fonts off the
# disk), and eight harnesses were still sleeping five.
#
# Raising the number is not the fix. It is the same assumption with a bigger
# constant: it still breaks the next time the boot gets slower, it still fails
# in a way that reads as "the filesystem is broken" rather than "we typed too
# early", and it makes every one of those harnesses slower on a fast machine to
# pay for the slowest one. The fix is to WAIT FOR THE THING: the shell prints a
# banner on the serial console when it starts, and that banner is proof both
# that the kernel booted and that something is now reading our bytes.
#
# The pipeline makes this easy: the left-hand side and QEMU run concurrently, so
# the group that types the commands can watch the log QEMU is writing.
#
# USAGE
#     . "$(dirname "$0")/bootwait.sh"
#     { logit_wait_for_shell "$LOG" 120; printf '%s' "$cmds"; sleep 12; } | qemu ...
#
# ON TIMEOUT it prints a loud line naming the boot as the suspect and RETURNS
# ANYWAY, so the harness proceeds exactly as it would have with a plain sleep
# and then fails on its own assertions. Two reasons: this file must not be able
# to turn a passing harness into a failing one, and the useful output when a
# machine will not boot is the serial log the harness is about to print, not an
# early exit that discards it.

# Marker: printed by c/apps/coreutils/sh.c on the non-interactive (serial) path,
# immediately before its first read of fd 0. Deliberately a substring of
# "LogitOS shell -- type 'help'" so a change to the help text cannot silently
# stop every harness in the tree from waiting.
LOGIT_SHELL_BANNER="${LOGIT_SHELL_BANNER:-LogitOS shell}"

# logit_wait_for_shell <logfile> [timeout-seconds] [marker]
logit_wait_for_shell() {
    _lw_log="$1"
    _lw_timeout="${2:-120}"
    _lw_marker="${3:-$LOGIT_SHELL_BANNER}"
    _lw_i=0
    _lw_n=$((_lw_timeout * 10))

    while [ "$_lw_i" -lt "$_lw_n" ]; do
        if grep -aq "$_lw_marker" "$_lw_log" 2>/dev/null; then
            # The banner is written before the shell's first read() is posted.
            # A short settle here is not a guess at the boot time -- it is one
            # scheduling quantum, and it is bounded by something that has
            # already been observed rather than by hope.
            sleep 0.5
            return 0
        fi
        sleep 0.1
        _lw_i=$((_lw_i + 1))
    done

    echo "WARNING: no '$_lw_marker' on serial after ${_lw_timeout}s." >&2
    echo "         The shell never came up, so the commands below were typed into" >&2
    echo "         nothing and any failure after this line is a BOOT failure, not" >&2
    echo "         a failure of what this harness tests." >&2
    return 0
}

# Same, for a marker other than the shell banner (a harness that drives
# something which is not /bin/sh). Kept as a separate name so a reader of a
# harness can see at a glance which of the two it is waiting for.
logit_wait_for_marker() { logit_wait_for_shell "$1" "${3:-120}" "$2"; }
