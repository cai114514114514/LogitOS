#!/usr/bin/env python3
"""Write the deliberately small BIOS-bootable ISO used by LogitOS.

Usage:
    mkiso.py <out.iso> --boot-image <real-mode.bin> [--loader <real-mode.bin>]

SCOPE: ISO9660 with one primary volume descriptor, a root directory (and no
subdirectories), and one El Torito BIOS entry in no-emulation mode.  There is
deliberately no Joliet, Rock Ridge, multi-session support, UEFI catalog
section, or general file-mastering interface.  This replaces xorriso for one
measured LogitOS layout; growing it into a mastering tool would add parsers and
metadata that neither firmware nor our boot path consumes, making the code we
must trust before the kernel starts larger for no boot benefit.

The LBAs below were measured from the GRUB-built LogitOS ISO on 2026-09-15.
They are intentionally constants, not a layout optimiser: matching the known
booting artifact makes this first self-hosting stage answer one question at a
time.  A normal preload is one 2,048-byte ISO sector, represented by four
512-byte units in the El Torito catalog; larger boot images are accepted only
so the gate can measure the firmware's catalog-preload limit.  El Torito does
not require an MBR signature in that boot image, so this writer pads a short
input but never invents 0x55AA at image offset 510.

The catalog --negctl-* switches exist only so test-mkiso can build corrupt
images and compare its parser with SeaBIOS.  The loader-LBA control serves the
same purpose for test-bios-preload.  They are not alternate output formats and
exactly one may be used at a time.
"""

import argparse
import os
import struct
import sys


ISO_SECTOR = 2048
BIOS_SECTOR = 512
BASE_VOLUME_SECTORS = 218
PVD_LBA = 16
BOOT_RECORD_LBA = 17
TERMINATOR_LBA = 18
ROOT_DIR_LBA = 19
L_PATH_TABLE_LBA = 20
M_PATH_TABLE_LBA = 21
BOOT_CATALOG_LBA = 49
BOOT_IMAGE_LBA = 217
BOOT_LOAD_SEGMENT = 0x07C0
MIN_BOOT_SECTOR_COUNT = 4

# This is the authority for the preload patch wire format.  preload.asm mirrors
# it because assembly cannot import Python, and pins the structure at byte
# 0x1f0 so a moved/duplicated magic is rejected instead of patching plausible
# bytes.  The count is in the CD drive's native 2,048-byte blocks because that
# is the unit consumed by INT 13h AH=42h after the firmware handoff:
#
#   0x00  char[4]  "L2P!"       patch-record magic
#   0x04  uint32   loader LBA   little-endian ISO/native-sector LBA
#   0x08  uint16   block count  little-endian 2,048-byte block count
LOADER_PATCH_OFFSET = 0x1F0
LOADER_PATCH = struct.Struct("<4sIH")
LOADER_PATCH_MAGIC = b"L2P!"
LOADER_LOAD_MAX_BLOCKS = 127


def die(message):
    sys.exit(f"mkiso: {message}")


def put_721(buf, offset, value):
    struct.pack_into("<H", buf, offset, value)


def put_722(buf, offset, value):
    struct.pack_into(">H", buf, offset, value)


def put_723(buf, offset, value):
    put_721(buf, offset, value)
    put_722(buf, offset + 2, value)


def put_731(buf, offset, value):
    struct.pack_into("<I", buf, offset, value)


def put_732(buf, offset, value):
    struct.pack_into(">I", buf, offset, value)


def put_733(buf, offset, value):
    put_731(buf, offset, value)
    put_732(buf, offset + 4, value)


def padded_ascii(text, width):
    raw = text.encode("ascii")
    if len(raw) > width:
        raise ValueError(f"'{text}' is wider than its {width}-byte ISO9660 field")
    return raw.ljust(width, b" ")


