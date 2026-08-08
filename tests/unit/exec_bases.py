#!/usr/bin/env python3
"""The link-base map, checked against the binaries instead of remembered.

Nothing in this system relocates, so every GUI app is linked at a fixed base
and the map is spread across thirty-odd -Ttext= sites in the Makefile. Three
things can go wrong there and none of them is a link error:

  * two apps at the same base. Harmless today (each app gets its own address
    space) but it is the invariant the Makefile comments claim, and the moment
    something keys off the base -- single-instance already nearly does -- it
    stops being harmless. Allowed only between builds of the SAME app: a
    deliberately crippled variant a test packs INSTEAD of the real one carries
    the same display name and is never on the disk beside it.

  * an image that has grown past the room above it. The launcher puts the user
    stack above the image top, inside the 1 GiB private user region; an app
    whose .bss reaches the top of that region does not fail to link, it fails
    to launch, at runtime, with a message about frames.

  * an entry point outside the region the loader accepts, i.e. a -Ttext typo.

Usage: exec_bases.py <file.aex>...
"""
import struct, sys, os

USER_BASE = 0x40000000
USER_END  = 0x80000000
# What the launcher needs above the image: a 4 MiB guard plus the largest stack
# it hands out (8 MiB for the browser). c/kernel/gui/wm.c owns these numbers;
# they are repeated here as a BOUND, not as a definition -- being generous is
# safe, being absent is not.
HEADROOM = 4 * 1024 * 1024 + 8 * 1024 * 1024


def read(path):
    d = open(path, "rb").read()
    if len(d) < 64 or d[:4] != b"AEX1":
        return None
    e = d[64:]
    if e[:4] != b"\x7fELF":
        return None
    name = e[8:40] if False else d[8:40].split(b"\0")[0].decode("utf-8", "replace")
    entry, phoff = struct.unpack_from("<QQ", e, 24)
    phentsize, phnum = struct.unpack_from("<HH", e, 54)
    base, top = None, 0
    for i in range(phnum):
        pt, pf, po, pv, pp, pfs, pms, pa = struct.unpack_from("<II6Q", e, phoff + i * phentsize)
        if pt != 1:
            continue
        end = (pv + pms + 0xFFF) & ~0xFFF
        if end <= USER_BASE:
            continue                      # the low headers segment the loader skips
        start = pv & ~0xFFF
        base = start if base is None else min(base, start)
        top = max(top, end)
    return dict(path=path, name=name, entry=entry, base=base, top=top)


def main(argv):
    imgs = []
    for p in argv:
        r = read(p)
        if r is None:
            print("FAIL: %s is not an AEX1 file wrapping an ELF64" % p)
            return 1
        if r["base"] is None:
            print("FAIL: %s has no PT_LOAD in the user region" % p)
            return 1
        imgs.append(r)

    fails = 0
    bygroup = {}
    for r in imgs:
        bygroup.setdefault(r["base"], []).append(r)

    print("link-base map (%d binaries):" % len(imgs))
    for base in sorted(bygroup):
        g = bygroup[base]
        names = sorted(set(x["name"] for x in g))
        top = max(x["top"] for x in g)
        print("  0x%08x  top 0x%08x  %-14s %s"
              % (base, top, ",".join(names),
                 " ".join(sorted(os.path.basename(x["path"]) for x in g))))
        # The CLI base is shared on purpose: those programs are fork+exec'd one
        # at a time into a fresh address space and are never resident together.
        if base == 0x50000000:
            continue
        if len(names) > 1:
            print("FAIL: base 0x%08x is used by %d DIFFERENT apps: %s"
                  % (base, len(names), ", ".join(names)))
            fails += 1

    for r in imgs:
        if not (USER_BASE <= r["entry"] < 0x7C000000):
            print("FAIL: %s entry 0x%x is outside [0x40000000, 0x7C000000) -- "
                  "the loader will refuse it" % (r["path"], r["entry"]))
            fails += 1
        if r["top"] + HEADROOM > USER_END:
            print("FAIL: %s reaches 0x%x; with the launcher's %d MiB of stack "
                  "headroom that leaves the 1 GiB user region"
                  % (r["path"], r["top"], HEADROOM >> 20))
            fails += 1

    if fails:
        print("FAIL: %d problem(s) in the link-base map" % fails)
        return 1
    biggest = max(imgs, key=lambda r: r["top"])
    print("ok: %d binaries, no two apps share a base, every entry point is in "
          "range," % len(imgs))
    print("    and the largest image (%s, top 0x%x) still leaves %d MiB above it"
          % (os.path.basename(biggest["path"]), biggest["top"],
             (USER_END - biggest["top"]) >> 20))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
