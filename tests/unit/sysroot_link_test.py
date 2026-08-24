#!/usr/bin/env python3
"""Part D of the sysroot: tcc links a program against the PACKED sysroot and
the result is byte-identical to linking the same object against the build
tree's objects directly. Plus the negative control that shows the link is
resolving from the sysroot's archive and nowhere else.

The host-built tcc has CONFIG_SYSROOT compiled in as build/sysroot, so its
DEFAULT search -- the one a user on the device gets with no flags -- is the
packed layout: crt1.o/crti.o/crtn.o from usr/lib, -lc from usr/lib, libtcc1.a
from usr/lib/tcc, headers from usr/lib/tcc/include then usr/include. Nothing
outside build/sysroot is on any of its paths, which is what makes "it linked"
mean something here.

Steps, each a check:

  compile    tcc -c hello.c, under tcc's default include order AND under
             -nostdinc -I usr/include -I usr/lib/tcc/include (the task's
             literal form, and the order the libc was written against). The
             two objects must be byte-identical.
  sysroot    tcc -static -Wl,-Ttext=0x50000000 hello.o -o hello_sys
             No -nostdlib: every crt and library comes from the layout by
             the name tcc asks for. -static because the device has no
             dynamic linker (a non-static tcc link writes PT_INTERP for
             /lib64/ld-linux-x86-64.so.2 and a .dynamic section). -Ttext is
             the CLI base every program on this machine links at
             (Makefile CLI_RULE, 0x50000000: inside the private user region).
  -B form    the same with -B build/sysroot/usr/lib/tcc, as the task spells
             it; must be byte-identical to the above.
  direct     REPLICATE tcc's archive loader (tccelf.c tcc_load_alacarte) over
             libc.a's and libtcc1.a's own "/" symbol indexes to find which
             members it pulls and in what order; then link the SAME hello.o
             with -nostdlib against the BUILD TREE's objects (not the
             archive members) in that order, crt1.o crti.o first and crtn.o
             last. Byte-identical to hello_sys proves two things at once:
             the archive's members are the build tree's objects bit for
             bit, and tcc's a-la-carte loading is the same link as naming
             every object. The replica can only make this gate FAIL, never
             pass, so a divergence between it and tcc is safe.
  NEGCTL     an overlay copy of usr/lib whose libc.a lacks one member
             (the one defining memset, then the one defining sqrt), linked
             with -L overlay -B overlay/tcc: the link must FAIL, and the
             error must name the symbol that member defined. Its positive
             twin -- the overlay with the COMPLETE archive -- must succeed
             and be byte-identical, or "-L was ignored" would pass the
             control for the wrong reason.

usage: sysroot_link_test.py <tcc> <sysroot> <workdir> <hello.c> <libc-objs-list> <libtcc1-objs-list> <nm> <ar>
"""
import os
import shutil
import struct
import subprocess
import sys

tcc, sysroot, work, hello_c, libc_list, tcc1_list, nm, ar = sys.argv[1:9]
sysroot = os.path.abspath(sysroot)
work = os.path.abspath(work)
os.makedirs(work, exist_ok=True)
LIB = os.path.join(sysroot, "usr/lib")
TCCDIR = os.path.join(LIB, "tcc")
TEXT = "0x50000000"
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails += 1


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return r.returncode, (r.stderr + r.stdout).strip()


def same(a, b):
    return open(a, "rb").read() == open(b, "rb").read()


def size(p):
    return os.path.getsize(p)


