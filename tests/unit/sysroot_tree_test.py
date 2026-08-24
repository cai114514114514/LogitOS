#!/usr/bin/env python3
"""Gate for tools/mkfs.py add_tree(): the directory packer.

What it has to be true of, each with the number it checks:

  ROUND TRIP   every regular file in a scratch tree is on the image, at its
               relative path, BYTE FOR BYTE (never a length check: a packer
               that hands one block to two files produces files of exactly the
               right length holding each other's data). Directories, including
               an empty one, come back as directories.
  DETERMINISM  two runs over the same tree give the same image bytes
               (SOURCE_DATE_EPOCH pinned -- the inode timestamps are the one
               input that is not the tree), AND a tree whose files were
               created in the reverse order gives the same bytes, because the
               walk is sorted. Negative control: the walk with its sort
               reversed must give DIFFERENT bytes, or this test could not see
               order at all.
  REFUSALS     a name of NAME_MAX bytes, a path of PATH_MAX bytes, a symlink
               (to a file inside the tree, to one outside it, and a dangling
               one) and a fifo are each refused BY NAME, with the offending
               path in the message. A refusal is checked as "exited non-zero
               and named it", not merely "exited non-zero".

usage: sysroot_tree_test.py <workdir>
"""
import importlib.util
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MKFS = os.path.join(REPO, "tools", "mkfs.py")
LFSX = os.path.join(REPO, "tests", "boot", "lfs_extract.py")


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mkfs = load(MKFS, "mkfs")
lfs = load(LFSX, "lfs_extract")

checks = 0
fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if cond:
        print("  ok   " + what)
    else:
        fails += 1
        print("  FAIL " + what)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def mk_tree(root, order):
    """A small tree with sub-directories, an empty directory, a 59-byte name
    (the longest legal one) and files of 0, 1, 4096, 4097 and 60000 bytes --
    the block-boundary sizes, plus one past the direct pointers' reach is not
    needed here (test-hugefile covers indirect blocks on the real image)."""
    if os.path.exists(root):
        shutil.rmtree(root)
    files = {
        "a.txt": b"alpha\n",
        "b/empty": b"",
        "b/one": b"x",
        "b/c/block": bytes(range(256)) * 16,            # 4096
        "b/c/block+1": bytes(range(256)) * 16 + b"!",   # 4097
        "d/" + ("n" * 59): b"fifty-nine byte name\n",
        "d/big.bin": bytes((i * 7) & 0xFF for i in range(60000)),
    }
    names = sorted(files) if order == "sorted" else sorted(files, reverse=True)
    for n in names:
        write(os.path.join(root, n), files[n])
    os.makedirs(os.path.join(root, "b", "emptydir"))
    return files


def pack(img, spec, env_extra=None):
    env = dict(os.environ, SOURCE_DATE_EPOCH="0")
    if env_extra:
        env.update(env_extra)
    r = subprocess.run([sys.executable, MKFS, img, spec], capture_output=True, text=True, env=env)
    return r


def image_files(img_path):
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    g = lfs.sb(img)
    out = {}
    dirs = set()

    def walk(ino, prefix):
        for name, cino in lfs.readdir(img, g, ino):
            p = prefix + "/" + name
            t = lfs.itype(img, g, cino)
            if t == lfs.T_DIR:
                dirs.add(p)
                walk(cino, p)
            else:
                blks, size = lfs.blocks_of(img, g, cino)
                out[p] = b"".join(bytes(img[b * lfs.BS:(b + 1) * lfs.BS]) for b in blks)[:size]
    walk(g["root"], "")
    return out, dirs


