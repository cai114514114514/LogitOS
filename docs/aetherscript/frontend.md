# AetherScript lexer and parser

The AetherScript frontend turns source text into a syntax tree used by semantic checking, editor queries, and native code generation. A batch lexer converts indentation and logical lines into tokens; a recursive descent parser builds declarations, expressions, and statements. The implementation lives in `c/apps/as/frontend/`, with the shared project and tree records in `c/apps/as/ir/model.h`.

## Frontend flow

`as_typed_check_with_library()` in `c/apps/as/frontend/parser.c` allocates an `AsTypedProject`, initializes builtin types and exception types, and loads the entry module. Each module passes through these steps:

1. Copy source bytes from a file or an editor overlay into module storage.
2. Classify the language version.
3. Tokenize through the lexer’s recovery interface.
4. Parse declarations, imports, and executable statements.
5. Append any module initializer to the project’s initialization order.

Imports recursively enter the same loader. Modules are identified by their path strings. A module’s traversal state distinguishes an active import from a completed load, supporting reuse and cycle detection. Cycle records retain the ordered module chain for related diagnostic locations.

After loading, a project with zero diagnostics and no allocation failure enters `at_check_functions()`. The checker processes module initializers in the recorded dependency order. Source loading and checking construct the program representation; the command driver later selects execution or code generation. These interfaces are declared in `c/apps/as/include/project.h`.

## Source versions

`as_source_version()` in `c/apps/as/common/version.c` classifies a header before tokenization. The version header subsequently passes through the lexer as a comment.

| Header or input | Classifier result |
|---|---|
| `# aether: 1` | `AS_LANGUAGE_RETIRED`, value `-1` |
| `# aether: 2` | `AS_LANGUAGE_VM`, value `2` |
| `# aether: 3` or `# aether: 3.0` | `AS_LANGUAGE_NATIVE`, value `3` |
| Source without a recognized header | `AS_LANGUAGE_VM` |
| An `aether:` declaration with an unsupported value or trailing suffix | `0` |

The constants are defined in `c/apps/as/common/numeric.h`. The reader accepts whitespace after `#` and after `aether:`, including the compact spelling `#aether: 3`. After the version, it accepts whitespace followed by a newline or the end of the source.

Header placement is positional. The reader starts at the first source byte. When that byte begins `#!`, it skips the first line and examines the immediately following line. Otherwise, it examines the first line. A blank line after the shebang selects the default classification because that blank line is the line examined.

`tests/unit/as_version_shebang_test.c` defines cases for a header on the first line, a header immediately after a shebang, CRLF input, and a blank line between the shebang and header.

## Tokens and source ownership

`TokType` and `Token` are defined in `c/apps/as/frontend/lexer.h`. A token carries its kind, a source pointer, a byte length, and a line number. String spans cover their interiors, while layout tokens have zero length.

| Token group | Contents |
|---|---|
| Layout | `T_NEWLINE`, `T_INDENT`, `T_DEDENT` |
| Literals and names | `T_INT`, `T_FLOAT`, `T_STR`, `T_FSTR`, `T_IDENT` |
| Keywords | Function, class, control flow, import, exception, boolean, and resource keywords |
| Operators and punctuation | Arithmetic, comparison, bitwise, assignment, delimiters, member access, pipelines, and redirection |
| Recovery and termination | `T_ERROR`, `T_EOF` |

The lexer builds a token array for the whole source, and the parser walks it by index. The design comment in `c/apps/as/frontend/lexer.h` connects this choice to indentation: processing logical lines together simplifies maintenance of the indentation stack.

Each `AtModule` owns its source buffer and token array. Syntax nodes borrow token spans into that source. Editor overlays are copied during project construction, so editor buffers can change after checking returns. `as_typed_free()` releases module storage and the project’s node registry. These ownership rules appear in `c/apps/as/ir/model.h` and `c/apps/as/frontend/parser.c`.

## Lexical rules

`c/apps/as/frontend/lexer.c` defines local character predicates. Identifiers begin with an ASCII letter or underscore and continue with those characters or decimal digits. Keyword recognition compares complete spellings.

Several words acquire meaning in parser context while retaining `T_IDENT`: `struct`, `assert`, `pass`, `global`, `unsafe`, `as`, and `is`. The expression parser recognizes `None`, `True`, and `False` alongside the reserved spellings `nil`, `true`, and `false`.

The numeric scanner recognizes decimal integers, hexadecimal integers introduced by `0x` or `0X`, and floating forms with a decimal point or exponent. A floating token can begin with a decimal point followed by a digit. Signs become operator tokens. Tokens retain the numeric spelling for later validation.

