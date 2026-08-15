/* M28 host battery: capabilities, bounded regions, and slices.
 *
 * WHY THIS IS A SECOND BINARY AND NOT MORE CASES IN as_test.c. Half of what M28
 * has to prove is not reachable from a script AT ALL, and that is the design,
 * not an inconvenience: as_caps_set() and as_cap_attenuate() have no
 * script-visible entry point, because a capability a script can construct is not
 * a capability. So the attenuation lattice below is driven from C, against the
 * same objects the VM uses. Mixing that into as_test.c -- which is entirely
 * "run this snippet, assert its output" -- would blur the one line that makes
 * the property true.
 *
 * THE TWO NEGATIVE CONTROLS LIVE OR DIE HERE. `make test-as-region-negctl`
 * rebuilds this file with -DAS_REGION_NO_BOUNDS and REQUIRES it to fail;
 * `make test-as-cap-negctl` does the same with -DAS_CAP_NO_CHECK. Before this
 * file existed both controls were vacuous -- the tree compiled fine with the
 * checks removed and all 350 as_test checks still passed, because nothing in
 * that battery ever indexed a region out of range or opened a port without a
 * capability. A control nobody has watched fail is not evidence, so every
 * assertion below is written to be the one that breaks.
 *
 * HOST DEFAULT IS DENY (M28 spec D9). A test that forgets to grant fails closed
 * rather than silently running ungated, which is why the very first assertion is
 * that the held set starts empty.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "as.h"

static int fails = 0, total = 0;

static void chk(const char *name, int cond)
{
    total++;
    if (!cond) { fails++; printf("FAIL %s\n", name); }
}

/* Run a snippet and assert its printed output. Caps are set by the caller
 * BEFORE this runs; as_caps_set is process state, not per-interpret. */
static void ok(const char *name, const char *src, const char *want)
{
    char buf[4096];
    as_capture(buf, sizeof buf);
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r != 0) { fails++; printf("FAIL %-22s interpret error: %s\n", name, as_err); as_free_objects(); return; }
    if (strcmp(buf, want) != 0) {
        fails++;
        printf("FAIL %-22s\n  want [%s]\n  got  [%s]\n", name, want, buf);
    }
    as_free_objects();
}

/* Expect the snippet to RAISE. This is the shape the region negative control
 * breaks: with -DAS_REGION_NO_BOUNDS the access clamps instead of raising, the
 * snippet completes, and every one of these reports FAIL -- which is exactly
 * what makes `make test-as-region-negctl` succeed. */
static void raises(const char *name, const char *src)
{
    char buf[4096];
    as_capture(buf, sizeof buf);
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r == 0) { fails++; printf("FAIL %-22s expected a raise, ran to completion\n", name); }
    as_free_objects();
}

/* ------------------------------------------------------------------ caps -- */

/* The one legal way to obtain a root capability: grant the held set, then ask
 * for a Value describing it. as_cap_attenuate cannot be the root -- it
 * dereferences `from` unconditionally, and there is no "attenuate from
 * nothing". */
static ObjCap *root_cap(uint32_t bits, const char *prefix)
{
    as_caps_set(bits, prefix);
    return as_caps_value();
}

/* as_cap_attenuate's contract (see its comment in object.c): `from` must be
 * reachable from a GC ROOT for the whole call, and the function does not
 * protect it -- allocating the new capability can collect. Inside the VM that
 * is free, because the receiver sits on the value stack. Here in C it is not,
 * so every call goes through this. Getting it wrong is invisible in a normal
 * build and a use-after-free under -DAS_GC_STRESS, which is exactly the kind of
 * bug this battery is supposed to catch rather than contain. */
static ObjCap *narrow(ObjCap *from, uint32_t bits, const char *prefix)
{
    if (!from) return NULL;
    as_gc_protect((Obj *)from);
    ObjCap *n = as_cap_attenuate(from, bits, prefix);
    as_gc_release(1);
    return n;
}

