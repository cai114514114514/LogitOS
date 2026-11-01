#!/usr/bin/env python3
"""Build a FAT "EFI System Partition" disk image holding EFI/BOOT/BOOTX64.EFI
(the UEFI loader) and a kernel ELF at the root, for OVMF/QEMU (and real UEFI
firmware) to boot LogitOS from.

Usage:
    mkesp.py <out.img> --efi <BOOTX64.EFI> --kernel <kernel.elf>
             [--kernel-name LOGIT.ELF] [--size-mb N] [--layout superfloppy|gpt]
             [--file SRC:ESP_PATH ...] [--firmware-dir DIR]

SUPERFLOPPY, NOT GPT -- decided by experiment, not by guessing.
The UEFI spec's default-boot behaviour (spec 3.5.1.1's "removable media" boot
path) says firmware falls back to \\EFI\\BOOT\\BOOTX64.EFI on a FAT volume when
no NVRAM Boot#### variable names anything else -- and that volume can be a
whole-disk FAT filesystem with no partition table at all: a "superfloppy",
exactly the way a USB stick formatted straight to FAT32 in Windows boots. GPT
+ an ESP partition entry is the alternative this file's header describes, and
before writing this tool it was not obvious which one this repo's OVMF build
(/usr/share/OVMF/OVMF_CODE_4M.fd) actually honours. It was tried both ways:

    truncate -s 64M esp.img && mformat -i esp.img :: && mmd ... && mcopy ...
    qemu-system-x86_64 -machine q35 \\
        -drive if=pflash,readonly=on,file=OVMF_CODE_4M.fd \\
        -drive if=pflash,file=<vars-copy> \\
        -drive file=esp.img,format=raw,if=ide   # (also re-tried as virtio-blk-pci)

and BdsDxe's own serial log shows it finding and running the image with NO GPT
present, on both an IDE-attached and a virtio-blk-pci-attached superfloppy:

    BdsDxe: loading Boot0002 "UEFI QEMU HARDDISK QM00001 " from PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x0,0xFFFF,0x0)
    BdsDxe: starting Boot0002 "UEFI QEMU HARDDISK QM00001 " from PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x0,0xFFFF,0x0)

So this tool never writes a partition table. A superfloppy is also strictly
less code: no protective MBR, no GPT header/entry-array CRC32, one filesystem
instead of "a filesystem inside a partition inside a disk".

Correction 2026-10-31 (old claim retained above): superfloppy remains the
default, but --layout gpt now wraps the SAME FAT volume in a single ESP at
LBA 2048, with both GPT copies. --size-mb continues to size the FAT volume;
GPT adds 2048 leading and 33 trailing sectors. This gives firmware a second
removable-media layout without changing the old build interface.
Host/QEMU evidence only: not verified on X79-VG4 hardware.

EXTRA FILES (--file, --firmware-dir), added 2026-11-01 for stage B0 of the RX 580
driver (docs/superpowers/specs/2026-11-01-rx580-driver-design.md, section 4).
The root filesystem sits on a disk partition the owner can only reinstall from
Windows, so the USB stick has to be able to deliver the AMD microcode by itself.
The UEFI loader (c/boot/efi/loader.c) reads whatever \\LOGIT\\FW\\INDEX.TXT lists
before ExitBootServices and hands each file to the kernel as a boot module.

  --file SRC:ESP_PATH   copy host file SRC to ESP_PATH, repeatable. ESP_PATH
                        uses / or \\ and every component must fit an 8.3 short
                        name, on BOTH build paths: the from-scratch writer
                        cannot emit VFAT long names, and mtools would silently
                        emit them for a longer name, which a firmware FAT
                        driver and the loader's fixed paths then disagree about.
  --firmware-dir DIR    shorthand for the firmware set: every DIR/*.bin goes to
                        \\LOGIT\\FW\\FWnn.BIN and a generated \\LOGIT\\FW\\INDEX.TXT
                        records the name map.

THE NAME MAP. Real firmware names (polaris10_mec2_2.bin) are longer than 8.3,
so the ESP stores FWnn.BIN where nn is the 1-based, zero-padded position of the
real name in the sorted list of DIR/*.bin. INDEX.TXT, not that rule, is the
authority the loader reads: one line per file,

    FW01.BIN 8852 polaris10_ce.bin

i.e. "<8.3 name> <size in decimal> <real name>", space separated, LF ended.
The size lets the loader refuse a file the FAT driver delivers short; the real
name is what the kernel registry (c/kernel/init/bootmod.c) is asked for.

TWO BUILD PATHS, same on-disk result
  1. mtools (mformat/mcopy/mmd) if all three are on PATH -- it is a build-time
     HOST tool exactly like the i686-elf-grub-mkrescue/xorriso this repo
     already shells out to for the BIOS ISO (Makefile's $(ISO) rule), not a
     runtime dependency of the loader or the booted machine, so reaching for
     it here does not compromise "the loader is self-built".
  2. A from-scratch FAT16 writer (this file, no library), used when mtools is
     missing -- e.g. a CI box without mtools installed. It exists so this tool
     never HARD-depends on a host package the way logitfs's tools/mkfs.py
     never depends on any filesystem library for LogitFS itself.
Both paths are exercised by tests/uefi.mk (MKESP_FORCE_PYFAT=1 forces path 2
for its own boot-test run, so path 2 is not merely unit-tested against `mdir`
but proven to actually boot OVMF).

WHY FAT16 AND NOT FAT32: both are legal ESP filesystems (UEFI spec 13.3.1.1
requires FAT12/16/32 depending on volume size), but this image is small (a
multi-megabyte kernel ELF, not a multi-gigabyte Windows install), and FAT16
needs no FSInfo sector and no 32-bit-cluster bookkeeping -- one less format to
get subtly wrong. The two files placed at fixed paths (EFI/BOOT/BOOTX64.EFI,
and the kernel name, default LOGIT.ELF) are also chosen to fit an 8.3 short
name exactly, so the from-scratch writer never has to emit VFAT long-filename
entries either.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib

SECTOR = 512

# FAT16 cluster-count validity window (Microsoft "fatgen103", section on FAT
# type determination): below this the volume reads back as FAT12, at/above
# 65525 it reads back as FAT32 -- either way, firmware that trusts the CLUSTER
# COUNT (as every real FAT driver must; BPB_FATSz alone does not say which FAT
# type this is) decodes our fixed 16-bit FAT entries as the wrong width.
FAT16_MIN_CLUSTERS = 4085
FAT16_MAX_CLUSTERS = 65524


def die(msg):
    sys.exit(f"mkesp: {msg}")


# --------------------------------------------------------------------------
# Extra files on the ESP (--file / --firmware-dir)
# --------------------------------------------------------------------------

# Where the loader looks for the firmware set. c/boot/efi/loader.c spells the
# same two paths in UCS-2; change them together.
FIRMWARE_ESP_DIR = ("LOGIT", "FW")
FIRMWARE_INDEX_NAME = "INDEX.TXT"

# Real firmware names are copied into INDEX.TXT and later into a boot-module
# tag, so keep them to the characters linux-firmware names actually use and to
# a length the loader's fixed buffers hold.
FIRMWARE_NAME_MAX = 48
FIRMWARE_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_.-")


def esp_path_parts(esp_path):
    """Split an ESP path into upper-cased 8.3 components, or die.

    FAT names are case-insensitive; upper-casing here makes the duplicate and
    collision checks below compare what the filesystem will compare."""
    parts = [part for part in esp_path.replace("\\", "/").split("/") if part]
    if not parts:
        die(f"empty ESP path '{esp_path}'")
    for part in parts:
        short_name(part)
    return tuple(part.upper() for part in parts)


def parse_file_option(spec):
    """SRC:ESP_PATH -> (esp parts, bytes). Split on the LAST colon so a host
    path with a drive letter (C:\\fw\\a.bin:/LOGIT/FW/A.BIN) still parses."""
    src, colon, esp_path = spec.rpartition(":")
    if not colon or not src or not esp_path:
        die(f"--file wants SRC:ESP_PATH, got '{spec}'")
    if not os.path.isfile(src):
        die(f"--file: no such file: {src}")
    with open(src, "rb") as f:
        return esp_path_parts(esp_path), f.read()


def firmware_set(directory):
    """Every DIR/*.bin as FWnn.BIN under \\LOGIT\\FW, plus INDEX.TXT."""
    names = sorted(n for n in os.listdir(directory) if n.endswith(".bin"))
    if not names:
        die(f"--firmware-dir: no *.bin files in {directory}")
    if len(names) > 99:
        die("--firmware-dir: more than 99 files do not fit FWnn.BIN")
    files = []
    index_lines = []
    for position, name in enumerate(names, start=1):
        if len(name) > FIRMWARE_NAME_MAX or not set(name) <= FIRMWARE_NAME_CHARS:
            die(f"--firmware-dir: unsupported firmware name '{name}'")
        with open(os.path.join(directory, name), "rb") as f:
            blob = f.read()
        if not blob:
            die(f"--firmware-dir: {name} is empty")
        esp_name = f"FW{position:02d}.BIN"
        files.append((FIRMWARE_ESP_DIR + (esp_name,), blob))
        index_lines.append(f"{esp_name} {len(blob)} {name}\n")
    files.append((FIRMWARE_ESP_DIR + (FIRMWARE_INDEX_NAME,),
                  "".join(index_lines).encode("ascii")))
    return files


def check_extra_files(extra_files, kernel_name):
    """Refuse a duplicate path, a path that shadows one this tool already
    writes, and a name used as both a file and a directory."""
    taken = {("EFI", "BOOT", "BOOTX64.EFI"), (kernel_name.upper(),)}
    directories = {("EFI",), ("EFI", "BOOT")}
    for parts, _ in extra_files:
        if parts in taken:
            die(f"ESP path '/{'/'.join(parts)}' is written twice")
        taken.add(parts)
        for depth in range(1, len(parts)):
            directories.add(parts[:depth])
    clash = taken & directories
    if clash:
        die(f"ESP path '/{'/'.join(sorted(clash)[0])}' is both a file and a directory")


# --------------------------------------------------------------------------
# Path 1: mtools
# --------------------------------------------------------------------------

def have_mtools():
    return all(shutil.which(t) for t in ("mformat", "mcopy", "mmd"))


def build_with_mtools(out_path, size_mb, efi_bytes, kernel_bytes, kernel_name, extra_files=()):
    if os.path.exists(out_path):
        os.remove(out_path)
    with open(out_path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    def run(*args):
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            die(f"'{' '.join(args)}' failed:\n{r.stdout}{r.stderr}")

    # -i <img> :: addresses the image directly with no /etc/mtools.conf drive
    # letter and no partition suffix -- mtools' own convention for "this file
    # IS the filesystem, not a disk containing one", i.e. the superfloppy case.
    run("mformat", "-i", out_path, "-v", "LOGITESP", "::")
    run("mmd", "-i", out_path, "::/EFI")
    run("mmd", "-i", out_path, "::/EFI/BOOT")

    tmp_efi = out_path + ".efi.tmp"
    tmp_krn = out_path + ".krn.tmp"
    tmp_extra = out_path + ".extra.tmp"
    with open(tmp_efi, "wb") as f:
        f.write(efi_bytes)
    with open(tmp_krn, "wb") as f:
        f.write(kernel_bytes)
    try:
        run("mcopy", "-i", out_path, tmp_efi, "::/EFI/BOOT/BOOTX64.EFI")
        run("mcopy", "-i", out_path, tmp_krn, f"::/{kernel_name}")

        # Extra files: create each missing directory parents-first (mmd does
        # not make intermediate directories), then copy through a temp file for
        # the same reason as above -- the bytes live in memory, not on disk.
        made = {("EFI",), ("EFI", "BOOT")}
        for parts, blob in extra_files:
            for depth in range(1, len(parts)):
                if parts[:depth] not in made:
                    run("mmd", "-i", out_path, "::/" + "/".join(parts[:depth]))
                    made.add(parts[:depth])
            with open(tmp_extra, "wb") as f:
                f.write(blob)
            run("mcopy", "-i", out_path, tmp_extra, "::/" + "/".join(parts))
    finally:
        for tmp in (tmp_efi, tmp_krn, tmp_extra):
            if os.path.exists(tmp):
                os.remove(tmp)


# --------------------------------------------------------------------------
# Path 2: from-scratch FAT16 (mirrors tools/mkfs.py's house style: fixed
# struct offsets from the spec, no filesystem library, geometry computed by
# the same formula real formatters use rather than a guessed constant)
# --------------------------------------------------------------------------

def fat16_geometry(total_sectors, root_entries=512):
    """Microsoft "fatgen103" section 3.5's own FAT-size formula, walked over
    candidate SectorsPerCluster (powers of two, as the spec requires) until
    the resulting cluster count lands in the FAT16 window. Returns
    (sectors_per_cluster, fat_size_sectors, root_dir_sectors, cluster_count)
    or None if no candidate fits (image too small or absurdly large for
    FAT16 -- the caller turns that into a loud refusal, not a bad image)."""
    root_dir_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
    reserved_sectors = 1
    num_fats = 2
    for spc in (1, 2, 4, 8, 16, 32, 64, 128):
        tmp1 = total_sectors - (reserved_sectors + root_dir_sectors)
        tmp2 = 256 * spc + num_fats
        if tmp1 <= 0 or tmp2 <= 0:
            continue
        fat_sz = (tmp1 + tmp2 - 1) // tmp2
        data_sectors = total_sectors - (reserved_sectors + num_fats * fat_sz + root_dir_sectors)
        if data_sectors <= 0:
            continue
        cluster_count = data_sectors // spc
        if FAT16_MIN_CLUSTERS <= cluster_count <= FAT16_MAX_CLUSTERS:
            return spc, fat_sz, root_dir_sectors, cluster_count
    return None


def dos_datetime(t=None):
    t = time.localtime(t)
    date = ((max(t.tm_year, 1980) - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    time_ = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date & 0xFFFF, time_ & 0xFFFF


def short_name(name):
    """8.3-encode NAME.EXT. Every name this tool ever writes (BOOTX64.EFI,
    the default LOGIT.ELF / any --kernel-name the caller picks, and every
    component of a --file path) is required to already fit 8.3 -- refusing here is cheaper than silently
    emitting a VFAT long-name entry set this reader was never written to
    produce.

    "." and ".." are the one case the generic base.ext split gets wrong: the
    name IS the dot(s), so partitioning on "." peels it off as an empty base
    with an empty extension and this would silently write eleven spaces --
    a nameless directory entry that `mdir` prints as blank and that fsck-like
    tooling being written elsewhere in this tree would be right to flag.
    fatgen103 sec. 6 special-cases these two entries by name for exactly this
    reason, so this does too, before the general 8.3 rule ever sees them."""
    if name in (".", ".."):
        return name.ljust(11).encode("ascii")
    base, dot, ext = name.upper().partition(".")
    if len(base) > 8 or len(ext) > 3 or (not dot and "." in name):
        die(f"'{name}' does not fit an 8.3 short name (rename it; both build paths require 8.3)")
    return base.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")


class Fat16Image:
    """Accumulates directory entries and a cluster-indexed data area, then
    serializes boot sector + 2 FATs + root dir + data in one pass -- the same
    two-phase shape (build a tree, THEN lay out bytes) as mkfs.py's Builder,
    for the same reason: cluster/FAT-chain numbers cannot be assigned until
    every file that needs one has been added."""

    def __init__(self, total_sectors, root_entries=512):
        geo = fat16_geometry(total_sectors, root_entries)
        if geo is None:
            die(f"{total_sectors * SECTOR // 1024} KiB does not fit FAT16's cluster-count "
                f"window ({FAT16_MIN_CLUSTERS}-{FAT16_MAX_CLUSTERS} clusters) at any "
                f"SectorsPerCluster -- pick a different --size-mb")
        self.spc, self.fat_sz, self.root_dir_sectors, self.cluster_count = geo
        self.total_sectors = total_sectors
        self.root_entries = root_entries
        self.cluster_bytes = self.spc * SECTOR
        self.next_free_cluster = 2          # 0, 1 are reserved by the spec
        self.fat = [0] * (2 + self.cluster_count)
        self.fat[0] = 0xFFF8                # low byte mirrors BPB_Media (0xF8)
        self.fat[1] = 0xFFFF
        self.data = bytearray(self.cluster_count * self.cluster_bytes)
        self.root_dir = bytearray(self.root_entries * 32)
        self._root_used = 0

    def _alloc_chain(self, nbytes):
        n = max(1, (nbytes + self.cluster_bytes - 1) // self.cluster_bytes)
        first = self.next_free_cluster
        if first + n - 1 - 2 >= self.cluster_count:
            die("FAT16 image ran out of clusters -- pick a larger --size-mb")
        for i in range(n):
            cl = self.next_free_cluster + i
            self.fat[cl] = (self.next_free_cluster + i + 1) if i < n - 1 else 0xFFFF
        first_cluster = self.next_free_cluster
        self.next_free_cluster += n
        return first_cluster

    def _write_cluster_data(self, first_cluster, blob):
        off = (first_cluster - 2) * self.cluster_bytes
        self.data[off:off + len(blob)] = blob

    def _entry(self, buf, at, name, attr, cluster, size, dt):
        # Microsoft "fatgen103" table 5 (the 32-byte short directory entry):
        # Name[11] Attr NTRes CrtTimeTenth CrtTime CrtDate LstAccDate FstClusHI
        # WrtTime WrtDate FstClusLO FileSize -- 11s + 3B + 7H + I. A format
        # string with only 6 H's (one short of FstClusHI/FstClusLO both being
        # present) packed FstClusLO's value into where FileSize's bytes belong
        # and left FileSize reading FstClusLO's LOW 16 bits with the top 16
        # bits of whatever followed in the caller's argument tuple -- a
        # directory entry that names the right cluster but the wrong size,
        # which `mdir` would not even flag (it trusts the size next to the
        # name it just read) yet a firmware doing its own FAT walk uses to
        # decide when to stop. It was caught by pack_into simply refusing the
        # 12th positional value there is no field for.
        date, tm = dt
        struct.pack_into("<11sBBBHHHHHHHI", buf, at,
                          short_name(name), attr, 0, 0,
                          tm, date, date, 0, tm, date,
                          cluster, size)

    def add_root_file(self, name, blob, dt):
        cluster = self._alloc_chain(len(blob)) if blob else 0
        if blob:
            self._write_cluster_data(cluster, blob)
        self._entry(self.root_dir, self._root_used * 32, name, 0x20, cluster, len(blob), dt)
        self._root_used += 1

    def serialize(self):
        dt = dos_datetime()
        boot = bytearray(SECTOR)
        struct.pack_into("<3s8sHBHBHHBHHHIIBBBI11s8s", boot, 0,
                          b"\xeb\x3c\x90", b"LOGITESP",
                          SECTOR, self.spc, 1, 2, self.root_entries,
                          self.total_sectors if self.total_sectors < 0x10000 else 0,
                          0xF8, self.fat_sz, 63, 255,
                          0, self.total_sectors if self.total_sectors >= 0x10000 else 0,
                          0x80, 0, 0x29, 0x4C4F4749,
                          b"LOGIT ESP  ", b"FAT16   ")
        boot[510:512] = b"\x55\xaa"

        fat_bytes = bytearray(self.fat_sz * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)

        out = bytearray(self.total_sectors * SECTOR)
        out[0:SECTOR] = boot
        off = SECTOR
        for _ in range(2):                      # two identical FAT copies
            out[off:off + len(fat_bytes)] = fat_bytes
            off += self.fat_sz * SECTOR
        out[off:off + len(self.root_dir)] = self.root_dir
        off += self.root_dir_sectors * SECTOR
        out[off:off + len(self.data)] = self.data
        return bytes(out)


def write_directory_entries(img, children, dt):
    """Allocate and write everything below one directory; return its entries.

    CHILDREN maps an 8.3 name to file bytes or, for a subdirectory, to another
    such dict. A directory entry names a cluster its child must already own, so
    this recurses innermost first -- the order the fixed EFI/BOOT/BOOTX64.EFI
    chain used to be hard-coded in. Returns (name, attr, cluster, size) tuples
    for the caller to place in the root directory or in a new directory."""
    entries = []
    for name, node in children.items():
        if isinstance(node, dict):
            below = write_directory_entries(img, node, dt)
            entries.append((name, 0x10, make_subdir(img, below, dt), 0))
        else:
            cluster = img._alloc_chain(len(node)) if node else 0
            if node:
                img._write_cluster_data(cluster, node)
            entries.append((name, 0x20, cluster, len(node)))
    return entries


def make_subdir(img, children, dt):
    """Write a non-root directory holding CHILDREN; return its first cluster.

    This directory's own ".." is written as 0 (the root) and patched by ITS
    parent, in the loop below, once this directory's cluster is known:
    fatgen103 sec. 6 wants the real parent cluster there, 0 only for the root,
    and a firmware FAT walk going upward would otherwise land in the root from
    every directory."""
    payload = bytearray((2 + len(children)) * 32)
    for i, (cname, attr, ccluster, csize) in enumerate(children):
        img._entry(payload, (2 + i) * 32, cname, attr, ccluster, csize, dt)
    first = img._alloc_chain(len(payload))
    img._entry(payload, 0, ".", 0x10, first, 0, dt)
    img._entry(payload, 32, "..", 0x10, 0, 0, dt)
    img._write_cluster_data(first, payload)
    for cname, attr, ccluster, _ in children:
        if attr == 0x10:
            struct.pack_into("<H", img.data, (ccluster - 2) * img.cluster_bytes + 32 + 26,
                             first & 0xFFFF)
    return first


def build_with_pyfat(out_path, size_mb, efi_bytes, kernel_bytes, kernel_name, extra_files=()):
    total_sectors = size_mb * 1024 * 1024 // SECTOR
    img = Fat16Image(total_sectors)
    dt = dos_datetime()

    # One tree for every file the image holds, so EFI/BOOT/BOOTX64.EFI and the
    # extra files go through the same directory writer. Names are already
    # upper-cased 8.3 (short_name() would refuse anything else).
    tree = {"EFI": {"BOOT": {"BOOTX64.EFI": efi_bytes}}}
    for parts, blob in extra_files:
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = blob
    tree[kernel_name.upper()] = kernel_bytes

    for name, attr, cluster, size in write_directory_entries(img, tree, dt):
        img._entry(img.root_dir, img._root_used * 32, name, attr, cluster, size, dt)
        img._root_used += 1

    with open(out_path, "wb") as f:
        f.write(img.serialize())


# --------------------------------------------------------------------------

def gpt_header(current, alternate, last_usable, entries_lba, disk_guid, entries):
    """UEFI 2.10 section 5.3: CRC covers HeaderSize, with its CRC field zero.

    https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html
    The backup MUST have its own current/alternate/entry LBAs and CRC; copying
    the primary header verbatim appears bootable until firmware repairs GPT.
    """
    header = bytearray(SECTOR)
    struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART', 0x10000,
                     92, 0, 0, current, alternate, 34, last_usable, disk_guid,
                     entries_lba, 128, 128, zlib.crc32(entries))
    struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    return header


def write_backup_gpt(out, last_lba, header, entries):
    out.seek((last_lba - 32) * SECTOR)
    out.write(entries)
    out.write(header)


def wrap_gpt(out_path, fat_path):
    """Keep FAT construction independent of partitioning (both writers work)."""
    start = 2048
    fat_sectors = os.path.getsize(fat_path) // SECTOR
    last_lba = start + fat_sectors + 32
    last_usable = last_lba - 33
    entries = bytearray(128 * 128)
    struct.pack_into('<16s16sQQQ', entries, 0,
                     uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le,
                     uuid.uuid4().bytes_le, start, last_usable, 0)
    name = 'LogitOS ESP'.encode('utf-16le')
    entries[56:56 + len(name)] = name
    disk_guid = uuid.uuid4().bytes_le
    primary = gpt_header(1, last_lba, last_usable, 2, disk_guid, entries)
    backup = gpt_header(last_lba, 1, last_usable, last_lba - 32, disk_guid, entries)
    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xee,
                     b'\xff\xff\xff', 1, min(last_lba, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    with open(fat_path, 'r+b') as fat, open(out_path, 'wb') as out:
        # BPB_HiddSec is a whole-disk offset, not relative to the FAT volume.
        # Leaving the superfloppy's zero here silently disagrees with the GPT.
        fat.seek(28)
        fat.write(struct.pack('<I', start))
        fat.seek(0)
        out.truncate((last_lba + 1) * SECTOR)
        out.write(mbr)
        out.write(primary)
        out.write(entries)
        out.seek(start * SECTOR)
        shutil.copyfileobj(fat, out)
        write_backup_gpt(out, last_lba, backup, entries)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="output ESP image path")
    ap.add_argument("--layout", choices=('gpt', 'superfloppy'), default='superfloppy',
                    help="partition layout (default: superfloppy)")
    ap.add_argument("--efi", required=True, help="path to BOOTX64.EFI")
    ap.add_argument("--kernel", required=True, help="path to the kernel ELF")
    ap.add_argument("--kernel-name", default="LOGIT.ELF",
                     help="8.3 name the kernel is stored under at the ESP root (default LOGIT.ELF)")
    ap.add_argument("--size-mb", type=int, default=0,
                     help="image size in MiB (default: auto-sized from the input files)")
    ap.add_argument("--file", action="append", default=[], metavar="SRC:ESP_PATH",
                     help="also copy host file SRC to ESP_PATH (8.3 components); repeatable")
    ap.add_argument("--firmware-dir", metavar="DIR",
                     help="also store DIR/*.bin as /LOGIT/FW/FWnn.BIN plus INDEX.TXT (see header)")
    args = ap.parse_args()

    if not os.path.isfile(args.efi):
        die(f"no such EFI binary: {args.efi}")
    if not os.path.isfile(args.kernel):
        die(f"no such kernel ELF: {args.kernel}")
    efi_bytes = open(args.efi, "rb").read()
    kernel_bytes = open(args.kernel, "rb").read()
    # Sanity check the two names this tool ever WRITES to the FAT volume (not
    # args.efi's own path, which is a host filesystem path and irrelevant to
    # the image -- it always lands at the fixed EFI/BOOT/BOOTX64.EFI location).
    short_name("BOOTX64.EFI")
    short_name(args.kernel_name)

    extra_files = [parse_file_option(spec) for spec in args.file]
    if args.firmware_dir:
        extra_files += firmware_set(args.firmware_dir)
    check_extra_files(extra_files, args.kernel_name)

    size_mb = args.size_mb
    if size_mb <= 0:
        # Auto-size: both files, generous slack for FAT/dir overhead and a
        # floor comfortably above FAT16's minimum cluster count so a tiny
        # loader binary never produces a geometry with no valid SectorsPerCluster.
        needed = (len(efi_bytes) + len(kernel_bytes) +
                  sum(len(blob) for _, blob in extra_files)) / (1024 * 1024)
        size_mb = max(16, int(needed) + 8)

    force_py = os.environ.get("MKESP_FORCE_PYFAT") == "1"
    with tempfile.TemporaryDirectory(prefix='mkesp-') as directory:
        fat_path = os.path.join(directory, 'fat.img') if args.layout == 'gpt' else args.out
        if not force_py and have_mtools():
            build_with_mtools(fat_path, size_mb, efi_bytes, kernel_bytes, args.kernel_name,
                              extra_files)
            method = "mtools"
        else:
            if not force_py:
                print("mkesp: mtools not found on PATH, using the from-scratch FAT16 writer",
                      file=sys.stderr)
            build_with_pyfat(fat_path, size_mb, efi_bytes, kernel_bytes, args.kernel_name,
                             extra_files)
            method = "pyfat (from-scratch FAT16, no mtools)"
        if args.layout == 'gpt':
            wrap_gpt(args.out, fat_path)

    print(f"mkesp: {args.out} ({size_mb} MiB FAT volume, {args.layout}, via {method}) -- "
          f"EFI/BOOT/BOOTX64.EFI ({len(efi_bytes)} bytes), "
          f"{args.kernel_name} ({len(kernel_bytes)} bytes)"
          + (f", {len(extra_files)} extra file(s)" if extra_files else ""))


if __name__ == "__main__":
    main()