Single and double quotes delimit strings. The scanner tracks embedded newlines and skips escaped bytes while locating the closing quote. `as_token_decode()` in `c/apps/as/frontend/text.c` decodes `\n`, `\r`, `\t`, and `\0`. Other escapes contribute the byte following the backslash.

Operator scanning selects the longest recognized spelling. For example, `**=`, `<<=`, and `>>=` each form one token; `>> =` produces a shift token followed by assignment. The lexer recognizes `|>`, `->`, and `<-`. Because `<-` forms one token, a comparison with a negative operand uses spacing such as `x < -1`. This consequence is documented beside `T_LARROW` in `c/apps/as/frontend/lexer.h`.

## Indentation and logical lines

Outside continued expressions, the lexer compares each nonblank, noncomment line’s indentation with a stack of column counts. Spaces advance one column; tabs advance to the next multiple of eight.

An increase emits `T_INDENT`. A decrease emits `T_DEDENT` tokens until the stack reaches the new column. A column absent from the remaining stack produces an inconsistent indentation diagnostic. Blank lines and comment lines are skipped during layout processing.

Opening parentheses, brackets, and braces increment a shared continuation counter. Closing delimiters reduce it. While the counter is positive, physical newlines continue the current logical line.

At the end of the source, the lexer appends a `T_NEWLINE` when tokens exist and the last token is another kind. It then emits remaining dedents and `T_EOF`. The resulting block shape is colon, newline, indent, statements, dedent. Column handling belongs to `c/apps/as/frontend/lexer.c`; suite parsing belongs to `c/apps/as/frontend/parser.c`.

## Modules and declarations

Module parsing in `c/apps/as/frontend/parser.c` dispatches according to the current token and, for identifier-led forms, a small amount of lookahead.

| Construct | Representation or behavior |
|---|---|
| Imports | Dotted module paths, module aliases, and named imports with aliases |
| Functions | Name, optional generic parameters, parameters, optional result annotation, and an indented body |
| Structures | Typed fields and optional generic parameters |
| Classes | Typed fields, methods, and an optional single class base |
| Layout declarations | A module binding calling `layout`, validated as external record metadata |
| Module variables | Global records plus assignments in a generated module initializer |
| Script statements | Statements collected into a generated `main()` |

An identifier followed by `=`, `:`, or `,` selects module variable parsing, except for contextual statement words such as `assert` and `unsafe`. Executable statements beginning with identifiers or selected control keywords enter script parsing. An indented suite at module scope also enters the generated `main()`.

Module initializers preserve their statements in source order. They are recorded after imported modules finish loading. The parser diagnoses a conflict between a generated `main()` and a source declaration with that name.

Import resolution is implemented in `c/apps/as/frontend/import.c`. Dotted components become a path ending in `.as`. Bare imports search beside the importing source, then under the configured library root when the local source is absent. Editor overlays participate in local lookup. A path beginning with `std.` selects the library root directly.

`at_parse_type()` parses annotations while declarations are being read. It resolves function and structure type parameters, builtin aliases, declared types, and imported named types. Compound forms include arrays, slices, mutable slices, lists, dictionaries, optionals, pointers, and callable signatures.

Generic parameters use square brackets and can carry protocol constraints. Their type-table entries become available within the declaration’s annotations. Generic structure applications recursively parse their type arguments.

Public and generic function signatures carry parameter and result annotations. Nongeneric private functions and nested functions can use inference. Methods receive their class type for the first `self` parameter. Structure and class records retain field declaration tokens, including tokens copied from a base class.

`c/apps/as/frontend/layout.c` recognizes a binding whose value calls `layout`. It validates the external name, byte size, and literal field descriptions containing a name, offset, width, and kind, then records that metadata in an `AtType`.

## Statements and suites

Statement parsing covers assignments, annotated bindings, expression statements, nested function definitions, returns, conditionals, loops, loop control, assertions, exceptions, resource scopes, global declarations, `pass`, and unsafe blocks.

Suites begin with a colon and continue through a newline and an indented body. `if` and `elif` become conditional nodes with body and alternative links. A `for` statement records one binding name, an iterable expression, and a body.

A `try` statement collects `except` handlers and an optional `else` suite. A handler can name an exception type and an `as` binding. A `with` statement binds one owner name, or two comma-separated names, to an acquisition expression.

`c/apps/as/frontend/assignment.c` groups multiple assignment into an `AN_UNPACK` node containing assignment children. A single right side occupies the parent’s sequence slot; comma-separated right sides attach to corresponding children. This grouping preserves the operation for checking and lowering. `c/apps/as/sema/assignment.c` reads all right sides before updating definite-assignment facts.

