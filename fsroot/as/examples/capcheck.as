# aether: 3.0
# capcheck -- M28's headline assertion, on the real machine:
# a script that was not granted CAP_FS provably cannot read /etc.
#
# THIS IS THE ONE M28 CLAIM A HOST TEST CANNOT MAKE. tests/unit/as_cap_test.c
# proves the language half -- attenuation only ever narrows, a region raises
# instead of faulting, a denied native refuses catchably -- with 4152 checks.
# But every one of those runs against a held set the test itself installed
# through as_caps_set(), which is C. What it cannot show is that the set a
# script runs under is the one the KERNEL granted it, because on the host there
# is no kernel to disagree.
#
# So this program asserts the end of the chain: with the grant it has, the calls
# it should be able to make succeed, the calls it should not fail, and the
# failures are catchable rather than fatal. The harness
# (tests/boot/run-as-cap-test.sh) spawns it twice with different grants and
# requires different answers -- two runs of the same source, because a single
# run proving "it was refused" is equally consistent with the file simply not
# existing.
#
# EVERY REFUSAL IS CAUGHT, NOT FATAL. A program that dies on the first denial
# proves only that it died. The whole point of a capability error being an
# ordinary language exception (M22.4 try/except, reused deliberately rather than
# inventing a second error channel) is that a program can be refused and keep
# going -- so this one is refused, says so, and carries on to the next check.

def attempt(what: str, f: Callable[[], None]) -> str:
    # The old helper caught every error and never propagated. A3 catches only
    # permission refusals here: a missing file or broken read is a failed test,
    # not evidence of denied authority. The zero-argument callback keeps each
    # operation separate while sharing this classification rule.
    try:
        f()
        return "ok"
    except PermissionError:
        return "denied"


def _read_file(path: str) -> None:
    # A3 ports have unique ownership. Close the descriptor on every exit;
    # an ignored open() result would make this example teach a resource leak.
    # Only PermissionError means "denied": a missing fixture must fail loudly.
    with file = open(path, "r"):
        file.readall()


def _peek_owned() -> None:
    # The original fixture tried 0x1000, which is mapped to the kernel and
    # faults in ring 3. Read a byte this process owns, keeping the owner live.
    # unsafe permits the pointer operation but does not grant CAP_RAW.
    owned = Bytes("capcheck-owned-byte")
    unsafe:
        assert peek8(addr(owned)) == 99


def main() -> None:
    print("capcheck: start")

    # caps() reports the grant inherited at exec; it cannot manufacture one.
    # Keep both bits and scope visible so the denial lines have a referent.
    held = caps()
    print("bits", held.bits())
    print("path", held.path())

    print("read-etc", attempt("read /etc", lambda: _read_file("/etc/logit.conf")))
    print("read-usr", attempt("read /usr", lambda: _read_file("/usr/as/lib/sys.as")))
    print("raw-peek", attempt("peek", _peek_owned))

    # Scope attenuation creates a weaker value; it never changes the kernel
    # process grant or grants missing bits. Even a zero-bit value may narrow
    # its metadata, but cannot widen its path afterwards.
    try:
        narrowed = held.scope("/usr/as")
        print("narrowed", narrowed.path())
        try:
            narrowed.scope("/")
            print("REGAINED-ROOT")     # must never print
        except PermissionError:
            print("no-regain ok")
    except PermissionError:
        print("narrow denied")

    print("capcheck: done")
