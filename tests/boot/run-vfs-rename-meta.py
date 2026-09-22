#!/usr/bin/env python3
"""Check directory metadata moves through real guest syscalls on a private disk.

The host gate covers capacity and mount-boundary cases. This smaller acceptance
adds actual ring-3 credentials, hard-link reads and a failed long-path rename.
It deliberately mounts RAMFS: LogitFS's durable inode modes would hide a broken
move of the VFS-owned metadata records.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = ("login", "sh", "echo", "cat", "mkdir", "mv", "rm", "stat")
SOURCES = ("c/fs/vfs/vfs.c", "c/fs/vfs/vfs_meta.c", "c/fs/vfs/vfs_meta.h",
           "c/fs/ramfs/ramfs.c", "tests/boot/run-vfs-rename-meta.py")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_guest(guest, checks):
    def check(name, command, *, status=0, output=None, contains=()):
        # Keep fixture redirections inside the captured child shell.
        # The outer root shell opens the output file before a child drops uid.
        actual = guest.capture("/bin/sh -c " + shlex.quote(command), timeout=60)
        result = {"name": name, "command": command, "exit": guest.last_capture_exit,
                  "output": actual}
        checks.append(result)
        valid = result["exit"] == status
        if output is not None:
            valid = valid and actual == output
        valid = valid and all(text in actual for text in contains)
        result["passed"] = valid
        if not valid:
            raise RuntimeError(json.dumps(result, ensure_ascii=False))
        print("PASS guest VFS rename:", name, flush=True)
        return actual

    check("create private mount point", "/bin/mkdir /vmeta")
    check("request independent RAMFS", "/bin/echo mount ramfs rename-meta /vmeta > /dev/vfsctl")
    check("mount independent RAMFS", "/bin/cat /dev/vfsctl",
          output="ok mounted ramfs rename-meta at /vmeta\n")
    check("mount table confirms RAMFS", "/bin/cat /dev/vfsmounts",
          contains=("rename-meta /vmeta\n",))
    # This shell treats && as background operators and ; as an ordinary word.
    # The original combined fixture returned '[1] background' in a real guest.
    # Separate captures check each operation's actual exit status instead.
    for command in (
        "/bin/mkdir /vmeta/tree",
        "/bin/echo PRIVATE_VFS_RENAME > /vmeta/tree/private",
        "/bin/echo PUBLIC_VFS_RENAME > /vmeta/tree/public",
        "/bin/stat -c 600 /vmeta/tree/private",
        "/bin/stat -o 1000:2000 /vmeta/tree/private",
        "/bin/stat -h /vmeta/tree/private /vmeta/outside",
        "/bin/stat -h /vmeta/tree/private /vmeta/tree/alias",
        "/bin/stat -s private /vmeta/tree/relative",
        "/bin/stat -s /vmeta/tree/private /vmeta/tree/absolute",
        "/bin/mv /vmeta/tree /vmeta/moved",
    ):
        check("seed: " + command, command)
    check("mode owner and hard-link count moved", "/bin/stat /vmeta/moved/private",
          contains=("mode=600 type=reg uid=1000 gid=2000 nlink=3",))
    check("hard links still resolve", "/bin/cat /vmeta/outside /vmeta/moved/alias",
          output="PRIVATE_VFS_RENAME\n" * 2)
    check("relative symlink resolves", "/bin/cat /vmeta/moved/relative",
          output="PRIVATE_VFS_RENAME\n")
    check("absolute symlink text remains literal", "/bin/stat -P /vmeta/moved/absolute",
          contains=("target=/vmeta/tree/private\n",))
    check("absolute symlink is now dangling", "/bin/cat /vmeta/moved/absolute",
          status=1, output="")

    # su changes echo's parent, which is a disposable script-reading shell.
    # The interactive transport shell stays root for subsequent independent
    # checks. A public read proves the denied private read is not a dead child.
    owner = check("owner can still read private bytes",
                  "/bin/sh < /state/owner.sh",
                  contains=("uid 1000 gid 2000", "PRIVATE_VFS_RENAME\n"))
    denied = check("other uid reads public but cannot read private bytes",
                   "/bin/sh < /state/outsider.sh",
                   status=1, contains=("uid 3000 gid 3000", "PUBLIC_VFS_RENAME\n"))
    if "PRIVATE_VFS_RENAME" in denied or "PUBLIC_VFS_RENAME" in owner:
        raise RuntimeError("credential read outputs leaked or were mixed")
    check("unlink canonical name", "/bin/rm /vmeta/moved/private")
    check("canonical unlink preserves both remaining names",
          "/bin/cat /vmeta/outside /vmeta/moved/alias",
          output="PRIVATE_VFS_RENAME\n" * 2)

    capacity = int(re.search(r"^#define VMETA_PATH\s+(\d+)",
                            (ROOT / "c/fs/vfs/vfs_meta.h").read_text(), re.M)[1])
    leaf = "s" * 56
    source = "/vmeta/a/" + leaf
    destination = "/vmeta/" + "d" * 56
    boundary = "/vmeta/" + "d" * (capacity - 2 - len(leaf) - len("/vmeta/"))
    for command in (
        "/bin/mkdir /vmeta/a",
        "/bin/echo KEPT_VFS_RENAME > /vmeta/a/file",
        "/bin/stat -c 600 /vmeta/a/file",
        "/bin/stat -s file " + source,
    ):
        check("long-path seed: " + command, command)
    check("overflow refuses before moving backend bytes",
          "/bin/mv /vmeta/a " + destination, status=1, output="")
    check("refusal retains bytes and symlink", "/bin/cat /vmeta/a/file " + source,
          output="KEPT_VFS_RENAME\n" * 2)
    check("refusal retains stored mode", "/bin/stat /vmeta/a/file",
          contains=("mode=600 type=reg uid=0 gid=0",))
    check("refusal leaves destination absent", "/bin/stat " + destination,
          status=1, output="")
    check("exact metadata capacity still moves", "/bin/mv /vmeta/a " + boundary)
    check("exact capacity retains symlink bytes", "/bin/cat " + boundary + "/" + leaf,
          output="KEPT_VFS_RENAME\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="kernel ISO directory")
    parser.add_argument("--base", type=Path, help="coreutils directory; defaults to --build")
    parser.add_argument("--out", type=Path, required=True, help="new private artifact directory")
    args = parser.parse_args()
    build = args.build.resolve()
    base = args.base.resolve() if args.base else build
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "checks": [], "kernel_sha256": digest(build / "logit.iso"),
              "programs": {name: digest(base / (name + ".aex")) for name in PROGRAMS},
              "sources": {name: digest(ROOT / name) for name in SOURCES}}
    guest = None
    try:
        seed = out / "state.txt"
        seed.write_text("VFS rename acceptance\n")
        files = [f"{base}/{name}.aex:/bin/{name}" for name in PROGRAMS]
        files.append(f"{seed}:/state/fixture")
        for name, uid, gid in (("owner", 1000, 2000), ("outsider", 3000, 3000)):
            script = out / (name + ".sh")
            lines = [f"/bin/echo su {uid} {gid} > /dev/vfsctl", "/bin/cat /dev/vfsctl"]
            if name == "outsider":
                lines.append("/bin/cat /vmeta/moved/public")
            lines.extend(("/bin/cat /vmeta/moved/private", "exit"))
            script.write_text("\n".join(lines) + "\n")
            files.append(f"{script}:/state/{name}.sh")
        with (out / "mkfs.log").open("w") as log:
            subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=60)
        report["initial_disk_sha256"] = digest(out / "disk.img")
        spec = importlib.util.spec_from_file_location("vfs_guest", ROOT / "tests/boot/run-agent.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        guest = module.Guest(build, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        check_guest(guest, report["checks"])
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if guest:
            guest.close()
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
