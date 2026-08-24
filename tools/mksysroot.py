#!/usr/bin/env python3
"""Assemble the sysroot: what a C compiler on the device needs from the disk.

A Linux box keeps the C library's interface in /usr/include and its objects in
/usr/lib; a compiler is useless without both. LogitOS has the pieces -- the
mini-libc's headers in c/apps/libc/include, its objects under build/asobj, the
CLI startup in c/apps/crt0_cli.asm -- and no directory on the disk with any of
them. This tool lays them out the way tcc (third_party/tcc) looks for them,
so that `tcc hello.c -o hello` on the device needs no flags:

    usr/include/**            c/apps/libc/include/** with uonly/ flattened in
    usr/lib/libc.a            ar over $(LIBC_OBJS) + $(LIBM_OBJ)
    usr/lib/crt1.o            crt0_cli.o   (the name tcc asks for, see below)
    usr/lib/crti.o, crtn.o    empty objects (see below)
    usr/lib/tcc/include/*.h   tcc's own stddef/stdarg/stdbool/float/varargs
                              + stdint.h and limits.h GENERATED here (see below)
    usr/lib/tcc/libtcc1.a     ar over tcc's lib/ compiled FOR THE TARGET

This tool does not compile anything: make knows UCFLAGS and the dependency
graph, so the objects are built by tests/sysroot.mk and handed in as lists.
A sysroot built by the compiler it serves would be circular (tcc.aex does not
exist yet), so libtcc1's objects come from clang with the Makefile's UCFLAGS.

WHY THE NAMES ARE WHAT THEY ARE -- read from third_party/tcc, not assumed:

  crt1.o / crti.o / crtn.o.  libtcc.c:974-980: for an executable that is not
  -nostdlib, tcc_set_output_type() adds "crt1.o" and "crti.o" from
  CONFIG_TCC_CRTPREFIX (= CONFIG_SYSROOT "/usr/lib") BEFORE the user's files,
  and tccelf.c:1206 tcc_add_runtime() adds "crtn.o" after the libraries. A
  missing one is tcc_error_noabort("file '%s' not found") and the link fails.
  So crt0_cli.o is installed under the name tcc asks for, crt1.o, and NOT as
  a second copy called crt0.o -- two names for one object is two things to
  keep in step. crti.o/crtn.o are glibc's _init/_fini prologue and epilogue;
  this machine has neither (crt0_cli.asm does the whole job: argc/argv/envp,
  call main, SYS_EXIT), so both are EMPTY objects, assembled by nasm from
  tests/unit/sysroot_crtempty.asm rather than fabricated here, so that the
  tree's assembler vouches for their shape.
  The alternative contract -- "-nostdlib and name every file" -- was rejected
  because it moves the layout into every user's command line, and a layout
  nobody types by default is a layout nobody tests by default.

  libc.a is found by "-lc" (tccelf.c:1194 tcc_add_library_err(s1, "c")) in
  CONFIG_TCC_LIBPATHS, whose first entry is CONFIG_SYSROOT "/usr/lib".
  libtcc1.a is found by name in CONFIG_TCCDIR (tcc.h:273 TCC_LIBTCC1), which
  tcc.aex will compile in as /usr/lib/tcc.

  tcc reads GNU-format ar archives only through the "/" symbol index
  (tccelf.c:2636 tcc_load_archive -> :2593 tcc_load_alacarte): a BSD-format archive
  (__.SYMDEF) has no such member and every object in it is silently skipped.
  Hence --format=gnu, explicitly, and D for deterministic (no timestamps, no
  uids) so the archive is a function of its inputs.

  THE ARCHIVES ARE SCANNED ONCE EACH, IN ORDER: libc.a to a fixpoint, then
  libtcc1.a to a fixpoint, and never libc.a again. va_list.o in libtcc1 calls
  memset() and abort(); if the program has not already pulled those from
  libc.a, the link dies on them. MEASURED 2026-08-21 with --libtcc1-in-libc
  off: a program that does va_start/va_arg and nothing else fails with
  `undefined symbol 'memset'` (tests/unit/sysroot_link_test.py keeps the
  probe; sysroot_hello.c itself linked only because printf had already
  dragged string.o and stdlib.o in). So with --libtcc1-in-libc, which
  tests/sysroot.mk passes by default, every libtcc1 object is ALSO a member
  of libc.a: libc.a's own fixpoint scan resolves __va_start -> va_list.o ->
  memset/abort -> string.o/stdlib.o within one archive; libtcc1.a's scan
  afterwards finds nothing undefined and pulls nothing, so no symbol is
  defined twice. libtcc1.a still exists because tcc_add_support() opens it
  unconditionally and errors if it is absent.

  stdint.h and limits.h are FREESTANDING headers (C11 4p6) -- gcc and clang
  both ship them in the COMPILER's include dir, and a libc that has none is
  entitled to that. mini-libc has no stdint.h, and its limits.h is an
  #include_next onto the compiler's. tcc 0.9.27 ships neither, because on
  Linux it leans on glibc's. So this tool writes both into usr/lib/tcc/include
  with the LP64 x86-64 values clang used when it compiled the libc (int64_t is
  long, not long long -- the libc's objects were built against that and the
  PRI macros in inttypes.h say "ld"). tests/unit/sysroot_hdr_test.py diffs
  every macro's expansion against clang's own header. stdint.h is generated
  ONLY while the libc has none: the day c/apps/libc/include/stdint.h exists,
  this tool stops writing one, because {B}/include is searched first and a
  generated copy would shadow the real one forever.

The uonly/ flattening: uonly/sched.h exists because the KERNEL also has a
sched.h and the build's flat -I list cannot hold two (CLAUDE.md, Source
layout). On the disk there is no kernel header to collide with, so it
installs as usr/include/sched.h. A collision after flattening is refused by
name -- it would mean the libc grew a top-level sched.h and the uonly copy is
stale, and packing either one silently is wrong. sys/ is an ordinary
subdirectory: the Makefile's exclusion of %/include/sys was about the HOST
build's include search, not about the files.

usage: mksysroot.py --out DIR --libc-include DIR --tcc-include DIR --ar AR
                    --libc-objs LISTFILE --libtcc1-objs LISTFILE
                    --crt1 OBJ --crtempty OBJ [--manifest FILE]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys


def die(msg):
    sys.exit("mksysroot: " + msg)


# ---------------------------------------------------------------------------
# The two generated compiler headers. Values are clang's for --target=x86_64-elf
# (clang -dM -E, recorded 2026-08-21: __INT64_TYPE__ long int, __INTPTR_TYPE__
# long int, __SIZE_TYPE__ long unsigned int, __WCHAR_TYPE__ int). tcc's own
# stddef.h typedefs int8_t..uint64_t behind `__int8_t_defined`; the same guard
# here makes the two agree whichever is included first, and tcc 0.9.27 accepts
# a compatible typedef redefinition in any case (tccgen.c decl0).
STDINT_H = """\
/* stdint.h -- GENERATED by tools/mksysroot.py; do not edit in place.
 * Freestanding header (C11 4p6): the compiler's, not the libc's, which is why
 * it lives beside tcc's stddef.h. Values are those of clang for x86_64-elf,
 * the compiler the libc's objects were built with. LP64: int64_t is long. */
