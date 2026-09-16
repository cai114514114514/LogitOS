# aether: 3.0
import reader

def main() -> i64:
    # This used to decode a fixed in-memory array. The program now accepts an
    # actual file, and the imported reader verifies its complete wire format.
    arguments = args()
    if len(arguments) != 2:
        print("usage: binary FILE")
        return 2
    data = file_read(arguments[1])
    header = reader.decode(data)
    print("header", header.version, header.payload_bytes)
    return 0
