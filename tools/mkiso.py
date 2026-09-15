#!/usr/bin/env python3
"""Write the deliberately small BIOS-bootable ISO used by LogitOS.

Usage:
    mkiso.py <out.iso> --boot-image <real-mode.bin>

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
time.  In particular, the catalog asks BIOS for four 512-byte sectors (one ISO
sector).  El Torito does not require an MBR signature in that boot image, so
this writer pads a short input but never invents 0x55AA at image offset 510.

The --negctl-* switches exist only so test-mkiso can build corrupt images and
watch both its parser and SeaBIOS reject them.  They are not alternate output
formats and exactly one may be used at a time.
"""

import argparse
import os
import struct
import sys


ISO_SECTOR = 2048
BIOS_SECTOR = 512
VOLUME_SECTORS = 218
PVD_LBA = 16
BOOT_RECORD_LBA = 17
TERMINATOR_LBA = 18
ROOT_DIR_LBA = 19
L_PATH_TABLE_LBA = 20
M_PATH_TABLE_LBA = 21
BOOT_CATALOG_LBA = 49
BOOT_IMAGE_LBA = 217
BOOT_LOAD_SEGMENT = 0x07C0
BOOT_SECTOR_COUNT = 4


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


def primary_volume_descriptor():
    pvd = bytearray(ISO_SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = padded_ascii("LOGITOS", 32)
    pvd[40:72] = padded_ascii("LOGITOS_BOOT", 32)
    put_733(pvd, 80, VOLUME_SECTORS)
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


def root_directory():
    root = bytearray(ISO_SECTOR)
    records = (
        directory_record(ROOT_DIR_LBA, ISO_SECTOR, 0x02, b"\x00"),
        directory_record(ROOT_DIR_LBA, ISO_SECTOR, 0x02, b"\x01"),
        directory_record(BOOT_CATALOG_LBA, ISO_SECTOR, 0x00, b"BOOT.CAT;1"),
        directory_record(BOOT_IMAGE_LBA, ISO_SECTOR, 0x00, b"BOOT.IMG;1"),
    )
    offset = 0
    for record in records:
        root[offset:offset + len(record)] = record
        offset += len(record)
    return root


def boot_catalog(args):
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
    put_721(catalog, 38, BOOT_SECTOR_COUNT)
    put_731(catalog, 40, BOOT_IMAGE_LBA)
    return catalog


def build_image(boot_image, args):
    if not boot_image:
        die("boot image is empty")
    boot_bytes = BOOT_SECTOR_COUNT * BIOS_SECTOR
    if len(boot_image) > boot_bytes:
        die(f"boot image is {len(boot_image)} bytes; the measured four-sector catalog entry "
            f"can load at most {boot_bytes}")

    image = bytearray(VOLUME_SECTORS * ISO_SECTOR)
    image[PVD_LBA * ISO_SECTOR:(PVD_LBA + 1) * ISO_SECTOR] = primary_volume_descriptor()
    image[BOOT_RECORD_LBA * ISO_SECTOR:(BOOT_RECORD_LBA + 1) * ISO_SECTOR] = boot_record_descriptor()
    image[TERMINATOR_LBA * ISO_SECTOR:(TERMINATOR_LBA + 1) * ISO_SECTOR] = terminator_descriptor()
    image[ROOT_DIR_LBA * ISO_SECTOR:(ROOT_DIR_LBA + 1) * ISO_SECTOR] = root_directory()
    image[L_PATH_TABLE_LBA * ISO_SECTOR:(L_PATH_TABLE_LBA + 1) * ISO_SECTOR] = path_table(False)
    image[M_PATH_TABLE_LBA * ISO_SECTOR:(M_PATH_TABLE_LBA + 1) * ISO_SECTOR] = path_table(True)
    image[BOOT_CATALOG_LBA * ISO_SECTOR:(BOOT_CATALOG_LBA + 1) * ISO_SECTOR] = boot_catalog(args)
    image[BOOT_IMAGE_LBA * ISO_SECTOR:BOOT_IMAGE_LBA * ISO_SECTOR + len(boot_image)] = boot_image
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", help="output ISO path")
    parser.add_argument("--boot-image", required=True,
                        help="raw real-mode image loaded at physical 0x7C00")
    controls = parser.add_mutually_exclusive_group()
    controls.add_argument("--negctl-bad-catalog-checksum", action="store_true",
                          help=argparse.SUPPRESS)
    controls.add_argument("--negctl-wrong-platform-id", action="store_true",
                          help=argparse.SUPPRESS)
    controls.add_argument("--negctl-emulation-floppy", action="store_true",
                          help=argparse.SUPPRESS)
    args = parser.parse_args()

    if not os.path.isfile(args.boot_image):
        die(f"no such boot image: {args.boot_image}")
    if os.path.abspath(args.out) == os.path.abspath(args.boot_image):
        die("output path and boot-image path must differ")
    with open(args.boot_image, "rb") as source:
        boot_image = source.read()
    image = build_image(boot_image, args)
    with open(args.out, "wb") as output:
        output.write(image)

    format_description = "no-emulation"
    if args.negctl_bad_catalog_checksum:
        format_description += ", NEGATIVE CONTROL: bad catalog checksum"
    elif args.negctl_wrong_platform_id:
        format_description += ", NEGATIVE CONTROL: UEFI platform id in BIOS-only catalog"
    elif args.negctl_emulation_floppy:
        format_description = "NEGATIVE CONTROL: 1.44 MiB floppy emulation"
    print(f"mkiso: {args.out} ({len(image)} bytes, BIOS El Torito {format_description}) -- "
          f"BOOT.IMG ({len(boot_image)} bytes, padded to {BOOT_SECTOR_COUNT * BIOS_SECTOR})")


if __name__ == "__main__":
    main()