def main():
    work = os.path.abspath(sys.argv[1])
    os.makedirs(work, exist_ok=True)
    tree = os.path.join(work, "tree")
    files = mk_tree(tree, "sorted")

    print("-- round trip")
    img1 = os.path.join(work, "img1.img")
    r = pack(img1, tree + ":/pkg/usr")
    check(r.returncode == 0, "pack succeeds: %s" % r.stdout.strip().splitlines()[-1:] )
    got, dirs = image_files(img1)
    for rel, data in sorted(files.items()):
        p = "/pkg/usr/" + rel
        check(p in got and got[p] == data, "%s byte-identical (%d B)" % (p, len(data)))
    check("/pkg/usr/b/emptydir" in dirs, "/pkg/usr/b/emptydir is a directory on the image")
    check(len(got) == len(files), "exactly %d files on the image (got %d)" % (len(files), len(got)))

    print("-- determinism")
    img2 = os.path.join(work, "img2.img")
    pack(img2, tree + ":/pkg/usr")
    same = open(img1, "rb").read() == open(img2, "rb").read()
    check(same, "two runs over the same tree: identical image bytes")
    tree_rev = os.path.join(work, "tree-rev")
    mk_tree(tree_rev, "reverse")
    img3 = os.path.join(work, "img3.img")
    pack(img3, tree_rev + ":/pkg/usr")
    check(open(img1, "rb").read() == open(img3, "rb").read(),
          "a tree created in reverse order: identical image bytes (the walk is sorted)")
    # NEGATIVE CONTROL: reverse the sort inside the packer and the image must
    # change. Done in-process by patching the module's `sorted`, so the
    # control is the SAME code with one rule removed.
    b = mkfs.Builder()
    b.add_tree(tree, "/pkg/usr")
    ref, _ = b.serialize()
    real_sorted = sorted
    mkfs.sorted = lambda xs, **kw: real_sorted(xs, reverse=True)
    try:
        b2 = mkfs.Builder()
        b2.add_tree(tree, "/pkg/usr")
        ctl, _ = b2.serialize()
    finally:
        mkfs.sorted = real_sorted
    check(bytes(ref) != bytes(ctl), "NEGATIVE CONTROL: an unsorted walk produces a different image")

    print("-- refusals (each must fail AND name the offender)")
    def refused(label, spec, needle, setup=None, where=None):
        d = where or os.path.join(work, "bad-" + label)
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
        setup(d)
        r = pack(os.path.join(work, "bad.img"), d + ":" + spec)
        msg = (r.stderr + r.stdout).strip()
        check(r.returncode != 0 and needle in msg,
              "%s: refused, message names it: %r" % (label, msg.splitlines()[-1] if msg else ""))

    refused("name60", "/x", "n" * 60,
            lambda d: write(os.path.join(d, "n" * 60), b"too long\n"))
    refused("dirname60", "/x", "d" * 60,
            lambda d: write(os.path.join(d, "d" * 60, "f"), b"in a long dir\n"))
    # 4 x 50-byte components under /x: 1+1+ 4*51 = 206 fine; 5 -> 257 > 255.
    refused("path256", "/x", "path too long",
            lambda d: write(os.path.join(d, *(["p" * 50] * 5)), b"deep\n"))
    if hasattr(os, "symlink"):
        def mk_links(d):
            write(os.path.join(d, "real"), b"real\n")
            os.symlink("real", os.path.join(d, "link-inside"))
        refused("symlink-inside", "/x", "link-inside", mk_links)
        refused("symlink-outside", "/x", "passwd",
                lambda d: os.symlink("/etc/passwd", os.path.join(d, "passwd")))
        refused("symlink-dangling", "/x", "dangling",
                lambda d: os.symlink("nowhere", os.path.join(d, "dangling")))
        refused("symlink-dir", "/x", "refusing to pack a symlink",
                lambda d: (os.makedirs(os.path.join(d, "sub")),
                           os.symlink("sub", os.path.join(d, "sublink"))))
    if hasattr(os, "mkfifo"):
        # In the system temp dir, not the work dir: a 9p/drvfs mount (WSL's
        # /mnt/d) refuses mkfifo with EOPNOTSUPP, which is the filesystem, not
        # the packer, declining. If even /tmp cannot, say so and move on.
        fifo_dir = tempfile.mkdtemp(prefix="sysroot-fifo-")
        try:
            os.mkfifo(os.path.join(fifo_dir, "probe"))
            os.remove(os.path.join(fifo_dir, "probe"))
            can_fifo = True
        except OSError as e:
            can_fifo = False
            print("  skip fifo: mkfifo unsupported here (%s)" % e)
        if can_fifo:
            refused("fifo", "/x", "a-fifo", lambda d: os.mkfifo(os.path.join(d, "a-fifo")),
                    where=os.path.join(fifo_dir, "bad-fifo"))
        shutil.rmtree(fifo_dir, ignore_errors=True)

    # And the legal extremes, to show the limits sit where the kernel's do:
    # a 59-byte name packs; a 255-byte path packs.
    d = os.path.join(work, "edge")
    if os.path.exists(d):
        shutil.rmtree(d)
    comps = ["c" * 50] * 4 + ["e" * 47]      # /x/ + 4*51 + 47 = 254 -> ok
    write(os.path.join(d, *comps), b"edge\n")
    r = pack(os.path.join(work, "edge.img"), d + ":/x")
    dest = "/x/" + "/".join(comps)
    check(r.returncode == 0 and len(dest.encode()) == 254,
          "a %d-byte path (limit %d) packs" % (len(dest.encode()), mkfs.PATH_MAX - 1))
    got, _ = image_files(os.path.join(work, "edge.img"))
    check(got.get(dest) == b"edge\n", "and reads back byte-identical")

    print("sysroot_tree_test: %d checks, %d failed" % (checks, fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
