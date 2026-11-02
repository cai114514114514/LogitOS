# logitparserutils: buffers, encodings and input streams

logitparserutils provides byte buffers, containers, character conversion and chunked input for LibCSS. The implementation lives in `c/lib/logitparserutils/` and preserves the `parserutils_*` source interfaces used by its callers. Its storage designs address three parser requirements: retaining state pointers during container growth, carrying incomplete characters across input chunks, and keeping borrowed character bytes valid while the lexer consumes them.

## Place in the parser pipeline

The browser’s `make_sheet()` in `c/apps/gui/browser/css/engine.c` creates a stylesheet with UTF-8 as its starting charset, appends the stylesheet bytes and signals completion. LibCSS creates the input stream, lexer, state stacks and token vector in `third_party/css/libcss/src/parse/parse.c`.

| Consumer | Use of logitparserutils |
|---|---|
| CSS lexer | Reads decoded characters and builds unescaped token text in a byte buffer. |
| CSS grammar parser | Stores parser states and open brackets in stacks, and tokens in a vector. |
| Property parsers | Traverse token vectors and assemble strings and calculation instructions in buffers. |
| Media query parser | Uses vector peeks and iteration to inspect tokens. |
| Font descriptor parser | Traverses vectors while parsing `@font-face` descriptors. |
| CSS charset detector | Resolves encoding labels to identifiers. |

The corresponding consumers are in `third_party/css/libcss/src/lex/lex.c`, `third_party/css/libcss/src/parse/parse.c`, `third_party/css/libcss/src/parse/properties/utils.c`, `third_party/css/libcss/src/parse/mq.c`, `third_party/css/libcss/src/parse/font_face.c` and `third_party/css/libcss/src/charset/detect.c`.

Other parsing paths use their own storage. The HTML tokenizer accumulates names and attributes in growable buffers and can return text slices borrowed from its input; see `c/apps/gui/browser/html/tokenizer.c` and `c/apps/gui/browser/html/tokenizer.h`. The CSS value canonicalizer uses a local scanner for declaration values in `third_party/css/libcss/src/parse/canon.c`.

The `Makefile` includes the library’s C files in `CSS_SRC`. That source group supplies the browser’s CSS objects and the host CSS archive.

## Byte buffers

`parserutils_buffer`, declared in `c/lib/logitparserutils/include/parserutils/utils/buffer.h`, exposes four fields:

| Field | Meaning |
|---|---|
| `alloc` | Allocation base used when freeing storage. |
| `data` | Start of the stored bytes. |
| `length` | Number of stored bytes. |
| `allocated` | Capacity available from `data`. |

The distinction between `alloc` and `data` supports `parserutils_buffer_randomise()`, which copies the content to an offset within a new allocation. This changes both its address and alignment.

`c/lib/logitparserutils/buffer.c` implements append, insertion, discard and growth. Insertion checks size arithmetic, reserves capacity and updates the length after copying the bytes. When the source is a slice of the same buffer, `parserutils_buffer_insert()` saves it before growth or movement of the destination’s tail.

`parserutils_buffer_appendv()` takes pairs of pointers and lengths, gathers them into temporary storage and performs one append. The destination length changes only after a successful allocation. LibCSS uses this operation to append calculation instructions composed of an opcode and its operands in `third_party/css/libcss/src/parse/properties/utils.c`.

## Vectors and stacks

`parserutils_vector` and `parserutils_stack` share paged storage implemented in `c/lib/logitparserutils/sequence.c`. Creation specifies the element size and the number of elements per page. Appending or pushing copies an element into a page.

Growth reallocates the index of pages while retaining existing element pages, so pointers to retained elements stay stable. The source comment identifies parser state pointers held across a push as the reason for this design. Removal reduces the element count; later additions reuse retained slots. Destruction releases the pages.

| Container | Access |
|---|---|
| Vector | Indexed peek, iteration, length query, clear and removal of the last element. |
| Stack | Push, pop and access to the current top element. |

Vector iteration uses an `int32_t` cursor and increments it after finding an element. Invalid positions return `NULL`; looking up the current element of an empty stack also returns `NULL`. Removing from an empty container returns `PARSERUTILS_INVALID`. These operations are implemented in `c/lib/logitparserutils/vector.c` and `c/lib/logitparserutils/stack.c`.

## Encoding names and conversion support