#ifndef _STDINT_H
#define _STDINT_H

#ifndef __int8_t_defined
#define __int8_t_defined
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
#endif

typedef signed char        int_least8_t;
typedef short              int_least16_t;
typedef int                int_least32_t;
typedef long               int_least64_t;
typedef unsigned char      uint_least8_t;
typedef unsigned short     uint_least16_t;
typedef unsigned int       uint_least32_t;
typedef unsigned long      uint_least64_t;

typedef signed char        int_fast8_t;
typedef short              int_fast16_t;
typedef int                int_fast32_t;
typedef long               int_fast64_t;
typedef unsigned char      uint_fast8_t;
typedef unsigned short     uint_fast16_t;
typedef unsigned int       uint_fast32_t;
typedef unsigned long      uint_fast64_t;

#ifndef _STDDEF_H
typedef long               intptr_t;
typedef unsigned long      uintptr_t;
#endif
typedef long               intmax_t;
typedef unsigned long      uintmax_t;

#define INT8_MIN           (-128)
#define INT16_MIN          (-32767-1)
#define INT32_MIN          (-2147483647-1)
#define INT64_MIN          (-9223372036854775807L-1)
#define INT8_MAX           127
#define INT16_MAX          32767
#define INT32_MAX          2147483647
#define INT64_MAX          9223372036854775807L
#define UINT8_MAX          255
#define UINT16_MAX         65535
#define UINT32_MAX         4294967295U
#define UINT64_MAX         18446744073709551615UL

