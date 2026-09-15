# SPDX-License-Identifier: MIT
"""Copy explicitly reserved user-state subtrees from a checked LogitFS image.

fsck/journal semantics stay in c/fs/fsck.c through lfs_snapshot. This reader
only walks that validated image and imports opaque bytes into mkfs.Builder;
no Cookie, storage, session or identity value is interpreted or logged.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import mkfs


class CheckedImage:
    def __init__(self, path, *, allow_identity=False):
        self.data = Path(path).read_bytes()
        self.sb = struct.unpack_from("<13I", self.data)
        if self.sb[0] != mkfs.MAGIC or self.sb[2] != mkfs.BS or self.sb[1] not in ((mkfs.VERSION, 5) if allow_identity else (mkfs.VERSION,)):
            raise ValueError("unsupported profile image")
        self.visited = set()

    def inode(self, number):
        if not 0 <= number < self.sb[4]:
            raise ValueError("profile inode is out of range")
        off = self.sb[7] * mkfs.BS + number * mkfs.INODE_SIZE
        raw = self.data[off:off + mkfs.INODE_SIZE]
        kind, _, size = struct.unpack_from("<HHI", raw)
        if kind not in (mkfs.T_FILE, mkfs.T_DIR):
            raise ValueError("profile references an unsupported inode")
        return kind, size, raw

    def block(self, number):
        if not self.sb[9] <= number < self.sb[3]:
            raise ValueError("profile block is out of range")
        return self.data[number * mkfs.BS:(number + 1) * mkfs.BS]

    def payload(self, number):
        _, size, raw = self.inode(number)
        count = (size + mkfs.BS - 1) // mkfs.BS
        if count > mkfs.NDIRECT + mkfs.PPB + mkfs.PPB * mkfs.PPB:
            raise ValueError("profile size is out of range")
        blocks = list(struct.unpack_from("<12I", raw, mkfs.OFF_DIRECT))[:min(count, mkfs.NDIRECT)]
        if count > mkfs.NDIRECT:
            indirect = struct.unpack_from("<I", raw, mkfs.OFF_INDIRECT)[0]
            blocks.extend(struct.unpack("<1024I", self.block(indirect))[:min(count - mkfs.NDIRECT, mkfs.PPB)])
        remaining = count - len(blocks)
        if remaining > 0:
            outer = struct.unpack_from("<I", raw, mkfs.OFF_DINDIRECT)[0]
            for inner in struct.unpack("<1024I", self.block(outer))[:(remaining + mkfs.PPB - 1) // mkfs.PPB]:
                take = min(count - len(blocks), mkfs.PPB)
                blocks.extend(struct.unpack("<1024I", self.block(inner))[:take])
        return b"".join(self.block(b) for b in blocks)[:size]

    def directory(self, number):
        if self.inode(number)[0] != mkfs.T_DIR:
            raise ValueError("profile path traverses a file")
        data = self.payload(number)
        if len(data) % mkfs.DIRENT:
            raise ValueError("malformed profile directory")
        entries = {}
        for off in range(0, len(data), mkfs.DIRENT):
            child = struct.unpack_from("<I", data, off)[0]
            field = data[off + 4:off + mkfs.DIRENT]
            if not field[0]:
                continue
            if b"\0" not in field:
                raise ValueError("unterminated profile name")
            name = field.split(b"\0", 1)[0].decode("utf-8")
            if name in (".", "..") or "/" in name or name in entries:
                raise ValueError("ambiguous profile directory entry")
            entries[name] = child
        return entries

    def resolve(self, path):
        number = self.sb[10]
        for part in path.strip("/").split("/"):
            if not part or part in (".", ".."):
                raise ValueError("invalid profile preservation root")
            entries = self.directory(number)
            if part not in entries:
                return None
            number = entries[part]
        return number

    def copy_tree(self, builder, number, path, merge=False):
        if number in self.visited:
            raise ValueError("profile inode is multiply linked")
        self.visited.add(number)
        builder._check_path(path)
        kind, _, raw = self.inode(number)
        parts = path.strip("/").split("/")
        parent = builder.get_or_make_dir(parts[:-1])
        existing = builder.lookup(parent, parts[-1])
        if existing is not None:
            if not merge:
                raise ValueError("packaged files conflict with a reserved user-state path")
            if builder.itype[existing] != kind:
                raise ValueError("preserved configuration changes a packaged path's type")
            # /etc contains packaged defaults AND administrator configuration.
            # Explicit merge retains old bytes/metadata, while new packaged
            # siblings remain available. Strict reserved roots stay strict.
            new = existing
            if kind == mkfs.T_FILE:
                builder.content[new] = self.payload(number)
        elif kind == mkfs.T_DIR:
            new = builder.get_or_make_dir(parts)
        else:
            builder.add_file(path, self.payload(number))
            new = builder.lookup(parent, parts[-1])
        builder.metadata[new] = raw[mkfs.OFF_ATIME:mkfs.OFF_GID + 4]
        count = 1
        if kind == mkfs.T_DIR:
            for name, child in self.directory(number).items():
                count += self.copy_tree(builder, child, path + "/" + name, merge)
        return count


def preserve(builder, image, roots, helper, merge_roots=()):
    if not (roots or merge_roots) or not Path(image).exists():
        return 0
    if not helper:
        raise ValueError("preserving a profile requires the filesystem snapshot helper")
    with tempfile.TemporaryDirectory(prefix=".profile-copy-", dir=Path(image).absolute().parent) as tmp:
        copy = Path(tmp) / "checked.img"
        result = subprocess.run([str(Path(helper).absolute()), str(Path(image).absolute()), str(copy)])
        if result.returncode:
            raise RuntimeError("filesystem snapshot refused; original disk retained")
        source = CheckedImage(copy)
        count = 0
        for root, merge in [(p, False) for p in roots] + [(p, True) for p in merge_roots]:
            number = source.resolve(root)
            if number is not None:
                count += source.copy_tree(builder, number, root.rstrip("/"), merge)
        return count


def atomic_write(path, data):
    from disk_guard import assert_unused
    path = Path(path).absolute()
    fd, tmp = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            if out.write(data) != len(data):
                raise OSError("incomplete disk image write")
            out.flush()
            os.fsync(out.fileno())
        # The shared lock covers cooperative launches. Check again for a
        # directly launched/legacy QEMU before replacing the old directory entry.
        assert_unused(path)
        os.replace(tmp, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
