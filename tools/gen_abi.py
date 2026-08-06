#!/usr/bin/env python3
"""Generate AetherScript's view of the kernel ABI from include/abi/logit_abi.h.

AetherScript reaches the kernel through syscall(), and the syscall NUMBERS are
already an identity rather than a copy: as_native.c includes logit_abi.h and
does as_define_int("SYS_WRITE", SYS_WRITE), so they cannot drift. The struct
LAYOUTS were the half that was still a copy -- `peek32(a + 12)` for the hour
field is a number someone counted out of a header comment, and a struct the
kernel reorders turns that into a script quietly reading the wrong field.

Two outputs, and the second one is the whole point:

  fsroot/as/lib/abi.as        what AetherScript sees (layout objects)
  c/apps/as/abi_layout.inc    _Static_asserts, included by as_native.c

The offsets here are computed with LP64 rules -- but nothing is asked to trust
that. Every number is emitted twice: once for the script, and once as an
assertion that the compiler building /bin/as has to agree with. So the numbers
the script uses are not this tool's opinion of the layout, they are the target
compiler's, and a disagreement is a build failure rather than a wild read.

That leaves exactly one thing the asserts cannot catch -- a field RENAMED at the
same offset -- which is what `--check` is for: it regenerates and diffs, so the
names have to match too.

  python3 tools/gen_abi.py --write    regenerate both files
  python3 tools/gen_abi.py --check    verify they are current (read-only)
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "abi", "logit_abi.h")
OUT_AS = os.path.join(ROOT, "fsroot", "as", "lib", "abi.as")
OUT_INC = os.path.join(ROOT, "c", "apps", "as", "abi_layout.inc")

# LP64 (x86_64): (size, alignment, AetherScript slot kind).
#   i = signed, sign-extended on read      u = unsigned, raw bits
#   p = pointer, raw bits                  s = fixed byte span, reads as a string
SCALARS = {
    "char":               (1, 1, "i"),
    "signed char":        (1, 1, "i"),
    "unsigned char":      (1, 1, "u"),
    "short":              (2, 2, "i"),
    "unsigned short":     (2, 2, "u"),
    "int":                (4, 4, "i"),
    "unsigned":           (4, 4, "u"),
    "unsigned int":       (4, 4, "u"),
    "long":               (8, 8, "i"),
    "unsigned long":      (8, 8, "u"),
    "long long":          (8, 8, "i"),
    "unsigned long long": (8, 8, "u"),
}
PTR = (8, 8, "p")


class Unsupported(Exception):
    """A member this tool will not guess at. Refusing beats emitting a number
    nobody checked -- the whole point is that the offsets are not guesses."""


def norm_type(t):
    t = re.sub(r"\b(const|volatile|struct|register)\b", " ", t)
    return " ".join(t.split())


# The only type words this tool recognises. A member built from anything else --
# a typedef, a nested struct -- has no base type here and is refused rather than
# guessed at.
TYPE_WORDS = ("char", "signed", "unsigned", "short", "int", "long")


def parse_members(struct_name, body):
    """-> [(field, size, align, kind)] in declaration order."""
    out = []
    for decl in body.split(";"):
        decl = norm_type(decl)          # also drops const/volatile/struct
        if not decl:
            continue
        # A member is a run of type words followed by declarators:
        #   "int x, y, px, mono"      -> base "int",           rest "x, y, px, mono"
        #   "unsigned char *rgba"     -> base "unsigned char", rest "*rgba"
        #   "unsigned char mac[6]"    -> base "unsigned char", rest "mac[6]"
        toks = decl.split()
        i = 0
        while i < len(toks) and toks[i] in TYPE_WORDS:
            i += 1
        if i == 0 or i == len(toks):
            raise Unsupported("%s: cannot parse member %r" % (struct_name, decl))
        base, rest = " ".join(toks[:i]), " ".join(toks[i:])

        for d in rest.split(","):
            d = d.strip()
            if not d:
                continue
            stars = d.count("*")
            d = d.replace("*", "").strip()
            am = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]$", d)
            if am:
                name, n = am.group(1), int(am.group(2))
                if stars:
                    raise Unsupported("%s.%s: pointer arrays are not supported" % (struct_name, name))
                if base not in ("char", "signed char", "unsigned char"):
                    raise Unsupported("%s.%s: only byte arrays are supported (got %s[])"
                                      % (struct_name, name, base))
                out.append((name, n, 1, "s"))
                continue
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", d):
                raise Unsupported("%s: cannot parse declarator %r" % (struct_name, d))
            if stars:
                out.append((d, PTR[0], PTR[1], PTR[2]))
            elif base in SCALARS:
                sz, al, kind = SCALARS[base]
                out.append((d, sz, al, kind))
            else:
                raise Unsupported("%s.%s: unsupported type %r" % (struct_name, d, base))
    return out


def lay_out(members):
    """Standard C layout: pad each member up to its alignment, round the struct
    up to the widest member's alignment."""
    off, salign = 0, 1
    fields = []
    for name, size, align, kind in members:
        off = (off + align - 1) // align * align
        fields.append((name, off, size, kind))
        off += size
        salign = max(salign, align)
    return fields, (off + salign - 1) // salign * salign