def directory_record(extent_lba, data_length, flags, identifier):
    """Return one ISO9660 directory record with both-endian numeric fields."""
    if len(identifier) > 255:
        raise ValueError("ISO9660 directory identifier is too long")
    # The fixed 2026-09-15 timestamp is deliberate: image bytes should not
    # change merely because a gate ran later, while all seven fields remain a
    # valid ISO9660 recording date.  A host clock timestamp would make the
    # boot artifact unreproducible without affecting anything the BIOS reads.
    length = 33 + len(identifier)
    if length & 1:
        length += 1
    record = bytearray(length)
    record[0] = length
    put_733(record, 2, extent_lba)
    put_733(record, 10, data_length)
    record[18:25] = bytes((126, 9, 15, 0, 0, 0, 0))
    record[25] = flags
    put_723(record, 28, 1)
    record[32] = len(identifier)
    record[33:33 + len(identifier)] = identifier
    return bytes(record)


def primary_volume_descriptor(volume_sectors):
    pvd = bytearray(ISO_SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = padded_ascii("LOGITOS", 32)
    pvd[40:72] = padded_ascii("LOGITOS_BOOT", 32)
    put_733(pvd, 80, volume_sectors)
    put_723(pvd, 120, 1)                    # one-volume set
    put_723(pvd, 124, 1)                    # first and only volume
    put_723(pvd, 128, ISO_SECTOR)
    put_733(pvd, 132, 10)                   # one root path-table record
    put_731(pvd, 140, L_PATH_TABLE_LBA)
    put_732(pvd, 148, M_PATH_TABLE_LBA)
    pvd[156:190] = directory_record(ROOT_DIR_LBA, ISO_SECTOR, 0x02, b"\x00")
    pvd[190:318] = padded_ascii("LOGITOS", 128)
    pvd[574:702] = padded_ascii("LOGITOS MKISO", 128)
    pvd[881] = 1                            # file structure version
    return pvd


def boot_record_descriptor():
    record = bytearray(ISO_SECTOR)
    record[0:7] = b"\x00CD001\x01"
    # Unlike ISO9660's ordinary A-characters identifiers, SeaBIOS compares the
    # entire 32-byte boot-system field against the El Torito magic followed by
    # NULs.  Space-padding looks reasonable and xorriso even reports such an
    # image as El Torito, but SeaBIOS then fails before reading the catalog
    # (measured as "Could not read from CDROM (code 0005)").
    boot_system_id = b"EL TORITO SPECIFICATION"
    record[7:7 + len(boot_system_id)] = boot_system_id
    put_731(record, 71, BOOT_CATALOG_LBA)
    return record


def terminator_descriptor():
    descriptor = bytearray(ISO_SECTOR)
    descriptor[0:7] = b"\xffCD001\x01"
    return descriptor


def path_table(big_endian):
    table = bytearray(ISO_SECTOR)
    table[0] = 1                            # root identifier length
    if big_endian:
        put_732(table, 2, ROOT_DIR_LBA)
        put_722(table, 6, 1)
    else:
        put_731(table, 2, ROOT_DIR_LBA)
        put_721(table, 6, 1)
    table[8] = 0
    return table


def root_directory(boot_image_bytes):
    root = bytearray(ISO_SECTOR)
    records = (
        directory_record(ROOT_DIR_LBA, ISO_SECTOR, 0x02, b"\x00"),
        directory_record(ROOT_DIR_LBA, ISO_SECTOR, 0x02, b"\x01"),
        directory_record(BOOT_CATALOG_LBA, ISO_SECTOR, 0x00, b"BOOT.CAT;1"),
        directory_record(BOOT_IMAGE_LBA, boot_image_bytes, 0x00, b"BOOT.IMG;1"),
    )
    offset = 0
    for record in records:
        root[offset:offset + len(record)] = record
        offset += len(record)
    return root


def boot_catalog(args, boot_sector_count):
    catalog = bytearray(ISO_SECTOR)

    # Validation entry.  The checksum word is chosen last so the little-endian
    # sum of all sixteen words is zero; 55 AA belongs here, not in BOOT.IMG.
    catalog[0] = 1
    catalog[1] = 0xEF if args.negctl_wrong_platform_id else 0
    catalog[4:28] = padded_ascii("LOGITOS EL TORITO", 24)
    catalog[30:32] = b"\x55\xaa"
    words_without_checksum = sum(struct.unpack("<16H", catalog[:32]))
    put_721(catalog, 28, (-words_without_checksum) & 0xFFFF)
    if args.negctl_bad_catalog_checksum:
        catalog[28] ^= 1

    # One initial/default entry only.  A UEFI section would be dead metadata:
    # LogitOS UEFI boots from the separate FAT image written by mkesp.py.
    catalog[32] = 0x88
    catalog[33] = 2 if args.negctl_emulation_floppy else 0
    put_721(catalog, 34, BOOT_LOAD_SEGMENT)
    catalog[36] = 0
    catalog[37] = 0
    put_721(catalog, 38, boot_sector_count)
    put_731(catalog, 40, BOOT_IMAGE_LBA)
    return catalog


def ceil_div(value, divisor):
    return (value + divisor - 1) // divisor


def patch_loader_location(boot_image, loader_lba, loader_blocks, args):
    """Patch the single authoritative L2P! record in a preload image."""
    occurrences = []
    start = 0
    while True:
        offset = boot_image.find(LOADER_PATCH_MAGIC, start)
        if offset < 0:
            break
        occurrences.append(offset)
        start = offset + 1
    if occurrences != [LOADER_PATCH_OFFSET]:
        found = ", ".join(f"0x{offset:x}" for offset in occurrences) or "none"
        die(f"preload patch magic must occur once at 0x{LOADER_PATCH_OFFSET:x}; found {found}")

    patched = bytearray(boot_image)
    patched_lba = loader_lba + (1 if args.negctl_loader_lba_plus_one else 0)
    LOADER_PATCH.pack_into(
        patched, LOADER_PATCH_OFFSET, LOADER_PATCH_MAGIC, patched_lba, loader_blocks
    )
    return bytes(patched)


def build_image(boot_image, loader, args):
    if not boot_image:
        die("boot image is empty")
    boot_sector_count = max(MIN_BOOT_SECTOR_COUNT, ceil_div(len(boot_image), BIOS_SECTOR))
    if boot_sector_count > 0xFFFF:
        die(f"boot image needs {boot_sector_count} catalog sectors; El Torito stores only 16 bits")
    boot_bytes = boot_sector_count * BIOS_SECTOR
    boot_iso_blocks = ceil_div(boot_bytes, ISO_SECTOR)
    loader_lba = BOOT_IMAGE_LBA + boot_iso_blocks
    loader_blocks = 0
    if loader is not None:
        if not loader:
            die("loader image is empty")
        loader_blocks = ceil_div(len(loader), ISO_SECTOR)
        # preload deliberately uses one DAP: splitting transfers belongs with
        # the later kernel-loading stage, not this narrowly measured handoff.
        # 127 is the conservative EDD maximum accepted by legacy BIOSes.
        if loader_blocks > LOADER_LOAD_MAX_BLOCKS:
            die(f"loader needs {loader_blocks} native CD blocks; preload's one-DAP limit is "
                f"{LOADER_LOAD_MAX_BLOCKS}")
        boot_image = patch_loader_location(boot_image, loader_lba, loader_blocks, args)
    elif args.negctl_loader_lba_plus_one:
        die("--negctl-loader-lba-plus-one requires --loader")

    volume_sectors = max(BASE_VOLUME_SECTORS, loader_lba + loader_blocks)
    image = bytearray(volume_sectors * ISO_SECTOR)
    image[PVD_LBA * ISO_SECTOR:(PVD_LBA + 1) * ISO_SECTOR] = primary_volume_descriptor(volume_sectors)
    image[BOOT_RECORD_LBA * ISO_SECTOR:(BOOT_RECORD_LBA + 1) * ISO_SECTOR] = boot_record_descriptor()
    image[TERMINATOR_LBA * ISO_SECTOR:(TERMINATOR_LBA + 1) * ISO_SECTOR] = terminator_descriptor()
    # BOOT.IMG names the bytes the catalog can load, including deterministic
    # zero padding.  Reporting only the source file length would make the ISO
    # directory disagree with the extent that firmware actually consumes.
    image[ROOT_DIR_LBA * ISO_SECTOR:(ROOT_DIR_LBA + 1) * ISO_SECTOR] = root_directory(boot_bytes)
    image[L_PATH_TABLE_LBA * ISO_SECTOR:(L_PATH_TABLE_LBA + 1) * ISO_SECTOR] = path_table(False)
    image[M_PATH_TABLE_LBA * ISO_SECTOR:(M_PATH_TABLE_LBA + 1) * ISO_SECTOR] = path_table(True)
    image[BOOT_CATALOG_LBA * ISO_SECTOR:(BOOT_CATALOG_LBA + 1) * ISO_SECTOR] = boot_catalog(
        args, boot_sector_count
    )
    image[BOOT_IMAGE_LBA * ISO_SECTOR:BOOT_IMAGE_LBA * ISO_SECTOR + len(boot_image)] = boot_image
    if loader is not None:
        start = loader_lba * ISO_SECTOR
        image[start:start + len(loader)] = loader
    return image, boot_sector_count, loader_lba, loader_blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", help="output ISO path")
    parser.add_argument("--boot-image", required=True,
                        help="raw real-mode image loaded at physical 0x7C00")
    parser.add_argument("--loader",
                        help="raw payload placed at a native CD LBA and patched into preload")
    controls = parser.add_mutually_exclusive_group()
    controls.add_argument("--negctl-bad-catalog-checksum", action="store_true",
                          help=argparse.SUPPRESS)
    controls.add_argument("--negctl-wrong-platform-id", action="store_true",
                          help=argparse.SUPPRESS)
    controls.add_argument("--negctl-emulation-floppy", action="store_true",
                          help=argparse.SUPPRESS)
    controls.add_argument("--negctl-loader-lba-plus-one", action="store_true",
                          help=argparse.SUPPRESS)
    args = parser.parse_args()

    if not os.path.isfile(args.boot_image):
        die(f"no such boot image: {args.boot_image}")
    if os.path.abspath(args.out) == os.path.abspath(args.boot_image):
        die("output path and boot-image path must differ")
    with open(args.boot_image, "rb") as source:
        boot_image = source.read()
    loader = None
    if args.loader:
        if not os.path.isfile(args.loader):
            die(f"no such loader image: {args.loader}")
        with open(args.loader, "rb") as source:
            loader = source.read()
    image, boot_sector_count, loader_lba, loader_blocks = build_image(boot_image, loader, args)
    with open(args.out, "wb") as output:
        output.write(image)

    format_description = "no-emulation"
    if args.negctl_bad_catalog_checksum:
        format_description += ", NEGATIVE CONTROL: bad catalog checksum"
    elif args.negctl_wrong_platform_id:
        format_description += ", NEGATIVE CONTROL: UEFI platform id in BIOS-only catalog"
    elif args.negctl_emulation_floppy:
        format_description = "NEGATIVE CONTROL: 1.44 MiB floppy emulation"
    elif args.negctl_loader_lba_plus_one:
        format_description += ", NEGATIVE CONTROL: loader LBA plus one"
    print(f"mkiso: {args.out} ({len(image)} bytes, BIOS El Torito {format_description}) -- "
          f"BOOT.IMG ({len(boot_image)} bytes, padded to {boot_sector_count * BIOS_SECTOR})")
    if loader is not None:
        patched_lba = loader_lba + (1 if args.negctl_loader_lba_plus_one else 0)
        print(f"mkiso: loader {len(loader)} bytes at native-CD LBA {loader_lba}, "
              f"preload patch LBA {patched_lba}, blocks {loader_blocks}")


if __name__ == "__main__":
    main()
