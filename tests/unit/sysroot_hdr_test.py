#!/usr/bin/env python3
"""Part C of the sysroot: can tcc 0.9.27 actually use the headers it is given?

The libc's headers were written to be compiled by clang with the Makefile's
flags. tcc is a different compiler with a different include-search ORDER and a
shorter list of builtins. Two things are measured, with the host-built tcc
(the same source that becomes tcc.aex):

 1. EVERY HEADER PARSES. For each file under usr/include and usr/lib/tcc/include
    a one-line program includes it and tcc compiles it to an object, under
    BOTH search orders:
      default   tcc's compiled-in order: {B}/include, /usr/local/include,
                /usr/include  (the compiler's dir FIRST -- gcc's and clang's
                order too)
      libcfirst -nostdinc -I usr/include -I usr/lib/tcc/include  (the order
                the libc was written against: the Makefile's -I list puts the
                libc before clang's resource dir)
    A header that fails under one order and not the other is reported as
    exactly that.

 2. USE-SITE PROBES. A header that merely DEFINES `isnan(x)` as
    `__builtin_isnan(x)` parses fine; the program that calls isnan() is what
    breaks, at its first use, in somebody else's file. Each clang-ism the
    static scan found (tools/mksysroot.py) has a probe here that USES it, and
    the verdict is recorded against an EXPECTATION TABLE: a probe that starts
    passing (the libc fixed it) fails this gate just as one that starts
    failing does, so the table is a change detector and not a wish list.

 3. THE GENERATED HEADERS AGREE WITH CLANG. For every standard macro in
    stdint.h and limits.h, tcc's expansion of mksysroot.py's generated header
    is compared with clang's expansion of its own header for x86_64-elf,
    whitespace-normalised. The typedefs are checked by size and signedness
    with compile-time array tricks, because tcc 0.9.27 has no _Static_assert.

usage: sysroot_hdr_test.py <tcc> <sysroot> <clang> <workdir>
"""
import os
import re
import subprocess
import sys

tcc, sysroot, clang, work = sys.argv[1:5]
sysroot = os.path.abspath(sysroot)
os.makedirs(work, exist_ok=True)
INC = os.path.join(sysroot, "usr/include")
TINC = os.path.join(sysroot, "usr/lib/tcc/include")
# Three search orders:
#   device   no flags. The host tcc is built with CONFIG_TCC_SYSINCLUDEPATHS =
#            <sysroot>/usr/include:{B}/include (tests/unit/sysroot_tccsnap.sh)
#            -- the order tests/tcc.mk compiles into tcc.aex, libc first.
#   literal  -nostdinc -I usr/include -I usr/lib/tcc/include: the task's
#            literal form; the same order spelled on the command line.
#   stock    -nostdinc -I usr/lib/tcc/include -I usr/include: UPSTREAM tcc's
#            default ({B}/include first), also gcc's and clang's convention.
ORDERS = {
    "device":  [],
    "literal": ["-nostdinc", "-I" + INC, "-I" + TINC],
    "stock":   ["-nostdinc", "-I" + TINC, "-I" + INC],
}
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails += 1


def first_error(text):
    lines = [l for l in text.strip().splitlines() if l.strip()]
    errs = [l for l in lines if "error:" in l]
    return errs[0] if errs else (lines[-1] if lines else "")