`parserutils_charset_mibenum_from_name()` resolves a label to a numeric identifier. `parserutils_charset_mibenum_to_name()` performs the reverse lookup. Unknown labels return zero; unknown identifiers return `NULL`.

The lookup in `c/lib/logitparserutils/names.c` folds ASCII case and skips nonalphanumeric ASCII bytes except NUL. It searches registered names and aliases, then historical compatibility names. Most identifiers come from IANA MIBenum assignments; the compatibility table records additional identifiers retained from the earlier interface.

`c/lib/logitparserutils/registry.c` contains generated IANA registry data, with its source URL and input SHA-256 recorded in the header. `tools/gen_charset_registry.py` reads a supplied CSV snapshot, validates identifiers and alias formatting, and emits records sorted by identifier. Reverse lookup uses binary search over those records.

Codec creation resolves the name and checks availability through `text_supported()` in `c/lib/logitparserutils/charset.c`. Conversion covers ASCII, Latin-1, UTF-8, UTF-16 and UTF-32 variants, plus selected encodings backed by byte maps.

The conversion code shares mapping data from `c/apps/gui/browser/js/text/encoding_tables.inc` and applies its own repertoire rules. For example, the ISO graphic sets reject C1 control bytes. This allows reuse of the mapping data while preserving the parser converter’s treatment of those bytes.

## Stateful codecs

`parserutils_charset_codec_decode()` converts external bytes into UCS-4 bytes in big endian order. `parserutils_charset_codec_encode()` converts those UCS-4 bytes to the selected external encoding. Both update the caller’s input pointer, remaining input length, output pointer and available output capacity.

Each codec has an encoder lane and a decoder lane. Each lane holds an unfinished input character and a completed output character waiting for space. Their layout is defined in `c/lib/logitparserutils/codec_state.h`.

| Condition | Behavior |
|---|---|
| A character spans calls | The codec saves the prefix, consumes those bytes and returns `PARSERUTILS_OK`. |
| Output space is insufficient | The codec retains the completed character and returns `PARSERUTILS_NOMEM`. |
| Input is exhausted while output remains pending | A call with zero input length drains the saved output when enough space is available. |
| Conversion state needs restarting | `parserutils_charset_codec_reset()` clears both lanes and retains the error mode. |

After a capacity error, retry from the updated input position: the codec may already have consumed the character whose output is pending.

The default mode is `PARSERUTILS_CHARSET_CODEC_ERROR_LOOSE`. Decoding replaces malformed input with U+FFFD. Encoding replaces invalid or unrepresentable values with U+FFFD for Unicode output and `?` for a single byte encoding. Strict mode returns `PARSERUTILS_INVALID` before consuming the offending unit from the current call’s input.

The contracts are declared in `c/lib/logitparserutils/include/parserutils/charset/codec.h` and implemented in `c/lib/logitparserutils/codec.c`, `c/lib/logitparserutils/codec_input.c` and `c/lib/logitparserutils/codec_output.c`.

## Unicode utilities

The utilities in `c/lib/logitparserutils/utf8.c` and `c/lib/logitparserutils/utf16.c` convert individual Unicode scalars, count scalars and find adjacent character boundaries. Input sizes and navigation offsets use bytes; the length functions return character counts.

UTF-8 decoding checks continuation bytes, overlong forms, surrogate values and the Unicode upper bound. UTF-16 decoding checks surrogate pairs through the shared scalar reader in `c/lib/logitparserutils/charset.c`. Incomplete input returns `PARSERUTILS_NEEDDATA`, allowing the caller to supply the remaining bytes.

The scalar writers use different output conventions:

| Writer | Output convention |
|---|---|
| `parserutils_charset_utf8_from_ucs4()` | Advances the output pointer and reduces remaining capacity. |
| `parserutils_charset_utf16_from_ucs4()` | Writes to the supplied buffer and replaces the capacity value with bytes written. |

UTF-16 utilities use host byte order and support unaligned input. The standalone codec’s unsuffixed UTF-16 label also selects host order; UTF-16LE and UTF-16BE select their named order.

## Chunked input streams

`parserutils_inputstream` owns pending source bytes and a window of decoded UTF-8. Consumers access it through the opaque interface in `c/lib/logitparserutils/include/parserutils/input/inputstream.h`.

A consumer follows this sequence:

