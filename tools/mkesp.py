#!/usr/bin/env python3
"""Build a FAT "EFI System Partition" disk image holding EFI/BOOT/BOOTX64.EFI
(the UEFI loader) and a kernel ELF at the root, for OVMF/QEMU (and real UEFI
firmware) to boot LogitOS from.

Usage:
    mkesp.py <out.img> --efi <BOOTX64.EFI> --kernel <kernel.elf>
             [--kernel-name LOGIT.ELF] [--size-mb N]

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
import time

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
# Path 1: mtools
# --------------------------------------------------------------------------

def have_mtools():
    return all(shutil.which(t) for t in ("mformat", "mcopy", "mmd"))


def build_with_mtools(out_path, size_mb, efi_bytes, kernel_bytes, kernel_name):
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
    with open(tmp_efi, "wb") as f:
        f.write(efi_bytes)
    with open(tmp_krn, "wb") as f:
        f.write(kernel_bytes)
    try:
        run("mcopy", "-i", out_path, tmp_efi, "::/EFI/BOOT/BOOTX64.EFI")
        run("mcopy", "-i", out_path, tmp_krn, f"::/{kernel_name}")
    finally:
        os.remove(tmp_efi)
        os.remove(tmp_krn)


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
    """8.3-encode NAME.EXT. Both names this tool ever writes (BOOTX64.EFI,
    and the default LOGIT.ELF / any --kernel-name the caller picks) are
    required to already fit 8.3 -- refusing here is cheaper than silently
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
        die(f"'{name}' does not fit an 8.3 short name (need mtools, or rename it)")
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


def build_with_pyfat(out_path, size_mb, efi_bytes, kernel_bytes, kernel_name):
    total_sectors = size_mb * 1024 * 1024 // SECTOR
    img = Fat16Image(total_sectors)
    dt = dos_datetime()

    # BOOTX64.EFI lives inside EFI/BOOT, so allocate its data+entry first and
    # nest that directory inside EFI, THEN root -- exactly the order a real
    # writer needs, innermost first, because a directory entry names a
    # cluster its child must already own.
    efi_cluster = img._alloc_chain(len(efi_bytes))
    img._write_cluster_data(efi_cluster, efi_bytes)

    # Build EFI/BOOT as a non-root directory, then EFI, then link EFI into the
    # root -- each level needs the one below it's first cluster already
    # assigned, hence innermost first.
    def make_subdir(children, parent_cluster):
        payload = bytearray((2 + len(children)) * 32)
        for i, (cname, attr, ccluster, csize) in enumerate(children):
            img._entry(payload, (2 + i) * 32, cname, attr, ccluster, csize, dt)
        first = img._alloc_chain(len(payload))
        img._entry(payload, 0, ".", 0x10, first, 0, dt)
        img._entry(payload, 32, "..", 0x10, parent_cluster, 0, dt)
        img._write_cluster_data(first, payload)
        return first

    boot_dir = make_subdir([("BOOTX64.EFI", 0x20, efi_cluster, len(efi_bytes))], 0)
    efi_dir = make_subdir([("BOOT", 0x10, boot_dir, 0)], 0)
    # Patch EFI/BOOT's '..' now that EFI's own cluster is known (it was written
    # as 0 above, root-relative; EFI is not root, so fix it up).
    off = (boot_dir - 2) * img.cluster_bytes
    struct.pack_into("<H", img.data, off + 32 + 26, efi_dir & 0xFFFF)

    img._entry(img.root_dir, img._root_used * 32, "EFI", 0x10, efi_dir, 0, dt)
    img._root_used += 1
    img.add_root_file(kernel_name, kernel_bytes, dt)

    with open(out_path, "wb") as f:
        f.write(img.serialize())


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="output ESP image path")
    ap.add_argument("--efi", required=True, help="path to BOOTX64.EFI")
    ap.add_argument("--kernel", required=True, help="path to the kernel ELF")
    ap.add_argument("--kernel-name", default="LOGIT.ELF",
                     help="8.3 name the kernel is stored under at the ESP root (default LOGIT.ELF)")
    ap.add_argument("--size-mb", type=int, default=0,
                     help="image size in MiB (default: auto-sized from the two input files)")
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

    size_mb = args.size_mb
    if size_mb <= 0:
        # Auto-size: both files, generous slack for FAT/dir overhead and a
        # floor comfortably above FAT16's minimum cluster count so a tiny
        # loader binary never produces a geometry with no valid SectorsPerCluster.
        needed = (len(efi_bytes) + len(kernel_bytes)) / (1024 * 1024)
        size_mb = max(16, int(needed) + 8)

    force_py = os.environ.get("MKESP_FORCE_PYFAT") == "1"
    if not force_py and have_mtools():
        build_with_mtools(args.out, size_mb, efi_bytes, kernel_bytes, args.kernel_name)
        method = "mtools"
    else:
        if not force_py:
            print("mkesp: mtools not found on PATH, using the from-scratch FAT16 writer",
                  file=sys.stderr)
        build_with_pyfat(args.out, size_mb, efi_bytes, kernel_bytes, args.kernel_name)
        method = "pyfat (from-scratch FAT16, no mtools)"

    print(f"mkesp: {args.out} ({size_mb} MiB, superfloppy FAT16, via {method}) -- "
          f"EFI/BOOT/BOOTX64.EFI ({len(efi_bytes)} bytes), "
          f"{args.kernel_name} ({len(kernel_bytes)} bytes)")


if __name__ == "__main__":
    main()