static void cap_lattice(void)
{
    /* DEFAULT DENY, asserted before anything grants. If this ever fails, every
     * other capability assertion in this file is meaningless, because they would
     * all be running against a process that already holds everything. */
    chk("default-deny-bits",   as_caps_bits() == 0);
    chk("default-deny-have",   !as_caps_have(AS_CAP_FS_READ));
    chk("default-deny-raw",    !as_caps_have(AS_CAP_RAW));

    const uint32_t ALL = AS_CAP_FS_READ | AS_CAP_FS_WRITE | AS_CAP_NET |
                         AS_CAP_PROC | AS_CAP_GUI | AS_CAP_RAW;

    /* MONOTONICITY OVER THE WHOLE BITMAP LATTICE, not a handful of hand-picked
     * pairs. For every subset `want` of every held set `held`: attenuating to
     * `want` must succeed and yield exactly `want`; attenuating to anything with
     * a bit NOT in `held` must be refused outright. 64 x 64 = 4096 ordered pairs,
     * which is the whole lattice for six bits, so there is no "interesting case"
     * left to have overlooked. */
    for (uint32_t held = 0; held <= ALL; held++) {
        if (held & ~ALL) continue;
        ObjCap *base = root_cap(held, NULL);
        if (!base) { chk("lattice-root", 0); as_free_objects(); continue; }
        for (uint32_t want = 0; want <= ALL; want++) {
            ObjCap *got = narrow(base, want, NULL);
            int subset = ((want & ~held) == 0);
            if (subset) {
                if (!got || got->bits != want) {
                    fails++; total++;
                    printf("FAIL lattice held=%02x want=%02x: %s\n", held, want,
                           got ? "wrong bits" : "refused a legal narrowing");
                    break;
                }
            } else if (got) {
                fails++; total++;
                printf("FAIL lattice held=%02x want=%02x: WIDENED (got %02x)\n",
                       held, want, got->bits);
                break;
            }
            total++;   /* one lattice pair, checked */
        }
        as_free_objects();
    }

    /* PREFIX NARROWING. The boundary cases are the whole point: a substring test
     * would let "/usr" admit "/usrx", which is a different directory. */
    struct { const char *held, *want; int allowed; } pfx[] = {
        { "/usr",  "/usr",         1 },   /* equal */
        { "/usr",  "/usr/as",      1 },   /* deeper */
        { "/usr",  "/usr/",        1 },   /* trailing slash, same directory */
        { "/usr",  "/usrx",        0 },   /* THE substring trap */
        { "/usr",  "/usr2/x",      0 },
        { "/usr",  "/",            0 },   /* widening to root */
        { "/usr",  "/etc",         0 },   /* sibling */
        { "/usr",  NULL,           0 },   /* NULL == "/" == widening */
        { "/usr/as", "/usr",       0 },   /* widening by one component */
        { NULL,    "/usr",         1 },   /* from root, narrowing is fine */
        { NULL,    NULL,           1 },
    };
    for (unsigned i = 0; i < sizeof pfx / sizeof *pfx; i++) {
        ObjCap *base = root_cap(AS_CAP_FS_READ, pfx[i].held);
        ObjCap *got  = base ? narrow(base, AS_CAP_FS_READ, pfx[i].want) : NULL;
        total++;
        if (!!got != !!pfx[i].allowed) {
            fails++;
            printf("FAIL prefix held=%s want=%s: %s\n",
                   pfx[i].held ? pfx[i].held : "(root)",
                   pfx[i].want ? pfx[i].want : "(root)",
                   got ? "allowed, must be refused" : "refused, must be allowed");
        }
        as_free_objects();
    }

    /* ATTENUATION IS NOT A ROUND TRIP. Narrow twice and you cannot get back --
     * the property that makes a capability chain safe to hand to a child. */
    {
        ObjCap *a = root_cap(AS_CAP_FS_READ | AS_CAP_NET, "/usr");
        ObjCap *b = a ? narrow(a, AS_CAP_FS_READ, "/usr/as") : NULL;
        chk("narrow-twice-b", b && b->bits == AS_CAP_FS_READ);
        chk("no-regain-bits", !b || narrow(b, AS_CAP_FS_READ | AS_CAP_NET, "/usr/as") == NULL);
        chk("no-regain-path", !b || narrow(b, AS_CAP_FS_READ, "/usr") == NULL);
        as_free_objects();
    }

    /* permit_path against the HELD set, which is what the syscall-facing side
     * consults. Same boundary question, different entry point -- and they must
     * agree, or the language check and the kernel check disagree about the same
     * path. */
    as_caps_set(AS_CAP_FS_READ, "/usr");
    chk("permit-equal",   as_caps_permit_path("/usr"));
    chk("permit-deeper",  as_caps_permit_path("/usr/as/lib/asc.as"));
    chk("permit-substr",  !as_caps_permit_path("/usrx/evil"));
    chk("permit-sibling", !as_caps_permit_path("/etc/passwd"));
    chk("permit-root",    !as_caps_permit_path("/"));
    /* THE TWO HALVES ARE SEPARATE, AND BOTH ARE REQUIRED. as_caps_permit_path
     * answers ONLY "is this path inside my prefix"; with the bits revoked but no
     * prefix set it still says yes, because an unrestricted prefix does contain
     * every path. That is correct in isolation and a trap in use -- the name
     * reads like a complete answer. Every real call site pairs it with a bits
     * check FIRST (as_port.c's open/redirect paths all call require_cap(...)
     * before as_caps_permit_path(...)), and this pair of assertions pins that
     * contract so a future caller that checks only the path fails here rather
     * than shipping. */
    as_caps_set(0, NULL);
    chk("revoke-drops-bits", !as_caps_have(AS_CAP_FS_READ));
    chk("path-half-alone-is-not-permission", as_caps_permit_path("/usr"));
    as_caps_set(0, "/usr");
    chk("revoked-bits-narrow-prefix-still-refuses-outside", !as_caps_permit_path("/etc"));
}