1. Create a stream with a starting charset, an origin value and an optional detection callback.
2. Append source chunks with `parserutils_inputstream_append()`.
3. Peek at characters using byte offsets relative to the decoded cursor.
4. Consume decoded bytes with `parserutils_inputstream_advance()`.
5. Close the source by appending `NULL` with length zero, then consume the remaining decoded data.

A non-null pointer with zero length represents an empty chunk.

`c/lib/logitparserutils/stream.c` performs decoding through the shared scalar reader in `c/lib/logitparserutils/charset.c`. The stream owns its pending source queue and closure state, while the standalone codec owns conversion state for each character.

For nonempty source data, initial encoding selection waits for four bytes or closure. Four bytes distinguish the UTF-16LE and UTF-32LE byte order marks. Selection calls the detector, checks conversion support and consumes a byte order mark matching the selected encoding. LibCSS supplies `css__charset_extract()`, which examines byte order marks and `@charset` declarations while tracking the origin of the charset choice.

A detector’s request for more data keeps an open stream pending. At closure, selection proceeds with the available choice. If the detector proposes an unsupported encoding, the stream retains the caller’s starting encoding and origin. Unsuffixed UTF-16 and UTF-32 use big endian order in the stream until a matching byte order mark selects an order.

Decoding stores complete UTF-8 output before advancing the source offset. An unfinished source character waits for another chunk. At closure, a trailing incomplete sequence becomes U+FFFD; malformed sequences also produce replacement characters.

Valid U+0000 passes through. A comment in `c/lib/logitparserutils/stream.c` records the earlier correction: the first implementation replaced that scalar; the current stream passes it through and assigns language preprocessing to the consumer.

`parserutils_inputstream_insert()` validates UTF-8 and inserts it at the decoded cursor, regardless of the source encoding. A successful peek returns a borrowed pointer and the byte length of one UTF-8 character. `parserutils_inputstream_peek_slow()` delegates to the same implementation. `parserutils_inputstream_read_charset()` returns the current encoding name and origin.

## Borrowed character lifetimes

In the public stream header, a peek pointer stays valid until advancement passes its character or the stream is destroyed. Refills, insertion and compaction preserve this lifetime.

`c/lib/logitparserutils/window.c` retains replaced allocations as snapshots while their unread content remains in use. Insertion creates a replacement even when capacity would suffice, because shifting bytes would change data visible through borrowed pointers. The inserted bytes delay retirement of existing snapshots.

Advancement reduces each snapshot’s remaining consumption distance and releases snapshots when that distance reaches zero. Compaction becomes eligible after the consumed prefix reaches 65,536 bytes and occurs when a peek needs to extend the decoded window.

The file keeps the earlier design with a resizable buffer beside its correction: growth freed borrowed pointers, and compaction moved their bytes. Retaining replaced allocations addresses those lifetime failures.

## Error handling

`parserutils_error` is declared in `c/lib/logitparserutils/include/parserutils/errors.h`.

| Code | Meaning |
|---|---|
| `PARSERUTILS_OK` | Operation succeeded. |
| `PARSERUTILS_NOMEM` | Allocation, growth limits or output capacity prevented completion. |
| `PARSERUTILS_BADPARM` | An argument failed validation. |
| `PARSERUTILS_INVALID` | Invalid input or operation state. |
| `PARSERUTILS_FILENOTFOUND` | File lookup error value retained in the shared interface. |
| `PARSERUTILS_NEEDDATA` | More input bytes are required. |
| `PARSERUTILS_BADENCODING` | The requested encoding lacks conversion support. |
| `PARSERUTILS_EOF` | The requested stream position is past available data after closure. |

Stream consumers can append more input after `PARSERUTILS_NEEDDATA` and retry. LibCSS’s completion path closes the stream and continues parsing its remaining data in `third_party/css/libcss/src/parse/parse.c`.

`parserutils_error_to_string()` returns descriptions. `parserutils_error_from_string()` accepts exact, case-sensitive short names such as `"NOMEM"` or descriptions such as `"Insufficient memory"`. An unknown string returns `PARSERUTILS_BADPARM`. Both functions are implemented in `c/lib/logitparserutils/errors.c`.

LibCSS’s `css_error_from_parserutils_error()` casts the value to `css_error` in `third_party/css/libcss/src/utils/parserutilserror.h`. The matching numeric assignments in `third_party/css/libcss/include/libcss/errors.h` are part of the integration contract. In particular, `PARSERUTILS_BADENCODING` maps to `CSS_BADCHARSET`.

## Limitations