# ---- the replica of tcc_load_alacarte ---------------------------------------
def ar_index(path):
    """(symbol, member-offset) pairs from the GNU "/" index, in index order --
    exactly what tcc iterates (tccelf.c:2608-2631)."""
    data = open(path, "rb").read()
    assert data[:8] == b"!<arch>\n", path
    off = 8
    while off < len(data):
        name = data[off:off + 16].decode().rstrip()
        sz = int(data[off + 48:off + 58].decode().strip())
        body = data[off + 60:off + 60 + sz]
        if name == "/":
            n = struct.unpack(">I", body[:4])[0]
            offs = struct.unpack(">%dI" % n, body[4:4 + 4 * n])
            names = body[4 + 4 * n:].split(b"\0")[:n]
            return [(nm.decode(), o) for nm, o in zip(names, offs)]
        off += 60 + ((sz + 1) & ~1)
    sys.exit("no GNU symbol index in " + path)


def member_names(path):
    """member-offset -> member name, from the archive headers (long names via
    the // table, which llvm-ar emits for names over 15 bytes)."""
    data = open(path, "rb").read()
    off = 8
    longnames = b""
    out = {}
    while off < len(data):
        name = data[off:off + 16].decode().rstrip()
        sz = int(data[off + 48:off + 58].decode().strip())
        if name == "//":
            longnames = data[off + 60:off + 60 + sz]
        elif name not in ("/", "/SYM64/"):
            if name.startswith("/"):
                i = int(name[1:])
                name = longnames[i:].split(b"/\n")[0].decode()
            else:
                name = name.rstrip("/")
            out[off] = name
        off += 60 + ((sz + 1) & ~1)
    return out


WEAK_UNDEF = set()      # weak references seen anywhere; allowed to stay unresolved


def symbols(obj):
    """(defined, undefined) global symbol sets of an object. Undefined
    includes WEAK undefined: tcc pulls on st_shndx == SHN_UNDEF regardless of
    binding (and a weak one may legitimately stay unresolved, resolving to 0).
    COMMON counts as defined (SHN_COMMON != SHN_UNDEF)."""
    rc, out = run([nm, "-g", "-f", "posix", obj])
    if rc:
        sys.exit("nm failed on %s: %s" % (obj, out))
    d, u = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name, t = parts[0], parts[1]
        if t in ("U", "w", "v"):
            u.add(name)
            if t in ("w", "v"):
                WEAK_UNDEF.add(name)
        else:
            d.add(name)
    return d, u


def pull_order(archive, objs_by_member, defined, undefined):
    """tcc_load_alacarte: repeat over the index in order, loading a member
    the first time one of its symbols is found UNDEFINED in the symtab, until
    a pass loads nothing. Mutates defined/undefined. Returns members loaded."""
    idx = ar_index(archive)
    names = member_names(archive)
    loaded = []
    while True:
        bound = 0
        for sym, off in idx:
            if sym in undefined:
                mem = names[off]
                obj = objs_by_member[mem]
                d, u = symbols(obj)
                defined |= d
                undefined -= d
                undefined |= (u - defined)
                loaded.append(mem)
                bound += 1
        if not bound:
            return loaded


def objs_map(listfile):
    objs = sorted(l.strip() for l in open(listfile) if l.strip())
    m = {}
    for o in objs:
        b = os.path.basename(o)
        if b in m:
            sys.exit("two objects named %s: %s %s" % (b, m[b], o))
        m[b] = o
    return m


print("-- compile")
hello_o = os.path.join(work, "hello.o")
hello_o2 = os.path.join(work, "hello_explicit.o")
rc, out = run([tcc, "-c", hello_c, "-o", hello_o])
check(rc == 0, "tcc -c hello.c (default include order) %s" % out)
rc2, out2 = run([tcc, "-nostdinc", "-I" + os.path.join(sysroot, "usr/include"),
                 "-I" + os.path.join(TCCDIR, "include"), "-c", hello_c, "-o", hello_o2])
check(rc2 == 0, "tcc -nostdinc -I usr/include -I usr/lib/tcc/include -c hello.c %s" % out2)
if rc == 0 and rc2 == 0:
    check(same(hello_o, hello_o2), "both include orders give the same hello.o (%d B)" % size(hello_o))
if rc or rc2:
    print("sysroot_link_test: cannot continue without hello.o")
    sys.exit(1)

