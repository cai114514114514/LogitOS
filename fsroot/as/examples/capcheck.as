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

def attempt(what, f):
    # Returns "ok" or "denied", never propagates. `f` is a zero-arg closure --
    # M22's closures are what make this readable; without them each case would
    # need its own try block inline.
    try:
        f()
        return "ok"
    except e:
        return "denied"

print("capcheck: start")

# --- what this process holds, as a value -------------------------------------
# caps() reports the grant, it does not create one: the bits are whatever the
# kernel handed this process at exec. Printing them first makes the rest of the
# output interpretable -- a refusal below means something different depending on
# what was held here.
c = caps()
print("bits", c.bits())
print("path", c.path())

# --- the assertion ------------------------------------------------------------
print("read-etc", attempt("read /etc", lambda: open("/etc/logit.conf", "r")))
print("read-usr", attempt("read /usr", lambda: open("/usr/as/lib/sys.as", "r")))
print("raw-peek", attempt("peek", lambda: peek8(0x1000)))

# --- attenuation is real on the device too ------------------------------------
# Narrow to strictly less than we hold and confirm the narrowing took. If the
# process holds nothing, scope() itself is refused, which is also correct and is
# why this is wrapped.
try:
    n = c.scope("/usr/as")
    print("narrowed", n.path())
    # And the property that makes a chain safe: you cannot climb back up.
    try:
        n.scope("/")
        print("REGAINED-ROOT")     # must never print
    except e:
        print("no-regain ok")
except e:
    print("narrow denied")

print("capcheck: done")