The assertion parser preserves the library call form `assert(condition, message)`. It recognizes a call when it finds a comma at the first parenthesis depth. Commas within nested calls and containers remain part of the asserted expression.

## Expressions and precedence

`c/apps/as/frontend/expression.c` combines recursive descent for atoms and postfix operations with precedence climbing for binary operators.

Atoms include literals, names, `super`, parenthesized expressions, array literals, dictionary literals, lambdas, and formatted strings. Postfix parsing repeatedly attaches calls, indexing, and member access. Subscripts on the names `cast` and `is_type` parse a type argument and produce `AN_TYPE_APPLICATION`.

A bracketed first element followed by `for` becomes a comprehension. The parser records one binding, an iterable, and an optional `if` filter.

Binary precedence runs from weakest to strongest:

| Level | Operators |
|---|---|
| 1 | `or`, `\|>`, `->`, `<-` |
| 2 | `and` |
| 3 | `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `not in`, `is`, `is not` |
| 4 | `\|` |
| 5 | `^` |
| 6 | `&` |
| 7 | `<<`, `>>` |
| 8 | `+`, `-` |
| 9 | `*`, `/`, `%` |
| 11 | `**` |

Binary operators associate to the left, except for power, which associates to the right. Unary `+`, `-`, and `~` parse their operands at level 10, so exponentiation binds inside those unary operations. Unary `not` includes comparisons in its operand.

Conditional expressions attach after binary parsing and record the condition, true value, and false value. The parser represents `not in` as membership wrapped in unary negation. It translates `is` and `is not` into equality and inequality nodes.

Nested definitions and lambdas create function records referenced by `AN_CLOSURE` nodes. `c/apps/as/frontend/closure.c` turns a lambda’s expression body into a return statement.

## Syntax tree representation

The frontend builds the representation defined in `c/apps/as/ir/model.h`.

| Record | Role |
|---|---|
| `AsTypedProject` | Owns modules, functions, types, globals, diagnostics, and node storage |
| `AtModule` | Owns source and tokens; records imports and its initializer |
| `AtFunction` | Records signature, locals, lexical parent, class owner, and body |
| `AtType` | Records scalar, compound, generic, class, structure, and layout information |
| `AtNode` | Represents expressions and statements with source and resolution metadata |

An `AtNode` records its kind, token, module, owning function, operator, and stable node identifier. Semantic checking fills fields such as resolved type, symbol, and field index.

Child meanings depend on node kind. Binary expressions use `a` and `b` for operands. Conditional statements use `a` for the condition and `b` and `c` for branches. Calls store the callee in `a` and arguments in `args`. Statement and handler sequences use `next`.

`at_parse_node()` in `c/apps/as/frontend/support.c` registers each allocation immediately. The project registry retains ownership when recovery abandons a partial tree. Fixed project tables keep module, function, and type addresses stable during recursive loading.

## Formatted strings

The lexer emits the interior of an `f` or `F` string as one `T_FSTR`. Its scanner tracks nested delimiters and quoted strings inside interpolation holes.

`c/apps/as/frontend/text.c` divides the token into literal segments and expression holes. Literal segments become string nodes, holes become `AN_FORMAT` nodes, and binary addition joins the pieces. Doubled braces decode to literal braces.

`at_parse_fragment()` in `c/apps/as/frontend/expression.c` reuses the expression parser for each hole. It temporarily surrounds the hole with parentheses to enable continuation across lines. It then rebases token spans into the owning module’s source before freeing the temporary scanner buffer. Retained tree locations refer to the original source snapshot.

## Error recovery and diagnostics

The lexer exposes two interfaces in `c/apps/as/frontend/lexer.h`. `as_lex_source()` returns a lexical failure and frees its partial token array. With a reporting callback, `as_lex_recover()` reports a damaged fragment, emits `T_ERROR`, resets continuation depth, and resumes at the next physical line. For an unterminated string that scanned to the end of the file, recovery rewinds to the line containing the failed token before resuming.

The module loader uses recovery mode and records lexical errors as `AS1001`. Expected-token checks consume `T_ERROR` markers using the lexer’s existing diagnostic. In expression position, a marker becomes `AN_ERROR`.

Statement recovery advances to a newline, dedent, or end of source. It consumes a newline and preserves the dedent for the enclosing suite parser. This keeps the following declaration in its enclosing scope. Declaration recovery can skip the indented suite belonging to a malformed signature.

When an expression is missing an operand at a newline or dedent, expression parsing returns that consumed boundary to statement parsing. Parsing loops also check cursor progress so recovery continues through the token stream.

`at_error()` selects the diagnostic state belonging to the token’s module. `c/apps/as/common/diagnostic.c` records byte ranges, expands range endpoints to UTF-8 character boundaries, and computes columns by counting noncontinuation bytes.

Recovery gives editor queries access to declarations around damaged text. The command driver in `c/apps/as/cli/commands.c` reports project errors and returns before build or execution proceeds.

The assertions in `tests/unit/as_lex_recovery_test.c` cover token parity between lexer modes, preservation of later declarations, compound operators, and allocation failures. `tests/unit/as_recovery_test.py` covers recovered symbols, UTF-8 ranges, diagnostic truncation, and rejection of damaged programs by command paths. These describe test coverage; no tests were run for this review.

## Limitations

- The native module loader accepts version 3 and 3.0. Version 1 is classified as retired; the version 2 engine has been removed. Unversioned source retains the version 2 classification. See `c/apps/as/common/version.c`, `c/apps/as/frontend/parser.c`, and `c/apps/as/cli/main.c`.
- Identifiers use ASCII character classes. Semicolons receive `T_SEMI`, but statement parsing uses newline boundaries. Block suites require a following indented line. See `c/apps/as/frontend/lexer.c` and `c/apps/as/frontend/parser.c`.
- Generic classes, generic methods, and type parameters declared by nested functions receive diagnostics. Identity comparisons require at least one operand to be an `AN_NONE` syntax node. Formatted string format specifications receive diagnostics. See `c/apps/as/frontend/parser.c`, `c/apps/as/frontend/expression.c`, and `c/apps/as/frontend/text.c`.
- Compiler bounds include 1 MiB per source, 64 modules, 512 functions, 1,024 types, and 65,536 syntax nodes. Sources containing NUL bytes are rejected. Recursive expression, type, and block parsing checks a shared depth bound of 64; each module retains up to 32 diagnostics and records further truncation. See `c/apps/as/ir/model.h`, `c/apps/as/frontend/parser.c`, `c/apps/as/frontend/expression.c`, `c/apps/as/frontend/support.c`, and `c/apps/as/common/diagnostic.h`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/frontend/lexer.h` | Token kinds, source spans, and lexer interfaces |
| `c/apps/as/frontend/lexer.c` | Scanning, indentation, continuation, and lexical recovery |
| `c/apps/as/frontend/parser.c` | Module loading, declarations, types, statements, suites, and recovery |
| `c/apps/as/frontend/expression.c` | Atoms, postfix operations, precedence, and interpolation fragments |
| `c/apps/as/frontend/assignment.c` | Multiple assignment and unpacking syntax |
| `c/apps/as/frontend/closure.c` | Lambda function and closure construction |
| `c/apps/as/frontend/text.c` | Escape decoding and formatted string construction |
| `c/apps/as/frontend/layout.c` | External record metadata validation |
| `c/apps/as/frontend/import.c` | Local, library, and overlay import lookup |
| `c/apps/as/frontend/support.c` | Token cursors, diagnostic routing, and node ownership |
| `c/apps/as/ir/model.h` | Project records, syntax nodes, ownership, and resource bounds |
| `c/apps/as/include/project.h` | Public project checking and lifetime interfaces |
| `c/apps/as/common/version.c` | Source version classification |
| `c/apps/as/common/numeric.h` | Language version constants |
| `c/apps/as/common/diagnostic.h` | Diagnostic records and capacity |
| `c/apps/as/common/diagnostic.c` | Byte ranges, display positions, and diagnostic formatting |
| `c/apps/as/sema/scope.c` | Declared and imported type lookup |
| `c/apps/as/sema/assignment.c` | Grouped assignment checking |
| `c/apps/as/sema/check.c` | Semantic checking entry and initializer order |
| `c/apps/as/cli/main.c` | Version handling and lexer token dump |
| `c/apps/as/cli/commands.c` | Command dispatch and diagnostic gating |
| `tests/unit/as_version_shebang_test.c` | Version header placement assertions |
| `tests/unit/as_lex_recovery_test.c` | Lexer recovery, operator, and allocation assertions |
| `tests/unit/as_recovery_test.py` | Project recovery and diagnostic assertions |

## See also

- [AetherScript: language and toolchain](overview.md)
- [AetherScript language guide](language-guide.md)
- [Type system and semantic analysis](type-system.md)
- [IR and the LLVM backend](ir-and-backend.md)
- [Testing AetherScript](testing.md)