#define INT_LEAST8_MIN     INT8_MIN
#define INT_LEAST16_MIN    INT16_MIN
#define INT_LEAST32_MIN    INT32_MIN
#define INT_LEAST64_MIN    INT64_MIN
#define INT_LEAST8_MAX     INT8_MAX
#define INT_LEAST16_MAX    INT16_MAX
#define INT_LEAST32_MAX    INT32_MAX
#define INT_LEAST64_MAX    INT64_MAX
#define UINT_LEAST8_MAX    UINT8_MAX
#define UINT_LEAST16_MAX   UINT16_MAX
#define UINT_LEAST32_MAX   UINT32_MAX
#define UINT_LEAST64_MAX   UINT64_MAX

#define INT_FAST8_MIN      INT8_MIN
#define INT_FAST16_MIN     INT16_MIN
#define INT_FAST32_MIN     INT32_MIN
#define INT_FAST64_MIN     INT64_MIN
#define INT_FAST8_MAX      INT8_MAX
#define INT_FAST16_MAX     INT16_MAX
#define INT_FAST32_MAX     INT32_MAX
#define INT_FAST64_MAX     INT64_MAX
#define UINT_FAST8_MAX     UINT8_MAX
#define UINT_FAST16_MAX    UINT16_MAX
#define UINT_FAST32_MAX    UINT32_MAX
#define UINT_FAST64_MAX    UINT64_MAX

#define INTPTR_MIN         INT64_MIN
#define INTPTR_MAX         INT64_MAX
#define UINTPTR_MAX        UINT64_MAX
#define INTMAX_MIN         INT64_MIN
#define INTMAX_MAX         INT64_MAX
#define UINTMAX_MAX        UINT64_MAX
#define PTRDIFF_MIN        INT64_MIN
#define PTRDIFF_MAX        INT64_MAX
#define SIZE_MAX           UINT64_MAX
#define SIG_ATOMIC_MIN     INT32_MIN
#define SIG_ATOMIC_MAX     INT32_MAX
#define WCHAR_MIN          INT32_MIN
#define WCHAR_MAX          INT32_MAX
#define WINT_MIN           INT32_MIN
#define WINT_MAX           INT32_MAX

#define INT8_C(c)          c
#define INT16_C(c)         c
#define INT32_C(c)         c
#define INT64_C(c)         c ## L
#define UINT8_C(c)         c
#define UINT16_C(c)        c
#define UINT32_C(c)        c ## U
#define UINT64_C(c)        c ## UL
#define INTMAX_C(c)        c ## L
#define UINTMAX_C(c)       c ## UL

#endif /* _STDINT_H */
"""

# limits.h: the C limits, then the libc's POSIX additions via include_next --
# guarded by the libc header's OWN guard macro, which it defines before its
# #include_next. Under the order tcc.aex is built with (tests/tcc.mk:
# /usr/include first, then {B}/include -- the order this libc was written
# against) the libc's limits.h has already been entered when this one is
# reached, _LIBC_LIMITS_H is set, and this header adds nothing on top. Under
# UPSTREAM tcc's default order (compiler dir first, as gcc and clang also
# do) this header is entered first, the guard is clear, and the include_next
# reaches the libc's header -- whose own unconditional include_next then
# finds no further limits.h and fails BY NAME at c/apps/libc/include/
# limits.h:8. That is the honest outcome: the compiler's half is complete
# either way, and the half that is wrong says where. (The libc's fix, if it
# wants to be order-independent the way glibc's is: guard its include_next on
# the compiler header's macro, here _TCC_LIMITS_H.)
# MB_LEN_MAX is 1 because clang's is 1 and the libc's header only sets its
# own 4 when the compiler left it undefined -- which, with the include_next
# at its line 8 running first, clang never does. Mirrored, not corrected.
LIMITS_H = """\
/* limits.h -- GENERATED by tools/mksysroot.py; do not edit in place.
 * The C limits for LP64 x86-64, clang's values. The POSIX limits (PATH_MAX,
 * NAME_MAX, OPEN_MAX, ...) are the libc's and come from its own limits.h. */
#ifndef _TCC_LIMITS_H
#define _TCC_LIMITS_H