def as_name(cname):
    """logit_time -> Time. Mechanical, so there is no mapping table to drift."""
    base = cname[len("logit_"):] if cname.startswith("logit_") else cname
    return base[:1].upper() + base[1:]


def read_structs():
    src = open(HEADER, encoding="utf-8").read()
    out = []
    for m in re.finditer(r"\bstruct\s+(logit_[A-Za-z0-9_]+)\s*\{([^{}]*)\}\s*;", src, re.S):
        name, body = m.group(1), m.group(2)
        body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
        body = re.sub(r"//[^\n]*", " ", body)
        fields, size = lay_out(parse_members(name, body))
        out.append((name, fields, size))
    if not out:
        raise SystemExit("gen_abi.py: no structs found in %s" % HEADER)
    return out


BANNER_AS = """\
# GENERATED by tools/gen_abi.py from include/abi/logit_abi.h -- DO NOT EDIT.
#
# The kernel's structs, as AetherScript layouts. `t = Time()` allocates one the
# collector owns; `addr(t)` is exactly the pointer a syscall expects; `t.hour`
# reads the field at the offset below.
#
# These offsets are not this file's opinion of the layout. Each one is also
# emitted as a _Static_assert in c/apps/as/abi_layout.inc, which as_native.c
# includes -- so the compiler that builds /bin/as checks every number here
# against the real struct, and a kernel that reorders a field breaks the build
# instead of leaving a script reading the wrong bytes.
#
# Regenerate:  python3 tools/gen_abi.py --write
# Verify:      make check-abi
"""

BANNER_INC = """\
/* GENERATED by tools/gen_abi.py from include/abi/logit_abi.h -- DO NOT EDIT.
 *
 * Every offset and size that fsroot/as/lib/abi.as hands to layout(), asserted
 * against the real struct. Included by as_native.c, which already includes
 * logit_abi.h, so this is checked by BOTH the host build and the x86_64-elf
 * target build of /bin/as.
 *
 * A failure here means abi.as is stale, not that the kernel is wrong:
 *     python3 tools/gen_abi.py --write
 *
 * (A field renamed at the same offset passes these; `make check-abi` catches
 * that by regenerating and diffing the names.) */
"""


def render():
    structs = read_structs()

    a = [BANNER_AS]
    for cname, fields, size in structs:
        a.append("")
        a.append("%s = layout(\"%s\", %d, [" % (as_name(cname), cname, size))
        for i, (fname, off, fsize, kind) in enumerate(fields):
            comma = "," if i + 1 < len(fields) else ""
            a.append("    [\"%s\", %d, %d, \"%s\"]%s" % (fname, off, fsize, kind, comma))
        a.append("])")
    a.append("")

    c = [BANNER_INC]
    for cname, fields, size in structs:
        c.append("")
        c.append("_Static_assert(sizeof(struct %s) == %d,\n"
                 "               \"abi.as is stale: sizeof(struct %s) changed -- run tools/gen_abi.py --write\");"
                 % (cname, size, cname))
        for fname, off, fsize, kind in fields:
            c.append("_Static_assert(offsetof(struct %s, %s) == %d,\n"
                     "               \"abi.as is stale: %s.%s moved -- run tools/gen_abi.py --write\");"
                     % (cname, fname, off, cname, fname))
            c.append("_Static_assert(sizeof(((struct %s *)0)->%s) == %d,\n"
                     "               \"abi.as is stale: %s.%s changed width -- run tools/gen_abi.py --write\");"
                     % (cname, fname, fsize, cname, fname))
    c.append("")
    return "\n".join(a), "\n".join(c)


def main(argv):
    mode = argv[1] if len(argv) > 1 else "--check"
    if mode not in ("--check", "--write"):
        raise SystemExit(__doc__)
    try:
        text_as, text_inc = render()
    except Unsupported as e:
        raise SystemExit("gen_abi.py: %s\n  (refusing to guess -- teach the generator or "
                         "change the struct)" % e)

    if mode == "--write":
        for path, text in ((OUT_AS, text_as), (OUT_INC, text_inc)):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("gen_abi.py: wrote %s" % os.path.relpath(path, ROOT))
        return 0

    bad = []
    for path, text in ((OUT_AS, text_as), (OUT_INC, text_inc)):
        cur = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else None
        if cur != text:
            bad.append(os.path.relpath(path, ROOT))
    if bad:
        print("check-abi: STALE -- %s does not match include/abi/logit_abi.h" % ", ".join(bad))
        print("  regenerate with: python3 tools/gen_abi.py --write")
        return 1
    n = len(read_structs())
    print("check-abi: ok (%d kernel structs; offsets also asserted at compile time)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