/* --------------------------------------------------------------- regions -- */

static void regions(void)
{
    as_caps_set(0, NULL);         /* regions need no capability; bounds are not a permission */

    ok("region-is-buffer", "r = region(4)\nprint(len(r))\n", "4\n");
    ok("region-rw",        "r = region(4)\nr[0] = 65\nprint(r[0])\n", "65\n");
    ok("region-neg-index", "r = region(4)\nr[3] = 9\nprint(r[-1])\n", "9\n");
    ok("region-truncates", "r = region(1)\nr[0] = 321\nprint(r[0])\n", "65\n");

    /* THE FOUR THAT BREAK UNDER -DAS_REGION_NO_BOUNDS. Each is an access one
     * past a real edge, which a clamping implementation answers silently. */
    raises("oob-get-high",   "r = region(4)\nprint(r[4])\n");
    raises("oob-get-low",    "r = region(4)\nprint(r[-5])\n");
    raises("oob-set-high",   "r = region(4)\nr[4] = 1\n");
    raises("oob-set-low",    "r = region(4)\nr[-5] = 1\n");

    /* Slices: same contract, two bounds and an ordering constraint. */
    ok("slice-reads",        "r = region(4)\nr[1] = 7\ns = r[1:3]\nprint(len(s))\nprint(s[0])\n", "2\n7\n");
    ok("slice-of-slice",     "r = region(8)\nr[5] = 3\ns = r[2:8]\nt = s[3:6]\nprint(t[0])\n", "3\n");
    raises("slice-past-end", "r = region(4)\ns = r[0:5]\n");
    raises("slice-reversed", "r = region(4)\ns = r[3:1]\n");

    /* Read-only holds for the VALUE, not just the literal `r[a:b] = v` syntax:
     * bound to a plain local there is no colon left for the compiler to see. */
    raises("slice-readonly", "r = region(4)\ns = r[0:2]\ns[0] = 1\n");

    /* CATCHABLE, which is the spec's actual requirement -- "raises a catchable
     * language error instead of a #PF". An error that kills the process would
     * satisfy `raises()` above and fail the milestone. */
    ok("oob-is-catchable",
       "r = region(2)\ntry:\n    print(r[9])\nexcept e:\n    print(\"caught\")\n", "caught\n");

    /* A LIVE SLICE KEEPS ITS PARENT ALIVE. The function returns, popping the only
     * other reference to the buffer off the value stack, and then the collector
     * runs. If the slice's edge back to its parent were weak (or a copy had been
     * made instead of a view, silently changing the semantics), this reads freed
     * memory -- which is why the gate also runs under -DAS_GC_STRESS and ASan. */
    ok("slice-holds-parent",
       "def mk():\n    b = region(8)\n    b[4] = 42\n    return b[4:8]\n"
       "s = mk()\ngc()\nprint(s[0])\n", "42\n");
}

/* ----------------------------------------------------------- CAP_RAW gate -- */