#define CHAR_BIT      8
#define SCHAR_MIN     (-128)
#define SCHAR_MAX     127
#define UCHAR_MAX     255
#define CHAR_MIN      SCHAR_MIN
#define CHAR_MAX      SCHAR_MAX
#define SHRT_MIN      (-32767-1)
#define SHRT_MAX      32767
#define USHRT_MAX     65535
#define INT_MIN       (-2147483647-1)
#define INT_MAX       2147483647
#define UINT_MAX      4294967295U
#define LONG_MIN      (-9223372036854775807L-1L)
#define LONG_MAX      9223372036854775807L
#define ULONG_MAX     18446744073709551615UL
#define LLONG_MIN     (-9223372036854775807LL-1LL)
#define LLONG_MAX     9223372036854775807LL
#define ULLONG_MAX    18446744073709551615ULL
#define MB_LEN_MAX    1

#ifndef _LIBC_LIMITS_H
#include_next <limits.h>
#endif

#endif /* _TCC_LIMITS_H */
"""

# The clang-isms a header may carry and tcc 0.9.27 may not take. This is the
# STATIC half of the audit (where they are); tests/unit/sysroot_hdr_test.py is
# the dynamic half (what tcc does at the use site). Listed in the manifest so
# a reader can see the inventory without grepping.
CLANGISMS = re.compile(
    r"__int128|__builtin_\w+|__attribute__|\btypeof\b|__typeof\b|_Generic|"
    r"_Static_assert|#\s*include_next|__asm__|__has_include|_Noreturn|_Alignof|_Alignas")


def walk_regular(root):
    """Sorted (rel, abs) for every regular file under root; symlinks and
    anything that is not a file or directory are refused by name -- the same
    rule tools/mkfs.py add_tree() applies, for the same reason."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for d in list(dirnames):
            p = os.path.join(dirpath, d)
            if os.path.islink(p):
                die("refusing to follow a symlinked directory: %s" % p)
        for f in sorted(filenames):
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                die("refusing to pack a symlink: %s -> %s" % (p, os.readlink(p)))
            if not os.path.isfile(p):
                die("not a regular file: %s" % p)
            out.append((os.path.relpath(p, root).replace(os.sep, "/"), p))
    return out


