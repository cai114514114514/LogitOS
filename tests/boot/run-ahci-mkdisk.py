#!/usr/bin/env python3
"""Wrap a raw LogitFS image in a partition table, for the AHCI boot tests.

tools/mkfs.py writes a filesystem that starts at LBA 0 of the device, which is
what every harness in this tree has ever booted. That is exactly the case a
partition-table implementation does NOT exercise: the filesystem is at sector 0
either way. So this builds the other two shapes --

  mbr   an MBR disk with a filler primary AND an extended container whose one
        logical partition holds the filesystem. The logical is the point: the
        extended chain is a linked list read off the disk, and putting the root
        filesystem behind it means a kernel that cannot walk that chain cannot
        boot at all.

  gpt   a GPT disk, complete: protective MBR, primary header + entry array at
        the front, backup entry array + header at the back, both CRC32s correct.
        A GPT with a wrong CRC is supposed to be rejected, so an image whose CRCs
        were merely plausible would test nothing.

Usage: run-ahci-mkdisk.py <fs.img> <out.img> <mbr|gpt>
"""
import binascii
import struct
import sys

SECTOR = 512
FS_START = 2048                 # 1 MiB in, the alignment every partitioner uses
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ARRAY_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR      # 32

# "Linux filesystem data" -- a real type GUID, so the boot log prints something
# a reader can look up rather than a made-up one that looks like a bug.
TYPE_GUID = b"\xaf\x3d\xc6\x0f\x83\x84\x72\x47\x8e\x79\x3d\x69\xd8\x47\x7d\xe4"
PART_GUID = b"\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff\x00"
DISK_GUID = b"\x0f\x1e\x2d\x3c\x4b\x5a\x69\x78\x87\x96\xa5\xb4\xc3\xd2\xe1\xf0"


def mbr_entry(boot, ptype, start, count):
    """One 16-byte MBR partition record. The CHS fields are filled with the
    0xFE 0xFF 0xFF 'too big for CHS' sentinel that every modern partitioner
    writes; nothing here reads them, but a zeroed CHS is the kind of detail that
    makes an image look synthetic to a tool somebody later checks it with."""
    return struct.pack("<B3sB3sII",
                       0x80 if boot else 0x00, b"\xfe\xff\xff",
                       ptype, b"\xfe\xff\xff",
                       start, count)


def build_mbr(fs, out):
    fs_sectors = (len(fs) + SECTOR - 1) // SECTOR
    filler_start, filler_count = FS_START, 2048
    ebr_lba = filler_start + filler_count            # 4096
    logical_off = 2048                               # relative to the EBR
    logical_lba = ebr_lba + logical_off              # 6144
    total = logical_lba + fs_sectors + 64

    img = bytearray(total * SECTOR)

    mbr = bytearray(SECTOR)
    mbr[0x1BE:0x1CE] = mbr_entry(False, 0x83, filler_start, filler_count)
    mbr[0x1CE:0x1DE] = mbr_entry(False, 0x05, ebr_lba, total - ebr_lba)
    mbr[510:512] = b"\x55\xaa"
    img[0:SECTOR] = mbr

    ebr = bytearray(SECTOR)
    # Slot 0 is relative to THIS EBR; slot 1 (absent here: the chain ends) would
    # be relative to the extended container. Two different bases in one sector is
    # the trap the kernel-side walker has to get right.
    ebr[0x1BE:0x1CE] = mbr_entry(True, 0x83, logical_off, fs_sectors)
    ebr[510:512] = b"\x55\xaa"
    img[ebr_lba * SECTOR:(ebr_lba + 1) * SECTOR] = ebr

    img[logical_lba * SECTOR:logical_lba * SECTOR + len(fs)] = fs

    out.write(bytes(img))
    return ("MBR: p1 filler at %d (+%d), extended at %d, LOGICAL p2 = filesystem "
            "at %d (+%d sectors), %d total" %
            (filler_start, filler_count, ebr_lba, logical_lba, fs_sectors, total))


def gpt_header(my_lba, alt_lba, entry_lba, first_usable, last_usable, disk_sectors, array_crc):
    h = bytearray(92)
    struct.pack_into("<8sIII", h, 0, b"EFI PART", 0x00010000, 92, 0)
    struct.pack_into("<I", h, 20, 0)
    struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, first_usable, last_usable)
    h[56:72] = DISK_GUID
    struct.pack_into("<QIII", h, 72, entry_lba, GPT_ENTRIES, GPT_ENTRY_SIZE, array_crc)
    # The header CRC is computed over the header with its own CRC field zeroed --
    # which it already is, so this is the one and only place it gets filled in.
    struct.pack_into("<I", h, 16, binascii.crc32(bytes(h)) & 0xFFFFFFFF)
    assert disk_sectors  # kept for the caller's sanity, not used in the header
    return bytes(h) + b"\x00" * (SECTOR - 92)


def build_gpt(fs, out):
    fs_sectors = (len(fs) + SECTOR - 1) // SECTOR
    first_usable = 2 + GPT_ARRAY_SECTORS                      # 34
    tail = GPT_ARRAY_SECTORS + 1                              # backup array + header
    total = FS_START + fs_sectors + tail + 64
    last_usable = total - tail - 1

    entries = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)
    name = "LOGITOS".encode("utf-16-le")
    struct.pack_into("<16s16sQQQ72s", entries, 0,
                     TYPE_GUID, PART_GUID,
                     FS_START, FS_START + fs_sectors - 1, 0,
                     name + b"\x00" * (72 - len(name)))
    array_crc = binascii.crc32(bytes(entries)) & 0xFFFFFFFF

    primary_array = 2
    backup_array = total - tail
    backup_hdr = total - 1

    img = bytearray(total * SECTOR)

    # Protective MBR: one 0xEE entry covering the disk. Its purpose is to make a
    # tool that only understands MBR see a full, unknown disk rather than free
    # space -- so the sector count is clamped to 32 bits on purpose.
    mbr = bytearray(SECTOR)
    mbr[0x1BE:0x1CE] = mbr_entry(False, 0xEE, 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    img[0:SECTOR] = mbr

    img[1 * SECTOR:2 * SECTOR] = gpt_header(1, backup_hdr, primary_array,
                                            first_usable, last_usable, total, array_crc)
    img[primary_array * SECTOR:primary_array * SECTOR + len(entries)] = entries
    img[backup_array * SECTOR:backup_array * SECTOR + len(entries)] = entries
    img[backup_hdr * SECTOR:(backup_hdr + 1) * SECTOR] = \
        gpt_header(backup_hdr, 1, backup_array, first_usable, last_usable, total, array_crc)

    img[FS_START * SECTOR:FS_START * SECTOR + len(fs)] = fs

    out.write(bytes(img))
    return ("GPT: protective MBR, header at 1 + backup at %d, entries at %d/%d, "
            "p1 'LOGITOS' = filesystem at %d (+%d sectors), %d total" %
            (backup_hdr, primary_array, backup_array, FS_START, fs_sectors, total))


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in ("mbr", "gpt"):
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        fs = f.read()
    with open(sys.argv[2], "wb") as out:
        desc = build_mbr(fs, out) if sys.argv[3] == "mbr" else build_gpt(fs, out)
    print(desc)


if __name__ == "__main__":
    main()
