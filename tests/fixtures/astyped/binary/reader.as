# aether: 3.0
# Decode fields explicitly: wire bytes need neither host alignment nor the
# host machine's endianness. Bytes indexing checks every byte access.
struct Header:
    version: u32
    payload_bytes: u32

def read_u32_le(data: Bytes, offset: i64) -> u32:
    first = u32(data[offset])
    second = u32(data[offset + 1]) << 8
    third = u32(data[offset + 2]) << 16
    fourth = u32(data[offset + 3]) << 24
    return first | second | third | fourth

def decode(data: Bytes) -> Header:
    if len(data) < 8:
        raise ValueError("truncated binary header")
    header = Header(read_u32_le(data, 0), read_u32_le(data, 4))
    if header.version != 3:
        raise ValueError("unsupported binary version")
    if i64(header.payload_bytes) != len(data) - 8:
        raise ValueError("binary payload length differs from header")
    return header