def copy_bytes(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        f.write(data)
    return len(data)


def read_list(path):
    with open(path) as f:
        items = [l.strip() for l in f if l.strip()]
    missing = [i for i in items if not os.path.isfile(i)]
    if missing:
        die("%d object(s) in %s do not exist, e.g. %s" % (len(missing), path, missing[:3]))
    return items


def run_ar(ar, archive, objs):
    """GNU-format, deterministic, members in the order given (sorted by the
    caller). Members are passed by path and ar stores the basename; two objects
    with one basename would be two members with one name, which the index
    still distinguishes by offset but a human cannot -- refused."""
    names = [os.path.basename(o) for o in objs]
    dups = sorted({n for n in names if names.count(n) > 1})
    if dups:
        die("duplicate member basenames in %s: %s" % (archive, dups))
    if os.path.exists(archive):
        os.remove(archive)
    cmd = [ar, "rcsD", "--format=gnu", archive] + objs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die("%s failed:\n%s%s" % (" ".join(cmd[:4]), r.stdout, r.stderr))
    return os.path.getsize(archive)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--libc-include", required=True)
    ap.add_argument("--tcc-include", required=True)
    ap.add_argument("--ar", required=True)
    ap.add_argument("--libc-objs", required=True, help="file listing $(LIBC_OBJS) $(LIBM_OBJ)")
    ap.add_argument("--libtcc1-objs", required=True, help="file listing libtcc1's target objects")
    ap.add_argument("--crt1", required=True, help="crt0_cli.o, installed as usr/lib/crt1.o")
    ap.add_argument("--crtempty", required=True, help="an empty object, installed as crti.o and crtn.o")
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--libtcc1-in-libc", action="store_true",
                    help="also put libtcc1's objects into libc.a (see the archive-order note)")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    # The tree is REBUILT from nothing on every run: a stale file from an
    # earlier layout would be packed onto the image as if it were current.
    # Guard the rm -rf by name so a mistyped --out cannot remove anything else.
    if os.path.basename(out) != "sysroot":
        die("--out must be a directory named 'sysroot' (it is removed and rebuilt): %s" % out)
    if os.path.lexists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    manifest = []
    totals = {}

    def note(section, rel, nbytes):
        totals.setdefault(section, [0, 0])
        totals[section][0] += 1
        totals[section][1] += nbytes
        manifest.append("%-10s %9d  %s" % (section, nbytes, rel))

    # --- usr/include -------------------------------------------------------
    plan = {}
    for rel, src in walk_regular(a.libc_include):
        if rel.startswith("uonly/"):
            dest = rel[len("uonly/"):]
            why = "uonly/ flattened"
        else:
            dest = rel
            why = ""
        if dest in plan:
            die("collision in usr/include/%s: %s and %s" % (dest, plan[dest][0], src))
        plan[dest] = (src, why)
    clangisms = []
    for dest in sorted(plan):
        src, why = plan[dest]
        n = copy_bytes(src, os.path.join(out, "usr/include", dest))
        note("include", "usr/include/%s%s" % (dest, ("   <- " + why) if why else ""), n)
        with open(src, encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                for m in CLANGISMS.finditer(line):
                    clangisms.append((dest, lineno, m.group(0)))
    libc_has_stdint = "stdint.h" in plan
    libc_has_limits = "limits.h" in plan

    # --- usr/lib/tcc/include -----------------------------------------------
    tcc_inc = os.path.join(out, "usr/lib/tcc/include")
    os.makedirs(tcc_inc)
    for rel, src in walk_regular(a.tcc_include):
        if not rel.endswith(".h") or "/" in rel:
            continue
        n = copy_bytes(src, os.path.join(tcc_inc, rel))
        note("tccinc", "usr/lib/tcc/include/" + rel, n)
    generated = []
    if not libc_has_stdint:
        with open(os.path.join(tcc_inc, "stdint.h"), "w", newline="\n") as f:
            f.write(STDINT_H)
        note("tccinc", "usr/lib/tcc/include/stdint.h   <- GENERATED (libc has none)", len(STDINT_H))
        generated.append("stdint.h")
    if libc_has_limits:
        with open(os.path.join(tcc_inc, "limits.h"), "w", newline="\n") as f:
            f.write(LIMITS_H)
        note("tccinc", "usr/lib/tcc/include/limits.h   <- GENERATED (libc's is an #include_next onto it)",
             len(LIMITS_H))
        generated.append("limits.h")
    else:
        die("c/apps/libc/include/limits.h is gone; the generated compiler limits.h "
            "was written against its #include_next and needs re-deciding")

    # --- usr/lib: libc.a, crt1.o, crti.o, crtn.o ------------------------------
    lib = os.path.join(out, "usr/lib")
    os.makedirs(lib, exist_ok=True)
    libc_objs = sorted(read_list(a.libc_objs))
    tcc1_objs = sorted(read_list(a.libtcc1_objs))
    extra = tcc1_objs if a.libtcc1_in_libc else []
    n = run_ar(a.ar, os.path.join(lib, "libc.a"), libc_objs + extra)
    note("lib", "usr/lib/libc.a   (%d libc/libm + %d libtcc1 members)" % (len(libc_objs), len(extra)), n)
    n = copy_bytes(a.crt1, os.path.join(lib, "crt1.o"))
    note("lib", "usr/lib/crt1.o   <- %s" % a.crt1, n)
    for nm in ("crti.o", "crtn.o"):
        n = copy_bytes(a.crtempty, os.path.join(lib, nm))
        note("lib", "usr/lib/%s   <- %s (empty)" % (nm, a.crtempty), n)

    # --- usr/lib/tcc/libtcc1.a ---------------------------------------------
    n = run_ar(a.ar, os.path.join(out, "usr/lib/tcc/libtcc1.a"), tcc1_objs)
    note("lib", "usr/lib/tcc/libtcc1.a   (%d members)" % len(tcc1_objs), n)

    # --- report -------------------------------------------------------------
    lines = []
    lines.append("mksysroot: %s" % out)
    nfiles = sum(v[0] for v in totals.values())
    nbytes = sum(v[1] for v in totals.values())
    for k in ("include", "tccinc", "lib"):
        c, b = totals.get(k, (0, 0))
        lines.append("  %-8s %4d files %10d bytes" % (k, c, b))
    lines.append("  total    %4d files %10d bytes" % (nfiles, nbytes))
    lines.append("  generated compiler headers: %s" % (", ".join(generated) or "none"))
    lines.append("  clang-isms in the libc headers (static scan; tcc's verdict is "
                 "tests/unit/sysroot_hdr_test.py's): %d sites" % len(clangisms))
    for dest, lineno, tok in clangisms:
        lines.append("    %s:%d  %s" % (dest, lineno, tok))
    text = "\n".join(lines)
    print(text)
    if a.manifest:
        os.makedirs(os.path.dirname(os.path.abspath(a.manifest)) or ".", exist_ok=True)
        with open(a.manifest, "w", newline="\n") as f:
            f.write(text + "\n\n" + "\n".join(manifest) + "\n")


if __name__ == "__main__":
    main()