static void raw_gate(void)
{
    /* Denied: every raw-indirection native must refuse without a capability, and
     * refuse CATCHABLY -- not by failing to exist. A native that is simply not
     * registered raises "undefined variable", which a script cannot tell from a
     * typo, and which a script that captured the value earlier would bypass. */
    as_caps_set(0, NULL);

    /* These four are the control's evidence for the raw gate: each is HARMLESS
     * if the gate is removed (addr returns an integer, alloc mallocs, syscall is
     * stubbed to -1 on the host, dealloc frees a pointer we just made), so under
     * -DAS_CAP_NO_CHECK they run to completion and report a clean FAIL. */
    raises("raw-addr-denied",    "r = region(4)\nprint(addr(r))\n");
    raises("raw-alloc-denied",   "print(alloc(8))\n");
    raises("raw-syscall-denied", "print(syscall(0, 0, 0, 0))\n");
    raises("raw-dealloc-denied", "dealloc(0)\n");

    /* These three dereference an address, so with the gate removed they do not
     * FAIL -- they SIGSEGV. A dead process is a nonzero exit and would make
     * `make test-as-cap-negctl` succeed, but it would be proving that the
     * process died rather than that an assertion noticed, which is the exact
     * distinction the negative-control discipline exists to keep. They are
     * asserted in the real build, where the gate stops them before the
     * dereference, and compiled out of the control build. The control's evidence
     * comes from the four above and from the port constructors below. */
#ifndef AS_CAP_NO_CHECK
    raises("raw-peek-denied",    "print(peek8(0))\n");
    raises("raw-poke-denied",    "poke8(0, 0)\n");
    raises("raw-mem2str-denied", "print(mem2str(0, 1))\n");
#endif

    /* Catchability of a denial. Same dereference hazard as the three above --
     * ungated, peek8(0) reads address zero -- so it is compiled out of the
     * control build for the same reason. The catchable-error PROPERTY is
     * covered without a dereference by the region battery's oob-is-catchable. */
#ifndef AS_CAP_NO_CHECK
    ok("raw-denial-catchable",
       "try:\n    peek8(0)\nexcept e:\n    print(\"caught\")\n", "caught\n");
#endif

    /* The natives still EXIST while denied -- proving the gate is a check and
     * not a missing registration. `str` of a native is enough to prove the
     * global resolves; calling it is what is refused. */
    ok("raw-native-exists", "print(len(str(peek8)) > 0)\n", "true\n");

    /* Granted: the same call now reaches the implementation. On the host
     * as_ll_peek is a real dereference, so peek a real address -- the region's
     * own bytes, reached through addr(), which is itself CAP_RAW-gated. */
    as_caps_set(AS_CAP_RAW, NULL);
    ok("raw-granted-roundtrip",
       "r = region(4)\nr[0] = 77\nprint(peek8(addr(r)))\n", "77\n");
    as_caps_set(0, NULL);
}

/* ------------------------------------------------------ port constructors -- */

static void port_gate(void)
{
    /* The half -DAS_CAP_NO_CHECK was already written to remove (as_port.c's
     * seven guards). The paths are chosen so that removing the gate SUCCEEDS
     * rather than failing for an unrelated reason: /dev/null opens on any host,
     * so with the check gone `open()` returns a real port and the assertion
     * reports a clean FAIL. A nonexistent path would raise either way and the
     * control would look like it worked while proving nothing. */
    as_caps_set(0, NULL);
    raises("open-read-denied",  "f = open(\"/dev/null\", \"r\")\n");
    raises("open-write-denied", "f = open(\"/dev/null\", \"w\")\n");
    raises("pipe-denied",       "p = pipe()\n");
    raises("run-denied",        "p = run([\"/bin/true\"])\np.start()\n");

    /* Granted the bit but scoped elsewhere: the path half must still refuse.
     * This is the pair the two-halves comment above pins, exercised end to end
     * through a real constructor rather than through as_caps_permit_path alone. */
    as_caps_set(AS_CAP_FS_READ, "/usr");
    raises("open-outside-scope", "f = open(\"/dev/null\", \"r\")\n");

    /* Granted and in scope: the same call now works, which is what makes the
     * refusals above evidence of a CHECK rather than of a broken open(). */
    as_caps_set(AS_CAP_FS_READ, "/dev");
    ok("open-in-scope", "f = open(\"/dev/null\", \"r\")\nprint(f.closed())\n", "false\n");
    as_caps_set(0, NULL);
}

int main(void)
{
    cap_lattice();
    regions();
    raw_gate();
    port_gate();

    if (fails) { printf("\n%d/%d M28 capability checks FAILED\n", fails, total); return 1; }
    printf("all %d M28 capability checks passed\n", total);
    return 0;
}
