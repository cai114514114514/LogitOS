# A3 binary file project

`create.as` writes a sample file; `main.as` loads it through native file I/O,
then calls `reader.as` to validate the little-endian header and payload length.
The eight-byte header contains a u32 version followed by a u32 payload size.
Version 3 is the file format version here; this does not select language semantics.

From the repository root, build and run on the host:

```sh
make BUILD=build/a3-cutover build/a3-cutover/asc
build/a3-cutover/asc build tests/fixtures/astyped/binary/create.as -o /tmp/aether-create
build/a3-cutover/asc build tests/fixtures/astyped/binary/main.as -o /tmp/aether-read
/tmp/aether-create /tmp/aether-sample.bin
/tmp/aether-read /tmp/aether-sample.bin
```

Expected output:

```text
wrote 4104 bytes
header 3 4096
```

Missing input, short headers, unsupported versions and mismatched payload sizes
report typed exceptions with source locations. The reader treats payload bytes
as binary data. Text files can instead be read with `file_read(path).decode()`,
which strictly validates UTF-8 and raises ConversionError on invalid input.

`test-as-binary` checks host execution against an independent binary encoder.
`test-as-typed-guest` also runs creation followed by reading on the guest disk,
alongside valid and malformed supplied files, in debug and release builds.