print("-- link against the sysroot (tcc's default paths, rooted at build/sysroot)")
hello_sys = os.path.join(work, "hello_sys")
rc, out = run([tcc, "-static", "-Wl,-Ttext=" + TEXT, hello_o, "-o", hello_sys])
check(rc == 0, "tcc -static -Wl,-Ttext=%s hello.o -o hello_sys %s" % (TEXT, out))
hello_b = os.path.join(work, "hello_B")
rc, out = run([tcc, "-B", TCCDIR, "-static", "-Wl,-Ttext=" + TEXT, hello_o, "-o", hello_b])
check(rc == 0, "the -B %s form links %s" % (os.path.relpath(TCCDIR), out))
if os.path.exists(hello_sys) and os.path.exists(hello_b):
    check(same(hello_sys, hello_b), "-B form byte-identical to the default-path form (%d B)" % size(hello_sys))
rc, out = run([tcc, "-Wl,-Ttext=" + TEXT, hello_o, "-o", os.path.join(work, "hello_dyn")])
if rc == 0:
    d = open(os.path.join(work, "hello_dyn"), "rb").read()
    print("  note: without -static tcc writes PT_INTERP=%s and a .dynamic section -- %d B vs %d B static; "
          "the device has no dynamic linker, so the device contract is -static"
          % ("/lib64/ld-linux-x86-64.so.2" if b"ld-linux-x86-64" in d else "?", len(d), size(hello_sys)))

print("-- the same link against the build tree's objects, in tcc's own pull order")
libc_objs = objs_map(libc_list)
tcc1_objs = objs_map(tcc1_list)
all_objs = dict(libc_objs)
all_objs.update(tcc1_objs)
crt1, crti, crtn = (os.path.join(LIB, n) for n in ("crt1.o", "crti.o", "crtn.o"))
defined, undefined = set(), set()
for o in (crt1, crti, hello_o):
    d, u = symbols(o)
    defined |= d
    undefined |= (u - defined)
    undefined -= d
libc_a = os.path.join(LIB, "libc.a")
tcc1_a = os.path.join(TCCDIR, "libtcc1.a")
pulled_libc = pull_order(libc_a, all_objs, defined, undefined)
pulled_tcc1 = pull_order(tcc1_a, all_objs, defined, undefined)
print("  libc.a pulled %d members: %s" % (len(pulled_libc), " ".join(pulled_libc)))
print("  libtcc1.a pulled %d members: %s" % (len(pulled_tcc1), " ".join(pulled_tcc1) or "(none)"))
left = sorted(undefined - {"_start"})
strong_left = [s for s in left if s not in WEAK_UNDEF]
weak_left = [s for s in left if s in WEAK_UNDEF]
print("  weak references left unresolved (legal, read as 0): %s" % (" ".join(weak_left) or "(none)"))
check(not strong_left, "no strong symbol left undefined by the replica's scan (got %s)" % strong_left)
direct_objs = [all_objs[m] for m in pulled_libc + pulled_tcc1]
hello_direct = os.path.join(work, "hello_direct")
rc, out = run([tcc, "-nostdlib", "-static", "-Wl,-Ttext=" + TEXT, crt1, crti, hello_o]
              + direct_objs + [crtn, "-o", hello_direct])
check(rc == 0, "tcc -nostdlib crt1.o crti.o hello.o <%d build-tree objects> crtn.o %s"
      % (len(direct_objs), out))
if rc == 0 and os.path.exists(hello_sys):
    check(same(hello_sys, hello_direct),
          "hello_sys == hello_direct, BYTE-IDENTICAL (%d B); members are the build tree's objects"
          % size(hello_sys))
    va = [m for m in pulled_libc + pulled_tcc1 if m.startswith("va_list")]
    fl = [m for m in pulled_libc + pulled_tcc1 if m.startswith("libtcc1")]
    check(bool(va) and bool(fl), "the link reached libtcc1's va_list.o and libtcc1.o (va=%s float=%s)" % (va, fl))
    check(any(m.startswith("sqrt") for m in pulled_libc), "the link reached libm's sqrt member")

