# aether: 3.0
# Write the wire layout explicitly. This file must be readable on a machine
# with a different alignment or byte order, without casting bytes to a struct.
def write_u32_le(data: Buffer, offset: i64, value: u32) -> None:
    for index in range(4):
        shift = u32(index * 8)
        data[offset + index] = i64((value >> shift) & 255)


def main() -> i64:
    arguments = args()
    if len(arguments) != 2:
        print("usage: binary-create FILE")
        return 2
    header_bytes = 8
    payload_bytes = 4096
    data = buffer(header_bytes + payload_bytes)
    write_u32_le(data, 0, 3)
    write_u32_le(data, 4, u32(payload_bytes))
    for index in range(payload_bytes):
        data[header_bytes + index] = index % 256
    written = file_write(arguments[1], Bytes(data))
    print("wrote", written, "bytes")
    return 0