- Charset recognition covers more encodings than conversion. For example, creating a Shift_JIS codec returns `PARSERUTILS_BADENCODING`; availability is defined by `text_supported()`.
- `PARSERUTILS_CHARSET_CODEC_ERROR_TRANSLIT` uses the loose replacement fallback. The codec header documents that linguistic transliteration stays out of scope.
- Zero-length codec input drains completed output without signalling closure. An unfinished source character remains pending until more bytes arrive or reset clears it.
- `parserutils_inputstream_change_charset()` accepts changes before encoding selection and returns `PARSERUTILS_INVALID` afterward.
- Compatibility covers source interfaces. `c/lib/logitparserutils/README.md` excludes the earlier binary layouts from that contract.

## Source map

| File | What it contains |
|---|---|
| `c/lib/logitparserutils/README.md` | Scope, interface history and compatibility notes. |
| `c/lib/logitparserutils/include/parserutils/utils/buffer.h` | Buffer layout and public operations. |
| `c/lib/logitparserutils/buffer.c` | Byte storage, alias handling and grouped appends. |
| `c/lib/logitparserutils/sequence.c` | Paged storage shared by vectors and stacks. |
| `c/lib/logitparserutils/vector.c` | Indexed access, iteration and vector operations. |
| `c/lib/logitparserutils/stack.c` | Stack operations and access to the top element. |
| `c/lib/logitparserutils/names.c` | Label matching, compatibility names and identifier lookup. |
| `c/lib/logitparserutils/registry.c` | Generated IANA registry data and provenance. |
| `tools/gen_charset_registry.py` | Registry generation from a supplied CSV snapshot. |
| `c/apps/gui/browser/js/text/encoding_tables.inc` | Shared character mapping data. |
| `c/lib/logitparserutils/charset.c` | Conversion availability and scalar readers and writers. |
| `c/lib/logitparserutils/include/parserutils/charset/codec.h` | Stateful conversion contract and error modes. |
| `c/lib/logitparserutils/codec.c` | Codec creation, options, reset and output draining. |
| `c/lib/logitparserutils/codec_state.h` | Encoder and decoder state. |
| `c/lib/logitparserutils/codec_input.c` | Conversion from external bytes to UCS-4. |
| `c/lib/logitparserutils/codec_output.c` | Conversion from UCS-4 to external bytes. |
| `c/lib/logitparserutils/utf8.c` | UTF-8 scalar, counting and navigation utilities. |
| `c/lib/logitparserutils/utf16.c` | UTF-16 scalar, counting and navigation utilities. |
| `c/lib/logitparserutils/include/parserutils/input/inputstream.h` | Stream interface and borrowed-pointer contract. |
| `c/lib/logitparserutils/stream.c` | Source queue, charset selection and stream decoding. |
| `c/lib/logitparserutils/window.c` | Decoded storage and snapshot retention. |
| `c/lib/logitparserutils/include/parserutils/errors.h` | Error values and conversion declarations. |
| `c/lib/logitparserutils/errors.c` | Error descriptions and string lookup. |
| `third_party/css/libcss/src/parse/parse.c` | CSS parser ownership, input delivery and completion. |
| `third_party/css/libcss/src/lex/lex.c` | Character consumption and unescaped token buffers. |
| `third_party/css/libcss/src/charset/detect.c` | CSS charset detection. |
| `third_party/css/libcss/src/parse/properties/utils.c` | Property token traversal and calculation buffers. |
| `third_party/css/libcss/src/parse/mq.c` | Media query token consumers. |
| `third_party/css/libcss/src/parse/font_face.c` | Font descriptor token consumers. |
| `third_party/css/libcss/src/utils/parserutilserror.h` | Conversion to LibCSS error values. |
| `third_party/css/libcss/include/libcss/errors.h` | Numeric LibCSS error assignments. |
| `third_party/css/libcss/src/parse/canon.c` | Local declaration-value scanner. |
| `c/apps/gui/browser/html/tokenizer.c` | HTML tokenizer and its character buffers. |
| `c/apps/gui/browser/html/tokenizer.h` | HTML token storage and input lifetime contracts. |
| `c/apps/gui/browser/css/engine.c` | Browser stylesheet creation and completion handling. |
| `Makefile` | CSS include paths, source groups and compilation rules. |

## See also

- [Libraries](overview.md)
- [CSS: parsing, cascade and computed style](../browser/css.md)
- [HTML parsing](../browser/html-parsing.md)