print("-- archive order: a variadic program that reaches NOTHING else in the libc")
# tcc scans libc.a to a fixpoint, then libtcc1.a to a fixpoint, and never
# libc.a again (tccelf.c tcc_add_runtime). va_list.o (libtcc1) calls memset()
# and abort(). hello.c happens to pull both through printf's dependencies
# before libtcc1.a is opened; this program does not, so it measures whether
# the order bites: if it links, libtcc1's objects need not be in libc.a; if
# it dies on 'abort' or 'memset', they must be (mksysroot --libtcc1-in-libc).
# The result is REPORTED, not asserted -- it is the measurement the flag's
# default is set from, and tests/sysroot.mk records which way it went.
min_c = os.path.join(work, "hello_min.c")
with open(min_c, "w", newline="\n") as f:
    f.write("#include <stdarg.h>\nstatic int sum(int n, ...){va_list ap;int s=0;va_start(ap,n);"
            "while(n-->0)s+=va_arg(ap,int);va_end(ap);return s;}\n"
            "int main(void){return sum(2,40,2)-42;}\n")
min_o = os.path.join(work, "hello_min.o")
rc, out = run([tcc, "-c", min_c, "-o", min_o])
check(rc == 0, "tcc -c hello_min.c (variadic, no printf) %s" % out)
rc, out = run([tcc, "-static", "-Wl,-Ttext=" + TEXT, min_o, "-o", os.path.join(work, "hello_min")])
libc_members = set(subprocess.run([ar, "t", libc_a], capture_output=True, text=True).stdout.split())
merged = "va_list.o" in libc_members
print("  libc.a %s libtcc1's objects; the minimal variadic program %s%s"
      % ("CONTAINS" if merged else "does NOT contain", "links" if rc == 0 else "FAILS",
         "" if rc == 0 else " -- " + out.splitlines()[0]))
check(rc == 0, "the minimal variadic program links against the sysroot as packed")

print("-- NEGATIVE CONTROL: drop one member from libc.a; the link must fail ON THAT SYMBOL")
overlay = os.path.join(work, "overlay")


def make_overlay(drop_member):
    if os.path.exists(overlay):
        shutil.rmtree(overlay)
    shutil.copytree(LIB, overlay)
    if drop_member:
        rc, out = run([ar, "d", os.path.join(overlay, "libc.a"), drop_member])
        if rc:
            sys.exit("ar d failed: " + out)
        # llvm-ar's 'd' rewrites the symbol index; confirm the member is gone.
        rc, out = run([ar, "t", os.path.join(overlay, "libc.a")])
        assert drop_member not in out.split(), drop_member


def overlay_link(out_name):
    return run([tcc, "-static", "-Wl,-Ttext=" + TEXT, "-L", overlay, "-B", os.path.join(overlay, "tcc"),
                hello_o, "-o", os.path.join(work, out_name)])


make_overlay(None)
rc, out = overlay_link("hello_overlay")
check(rc == 0 and same(hello_sys, os.path.join(work, "hello_overlay")),
      "positive twin: the overlay with the complete libc.a links byte-identical (proves -L/-B are honoured)")


def member_defining(sym):
    for m in pulled_libc:
        d, _ = symbols(all_objs[m])
        if sym in d:
            return m
    sys.exit("no pulled member defines " + sym)


for sym in ("memset", "sqrt", "printf"):
    mem = member_defining(sym)
    make_overlay(mem)
    rc, out = overlay_link("hello_negctl_" + sym)
    named = ("'%s'" % sym) in out or (" %s" % sym) in out
    check(rc != 0 and named,
          "drop %s (defines %s): link FAILS and names it: %s"
          % (mem, sym, out.splitlines()[0] if out else "(no message)"))

print("sysroot_link_test: %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
