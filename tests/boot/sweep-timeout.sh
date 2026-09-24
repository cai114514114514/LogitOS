# Sourced by sweep-targets.sh, sweep-confirm.sh and sweep-resume.sh.
#
# `timeout` is GNU coreutils. Stock macOS -- this tree's documented host -- has
# none, and without it every sweep row read `timeout: command not found`, exit
# 127, FAIL: on 2026-09-24 `make test-sweep-host` reported 1666 of 1666 host
# targets failing having run none of them. That is the shape the top of
# CLAUDE.md warns about: a missing host capability reported as a verdict on the
# code. So: GNU timeout if present, gtimeout (brew coreutils) next, and
# otherwise perl -- in the base system -- running the command in its own
# process group, so a timed-out make takes its QEMU with it, and answering 124
# on expiry exactly as coreutils does, which is what the callers test for.
if ! command -v timeout >/dev/null 2>&1; then
    if command -v gtimeout >/dev/null 2>&1; then
        timeout() { gtimeout "$@"; }
    elif command -v perl >/dev/null 2>&1; then
        timeout() {
            perl -e '
                my $t = shift;
                my $pid = fork;
                die "fork: $!" unless defined $pid;
                if (!$pid) { setpgrp(0, 0); exec @ARGV or exit 127; }
                $SIG{ALRM} = sub { kill "TERM", -$pid; sleep 2; kill "KILL", -$pid; exit 124; };
                alarm $t;
                waitpid($pid, 0);
                exit(($? & 127) ? 128 + ($? & 127) : $? >> 8);
            ' "$@"
        }
    else
        echo "sweep: no timeout, gtimeout or perl on this host -- cannot bound a target; refusing" >&2
        exit 2
    fi
fi