def compile_with(order, src, name, link=False):
    """tcc -c; with link=True also tcc -static into an executable against the
    sysroot, because tcc 0.9.27 treats an unknown __builtin_x(...) as an
    IMPLICIT FUNCTION DECLARATION -- a warning -- and the failure only shows
    as `undefined symbol '__builtin_x'` at link time. A -c probe would call
    that a pass. Returns (ok, message) where message is the first error line,
    or the implicit-declaration warning when that is what compiling said."""
    path = os.path.join(work, name + ".c")
    obj = os.path.join(work, name + ".o")
    with open(path, "w", newline="\n") as f:
        f.write(src)
    r = subprocess.run([tcc] + ORDERS[order] + ["-c", path, "-o", obj], capture_output=True, text=True)
    out = r.stderr + r.stdout
    if r.returncode != 0:
        return False, first_error(out)
    if not link:
        return True, ""
    warn = [l for l in out.splitlines() if "implicit declaration" in l]
    r = subprocess.run([tcc, "-static", "-Wl,-Ttext=0x50000000", obj, "-o", os.path.join(work, name + ".exe")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        msg = first_error(r.stderr + r.stdout)
        if warn:
            msg += "  (after: %s)" % warn[0].split(":", 2)[-1].strip()
        return False, msg
    return True, ""


# ---- 1. every header parses ----------------------------------------------
headers = []
for base, prefix in ((INC, ""), (TINC, "")):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn.endswith(".h"):
                headers.append(os.path.relpath(os.path.join(dirpath, fn), base).replace(os.sep, "/"))
headers = sorted(set(headers))
# tcc's own varargs.h is upstream's deliberate `#error "TCC no longer
# implements <varargs.h>"` -- a header that exists in order to fail with a
# sentence instead of "file not found". It is installed because that is the
# better message, and excluded here because "fails to parse" is its contract.
headers = [h for h in headers if h != "varargs.h"]
print("-- 1. %d headers x %d search orders (varargs.h excluded: it is an #error by design)"
      % (len(headers), len(ORDERS)))
results = {}
for h in headers:
    for order in ORDERS:
        ok, msg = compile_with(order, "#include <%s>\nint sysroot_probe_dummy;\n" % h,
                               "hdr_%s_%s" % (order, h.replace("/", "_").replace(".", "_")))
        results[(h, order)] = (ok, msg)
for order in ORDERS:
    bad = [h for h in headers if not results[(h, order)][0]]
    print("  %-8s %d / %d parse%s" % (order, len(headers) - len(bad), len(headers),
                                      "" if not bad else "; failing: " + " ".join(bad)))
    for h in bad:
        print("           %-16s %s" % (h, results[(h, order)][1]))
dev_bad = [h for h in headers if not results[(h, "device")][0]]
lit_bad = [h for h in headers if not results[(h, "literal")][0]]
stock_bad = [h for h in headers if not results[(h, "stock")][0]]
# The gate: every header parses under the device order and under the literal
# form (they are the same order, so they must agree); upstream's order is
# allowed to lose exactly limits.h -- the libc's #include_next has nothing
# after it when the compiler's dir comes first (tools/mksysroot.py LIMITS_H)
# -- and nothing else.
check(not dev_bad, "every header parses under the device order (libc first)")
check(dev_bad == lit_bad, "the literal -nostdinc -I form agrees with the device order")
check(stock_bad == ["limits.h"],
      "under upstream's order (compiler dir first) the only casualty is limits.h (got %s)" % stock_bad)
check("limits.h" in stock_bad and "include" in results[("limits.h", "stock")][1],
      "and it fails at the libc's #include_next, by name: %s" % results[("limits.h", "stock")][1])

# ---- 2. use-site probes ----------------------------------------------------
# (name, source, expected-to-compile?, why)
PROBES = [
    ("math_NAN", "#include <math.h>\ndouble f(void){return NAN;}\n", False,
     "NAN is __builtin_nanf(\"\"); tcc has no such builtin -> implicit decl, then undefined at link"),
    ("math_INFINITY", "#include <math.h>\ndouble f(void){return INFINITY;}\n", False,
     "__builtin_inff"),
    ("math_HUGE_VAL", "#include <math.h>\ndouble f(void){return HUGE_VAL;}\n", False,
     "__builtin_inf"),
    ("math_isnan", "#include <math.h>\nint f(double x){return isnan(x);}\n", False, "__builtin_isnan"),
    ("math_isinf", "#include <math.h>\nint f(double x){return isinf(x);}\n", False, "__builtin_isinf"),
    ("math_isfinite", "#include <math.h>\nint f(double x){return isfinite(x);}\n", False, "__builtin_isfinite"),
    ("math_isnormal", "#include <math.h>\nint f(double x){return isnormal(x);}\n", False, "__builtin_isnormal"),
    ("math_signbit", "#include <math.h>\nint f(double x){return signbit(x);}\n", False, "__builtin_signbit"),
    ("math_fpclassify", "#include <math.h>\nint f(double x){return fpclassify(x);}\n", False,
     "__builtin_fpclassify"),
    ("math_isgreater", "#include <math.h>\nint f(double x,double y){return isgreater(x,y);}\n", False,
     "__builtin_isgreater"),
    ("math_isless", "#include <math.h>\nint f(double x,double y){return isless(x,y);}\n", False,
     "__builtin_isless"),
    ("math_isunordered", "#include <math.h>\nint f(double x,double y){return isunordered(x,y);}\n", False,
     "__builtin_isunordered"),
    ("math_sqrt_call", "#include <math.h>\ndouble f(double x){return sqrt(x);}\n", True,
     "an ordinary prototype; the libm half of libc.a provides it"),
    ("stdlib_alloca", "#include <stdlib.h>\nvoid *f(int n){return alloca(n);}\n", False,
     "alloca is #defined to __builtin_alloca; tcc knows `alloca` (TOK_alloca -> libtcc1) but not the "
     "__builtin_ spelling"),
    ("stddef_alloca", "#include <stddef.h>\nvoid *f(int n){return alloca(n);}\n", True,
     "tcc's own stddef.h declares alloca(); without stdlib.h's macro it works"),
    ("assert_static_assert", "#include <assert.h>\nstatic_assert(1, \"x\");\nint y;\n", False,
     "static_assert -> _Static_assert, which tcc 0.9.27 does not have (no TOK for it in tcctok.h)"),
    ("assert_assert", "#include <assert.h>\nint f(int x){assert(x);return x;}\n", True, "plain assert()"),
    ("features_weak_alias", "#include <features.h>\nint a(void){return 0;}\nweak_alias(a, b);\n", True,
     "__typeof + __attribute__((__weak__, __alias__)) are all in tcctok.h"),
    ("features_hidden", "#include <features.h>\nhidden int a(void){return 0;}\n", True,
     "visibility attribute is TOK_VISIBILITY"),
    ("pthread_noreturn", "#include <pthread.h>\nvoid f(void){pthread_exit(0);}\n", True,
     "__attribute__((noreturn)) on a prototype"),
    ("types_ssize_t_twice", "#include <stddef.h>\n#include <sys/types.h>\nssize_t f(void){return -1;}\n", True,
     "tcc's stddef.h typedefs ssize_t too; tcc accepts a compatible typedef redefinition"),
    ("limits_INT_MAX", "#include <limits.h>\nint f(void){return INT_MAX;}\n", True, "generated + libc"),
    ("limits_PATH_MAX", "#include <limits.h>\nint f(void){return PATH_MAX;}\n", True,
     "the libc's POSIX half is reached"),
    ("stdint_types", "#include <stdint.h>\n"
     "typedef char c1[sizeof(int64_t)==8?1:-1];\ntypedef char c2[sizeof(intptr_t)==8?1:-1];\n"
     "typedef char c3[sizeof(int32_t)==4?1:-1];\ntypedef char c4[sizeof(uint8_t)==1?1:-1];\n"
     "typedef char c5[(int64_t)-1<0?1:-1];\ntypedef char c6[(uint64_t)-1>0?1:-1];\n"
     "typedef char c7[sizeof(intmax_t)==8?1:-1];\nint64_t x = INT64_MAX;\n", True,
     "generated stdint.h: widths and signedness"),
    ("inttypes_PRId64", "#include <inttypes.h>\n#include <stdio.h>\nvoid f(int64_t v){printf(\"%\" PRId64, v);}\n",
     True, "inttypes.h includes <stdint.h>, which the libc does not have and the sysroot generates"),
    ("stdio_printf", "#include <stdio.h>\nint f(void){return printf(\"%d\\n\", 1);}\n", True, ""),
    ("stdarg_va", "#include <stdarg.h>\nint f(int n,...){va_list ap;va_start(ap,n);n=va_arg(ap,int);"
     "va_end(ap);return n;}\n", True, "tcc's stdarg.h"),
]
print("-- 2. %d use-site probes (device order), each compiled AND linked with tcc" % len(PROBES))
resource_inc = subprocess.run([clang, "-print-resource-dir"], capture_output=True,
                              text=True).stdout.strip() + "/include"
for name, src, expect_ok, why in PROBES:
    src = src + "int main(void){return 0;}\n"
    ok, msg = compile_with("device", src, "probe_" + name, link=True)
    verdict = "compiles" if ok else "FAILS"
    check(ok == expect_ok, "%-22s %-9s (expected %s) %s%s"
          % (name, verdict, "compiles" if expect_ok else "FAILS", ("-- " + msg) if not ok else "",
             ("   [" + why + "]") if why and ok != expect_ok else ""))
    if not expect_ok:
        # CONTROL: the same probe under clang with the same libc headers must
        # compile -- the probe is valid C against this libc, and what fails
        # is tcc, not the probe.
        path = os.path.join(work, "probe_" + name + ".c")
        r = subprocess.run([clang, "--target=x86_64-elf", "-ffreestanding", "-nostdinc",
                            "-I" + INC, "-isystem", resource_inc, "-c", path,
                            "-o", os.path.join(work, "probe_" + name + ".clang.o")],
                           capture_output=True, text=True)
        check(r.returncode == 0, "    control: the same probe compiles under clang%s"
              % ("" if r.returncode == 0 else " -- " + (r.stderr.strip().splitlines() or ["?"])[0]))

# ---- 3. generated headers agree with clang ---------------------------------
STDINT_NAMES = """INT8_MIN INT16_MIN INT32_MIN INT64_MIN INT8_MAX INT16_MAX INT32_MAX INT64_MAX
UINT8_MAX UINT16_MAX UINT32_MAX UINT64_MAX
INT_LEAST8_MIN INT_LEAST16_MIN INT_LEAST32_MIN INT_LEAST64_MIN
INT_LEAST8_MAX INT_LEAST16_MAX INT_LEAST32_MAX INT_LEAST64_MAX
UINT_LEAST8_MAX UINT_LEAST16_MAX UINT_LEAST32_MAX UINT_LEAST64_MAX
INT_FAST8_MIN INT_FAST16_MIN INT_FAST32_MIN INT_FAST64_MIN
INT_FAST8_MAX INT_FAST16_MAX INT_FAST32_MAX INT_FAST64_MAX
UINT_FAST8_MAX UINT_FAST16_MAX UINT_FAST32_MAX UINT_FAST64_MAX
INTPTR_MIN INTPTR_MAX UINTPTR_MAX INTMAX_MIN INTMAX_MAX UINTMAX_MAX
PTRDIFF_MIN PTRDIFF_MAX SIZE_MAX SIG_ATOMIC_MIN SIG_ATOMIC_MAX
WCHAR_MIN WCHAR_MAX WINT_MIN WINT_MAX
INT8_C(1) INT16_C(1) INT32_C(1) INT64_C(1) UINT8_C(1) UINT16_C(1) UINT32_C(1) UINT64_C(1)
INTMAX_C(1) UINTMAX_C(1)""".split()
LIMITS_NAMES = """CHAR_BIT SCHAR_MIN SCHAR_MAX UCHAR_MAX CHAR_MIN CHAR_MAX SHRT_MIN SHRT_MAX USHRT_MAX
INT_MIN INT_MAX UINT_MAX LONG_MIN LONG_MAX ULONG_MAX LLONG_MIN LLONG_MAX ULLONG_MAX MB_LEN_MAX""".split()


def expansions(cmd, header, names, tag):
    src = "#include <%s>\n" % header + "".join("@%s@ %s\n" % (n, n) for n in names)
    path = os.path.join(work, "exp_%s_%s.c" % (tag, header.replace(".", "_")))
    with open(path, "w", newline="\n") as f:
        f.write(src)
    r = subprocess.run(cmd + [path], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr + r.stdout).strip().splitlines()[:2]
    out = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*@(\S+)@\s*(.*)$", line)
        if m:
            out[m.group(1)] = re.sub(r"\s+", "", m.group(2))
    return out, None


print("-- 3. generated stdint.h / limits.h vs clang's own (x86_64-elf)")
for header, names in (("stdint.h", STDINT_NAMES), ("limits.h", LIMITS_NAMES)):
    # Device order: the libc dir first, so <limits.h> is the libc's, whose
    # #include_next reaches the generated one (stdint.h exists only on the
    # tcc side, so the order does not matter for it).
    ours, err = expansions([tcc, "-nostdinc", "-I" + INC, "-I" + TINC, "-E", "-P"], header, names, "tcc")
    # clang: its resource-dir header only (no libc), freestanding.
    theirs, err2 = expansions([clang, "--target=x86_64-elf", "-ffreestanding", "-nostdinc",
                               "-isystem", subprocess.run([clang, "-print-resource-dir"], capture_output=True,
                                                          text=True).stdout.strip() + "/include",
                               "-E", "-P"], header, names, "clang")
    if ours is None or theirs is None:
        check(False, "%s: preprocessing failed: tcc=%s clang=%s" % (header, err, err2))
        continue
    diffs = [(n, ours.get(n), theirs.get(n)) for n in names if ours.get(n) != theirs.get(n)]
    check(not diffs, "%s: %d/%d macro expansions identical to clang's%s"
          % (header, len(names) - len(diffs), len(names),
             "" if not diffs else " -- differ: " + ", ".join("%s tcc=%s clang=%s" % d for d in diffs[:6])))

print("sysroot_hdr_test: %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
