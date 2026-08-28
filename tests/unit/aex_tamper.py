#!/usr/bin/env python3
"""aex_tamper.py <signed.aex> <out.aex>

Flip one byte of a SIGNED .aex's embedded ELF payload and repair its CRC-32
record so the file still passes aex.c's MANDATORY integrity check -- and
leaves its Ed25519 signature (over the ORIGINAL bytes) untouched.

WHY THIS AND NOT JUST FLIPPING A BYTE. CRC-32 has no secret in it: an attacker
who can write the file can also recompute a CRC that matches whatever they
wrote, which is exactly why aex.c's own comment says a CRC is not a signature.
So the control that actually demonstrates what the signature adds -- as
opposed to what the already-mandatory CRC already catches -- has to be a file
where the CRC is CORRECT for the tampered bytes and only the signature is not:
a mere bit-flip would be refused at the CRC gate before the signature check
ever ran, and that failure would prove nothing about the new mechanism.

Relies on the CRC record being the FIRST TLV record at a fixed layout
(tools/mkaex.py's build() always emits it first, at file offset 64): tag(4)
len(4) value(4) at [64,76). Asserts that shape rather than assuming it.
"""
import struct
import sys
import zlib

T_CRC32 = 0x43524341  # "ACRC"


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: aex_tamper.py <signed.aex> <out.aex>")
    data = bytearray(open(sys.argv[1], "rb").read())

    tag, ln = struct.unpack_from("<II", data, 64)
    if tag != T_CRC32 or ln != 4:
        sys.exit("aex_tamper: expected the CRC record first at offset 64 "
                 "(tag=0x%08x len=%d) -- mkaex.py's layout changed" % (tag, ln))

    hdr_size = struct.unpack_from("<H", data, 52)[0]
    if hdr_size >= len(data):
        sys.exit("aex_tamper: hdr_size >= file size, nothing to tamper")

    # Flip one byte roughly in the middle of the embedded ELF image -- deep
    # enough inside the payload that this is not touching a header field.
    i = hdr_size + (len(data) - hdr_size) // 2
    data[i] ^= 0xFF

    new_crc = zlib.crc32(bytes(data[hdr_size:])) & 0xFFFFFFFF
    struct.pack_into("<I", data, 72, new_crc)

    open(sys.argv[2], "wb").write(data)
    print("aex_tamper: %s -> %s  flipped byte %d, repaired CRC to 0x%08x "
          "(signature left over the ORIGINAL bytes -- must now fail to verify)"
          % (sys.argv[1], sys.argv[2], i, new_crc))


if __name__ == "__main__":
    main()
