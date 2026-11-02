# AetherScript language guide

AetherScript 3 uses indentation to structure statically typed programs and compiles them to native code. This guide describes the declarations, types, expressions, control flow, functions, imports and error handling implemented by the current compiler. The complete examples are derived from compiler fixtures in `tests/fixtures/astyped/`.

## Source files and entry points

Begin each source file with:

```text
# aether: 3.0
```

The header `# aether: 3` selects the same version. An executable script can place a shebang on the preceding line. The header reader is in `c/apps/as/common/version.c`; imported files receive the same version check in `c/apps/as/frontend/parser.c`.

A colon introduces an indented block. Newlines inside parentheses, brackets and braces continue the expression. Comments begin with `#`. Identifiers contain ASCII letters, digits and underscores, beginning with a letter or underscore. These rules are implemented in `c/apps/as/frontend/lexer.c`.

A native executable enters `main`, which takes zero parameters and returns `i64` or `None`. An `i64` result supplies the exit status through the generated native entry point; an uncaught runtime error produces status 1. Entry generation is in `c/apps/as/backend/llvm/emit.c`.

The parser also supports scripts by collecting executable statements at module scope into a generated `main`. Module variable declarations with initializers become a module initialization function. These two kinds of module code are identified in `c/apps/as/frontend/parser.c`.

The built-in `args()` returns a fresh `List[str]` containing the launch arguments, including argument zero. Its signature and implementation are in `c/apps/as/sema/builtin.c` and `c/apps/as/runtime/process.c`.

## Declarations and scope

Assignment introduces a variable and infers its type from its first value. An annotation supplies the type before inference:

```text
count: i64 = 0
```

A local declaration such as `count: i64` establishes its type before assignment. The checker requires initialization on every path reaching a read. It merges facts from branches that continue to the following statement and treats a loop as potentially executing zero times. These rules are implemented in `c/apps/as/sema/check.c`.

Assignment bindings belong to the function scope. A name assigned later in a function already counts as a local at an earlier read, where initialization checking applies. `global count` selects a variable declared in the current module for rebinding. Field and element updates act through the existing owner binding. Name resolution is in `c/apps/as/sema/scope.c`.

Nested functions capture enclosing bindings through shared typed cells. Assignments to a captured binding update the storage shared with the enclosing function and other closures from that invocation. Capture analysis is in `c/apps/as/sema/closure.c`; cell generation is in `c/apps/as/backend/llvm/closure.c`.

Multiple assignment uses names as targets. For example, `a, b = b, a` evaluates both right sides before storing either result. Sequence unpacking, such as `a, b = values`, checks the sequence length and extracts the values before the first store. Parsing, checking and native lowering are in `c/apps/as/frontend/assignment.c`, `c/apps/as/sema/assignment.c` and `c/apps/as/backend/llvm/assignment.c`.

## Built-in types

The parser recognizes these scalar types and annotation aliases:

| Type | Meaning |
|---|---|
| `i8`, `i16`, `i32`, `i64` | Signed integers of the named width. |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers of the named width. |
| `f32`, `f64` | Floating point values. |
| `bool` | Boolean values, spelled `True` and `False`, or `true` and `false`. |
| `str` | Immutable text stored as bytes with a length. |
| `NoneType` | The value type of `None`; `nil` is another spelling of the value. |
| `int`, `isize`, `usize`, `float` | Annotation aliases for `i64`, `i64`, `u64` and `f64`. |
| `None`, `void` | Return annotations for a function with no result. |

Type names and aliases are defined in `c/apps/as/frontend/parser.c`.

An integer literal defaults to `i64`. An integer context checks the literal against the expected width and signedness. Decimal and hexadecimal integers are supported; a leading zero retains decimal interpretation. A floating point literal defaults to `f64` and takes `f32` in an `f32` context. Literal checking is in `c/apps/as/sema/expression.c`.

Composite and runtime types describe collections, callable values and resources:

| Type | Meaning |
|---|---|
| `Array[T, N]` | A fixed array containing `N` values inline. |
| `Slice[T]` | A borrowed sequence providing read access to elements. |
| `MutSlice[T]` | A borrowed sequence providing mutable access to elements. |
| `List[T]` | A managed, mutable sequence of one element type. |
| `Dict[K, V]` | A managed mapping with one key type and one value type. |
| `Optional[T]` | A value of type `T` or `None`. |
| `Callable[[T], R]` | A function value accepting `T` and returning `R`; the parameter list can contain several types. |
| `Any` | A boxed value retaining its concrete type. |
| `Range` | An arithmetic sequence produced by `range`. |
| `Buffer` | Managed, mutable byte storage. |
| `Bytes` | Managed, immutable byte storage. |
| `Cap` | A capability snapshot. |
| `Command` | A managed command description. |
| `Port`, `Process`, `Region` | Resources acquired through a `with` scope. |
| `Ptr[T]` | A raw address whose element type satisfies `Integer`. |

Type syntax is in `c/apps/as/frontend/parser.c`. Composite type construction and constraints are in `c/apps/as/sema/generic.c`.

A nonempty list or dictionary literal can infer its types from its first element or pair. Empty literals receive their types from context, such as `List[i64]` or `Dict[str, i64]`. An `Array` context supplies the element type and required length. The checker then checks every element against the chosen types in `c/apps/as/sema/expression.c`.

`Optional[T]` accepts `T` or `None` in a typed context. Testing an uncaptured local against `None` establishes presence information for the corresponding branch. For example, after `value is not None`, a read can use the payload type. For fields, module variables and captured bindings, copy the value into a local before testing it. Optional conversion and flow analysis are in `c/apps/as/sema/optional.c` and `c/apps/as/sema/check.c`.

`Any(value)` boxes a value. The typed operations `is_type[T]` and `cast[T]` each take one `Any` argument: the first tests its stored type, and the second extracts a value after checking that type. Their signatures are checked in `c/apps/as/sema/expression.c`.

## Structs, classes and layouts

A `struct` declares typed fields. Its constructor takes every field in declaration order, and assignment copies the inline value. This declaration appears in `tests/fixtures/astyped/numeric/main.as`:

```text
struct Vec2:
    x: f64
    y: f64
```

`Vec2(2.0, 3.0)` constructs a value, and `point.x` accesses its first field. Fields containing references retain those references when the struct is copied.

Generic structs declare type parameters in brackets after their name. An annotation supplies concrete arguments, while a constructor can infer them from field values. Parsing is in `c/apps/as/frontend/parser.c`; instantiation and declaration identity are handled in `c/apps/as/sema/types.c`.

A `class` creates a managed object shared by references. Methods receive `self` as their first parameter. This declaration is taken from `tests/fixtures/astyped/class/models.as`:

```text
class Named:
    name: str

    def label(self) -> str:
        return self.name
```

`Named("example")` initializes the field through the generated constructor. A class declaring `init` takes its constructor arguments from that method's parameters after `self`. The checker follows field initialization through control flow and requires every field to be initialized before construction completes or `self` escapes. Constructor checking is in `c/apps/as/sema/class.c`.

Classes support one base class, written in parentheses after the class name. An override preserves the inherited parameter and result types. Calls through a base reference select the object's override; `super` selects the parent implementation belonging to the enclosing class. Checking and native dispatch are in `c/apps/as/sema/inheritance.c` and `c/apps/as/backend/llvm/class.c`.

ABI records use a module declaration through `layout`, with a literal record name, size and field metadata. They provide named fields over byte storage, and assignment shares the record. Declaration validation is in `c/apps/as/frontend/layout.c`; field access is in `c/apps/as/backend/llvm/layout.c`.

## Expressions and numeric operations

Expressions include literals, calls, field access, indexing, unary operators and binary operators. The conditional expression `a if condition else b` evaluates the chosen branch. Boolean `and` and `or` evaluate their right operand when the left operand requires it. Conditions and Boolean operands have type `bool`.

The parser's precedence runs from lower to higher as follows:

| Operators | Grouping |
|---|---|
| Conditional expression | Outside the binary operator levels. |
| `or`, `\|>`, `->`, `<-` | Left to right. |
| `and` | Left to right. |
| Unary `not` | Takes a comparison expression as its operand. |
| `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `not in`, `is`, `is not` | Left to right. |
| `\|`, then `^`, then `&` | Three successive bitwise levels. |
| `<<`, `>>` | Left to right. |
| `+`, `-` | Left to right. |
| `*`, `/`, `%` | Left to right. |
| Unary `+`, `-`, `~` | Numeric unary level. |
| `**` | Right to left. |

Parentheses control grouping. The operators `is` and `is not` express comparisons against `None`. The grammar is implemented in `c/apps/as/frontend/expression.c`; conditional and Boolean evaluation are lowered in `c/apps/as/backend/llvm/emit.c`.

Assignments and function arguments are checked against their destination types. Numeric constructors such as `i32(value)` and `f64(value)` perform conversions. Conversions to integers check representability and raise `ConversionError` on failure. The checks are emitted in `c/apps/as/backend/llvm/numeric.c`.

Arithmetic and numeric comparisons promote operands involving floating point values to `f64` when either operand is `f64`, and otherwise to `f32`. Integer operations require matching operand types, with literals receiving context during checking. Promotion is defined in `c/apps/as/sema/numeric.c`.

| Operation | Native behavior |
|---|---|
| Integer addition, subtraction, multiplication and negation | Raise `OverflowError` when the result exceeds the type's range. |
| Integer division and remainder | Division truncates toward zero; remainder follows that quotient. |
| Division or remainder by zero, including floating point operands | Raise `ZeroDivisionError`. |
| Minimum signed integer divided by, or taken modulo, `-1` | Raise `OverflowError`. |
| Integer shifts | Counts outside zero through width minus one raise `ValueError`; checked left shift also checks representability. |
| Integer power | Negative exponents raise `ValueError`; multiplication overflow raises `OverflowError`. |
| Floating point power | Supports negative exponents and produces infinity or NaN where applicable. |
| `wrapping_add`, `wrapping_sub`, `wrapping_mul`, `wrapping_shl` | Wrap results at the integer width; shift counts still receive validation. |

These operations are lowered in `c/apps/as/backend/llvm/numeric.c`.

Compound assignment supports `+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `&=`, `|=`, `^=`, `<<=` and `>>=`. It evaluates the target owner and index, reads the old value, evaluates the right side, then computes and stores the result. Each owner and index expression runs once. The backend resolves container storage again after right side evaluation so that a resize during that evaluation preserves the intended target. See `c/apps/as/backend/llvm/compound.c`.

Strings and `Bytes` compare contents. Lists, dictionaries and class objects compare identity. Callable equality compares the function entry and captured environment. Comparison checking is in `c/apps/as/sema/generic.c`; native equality is implemented in `c/apps/as/backend/llvm/numeric.c` and `c/apps/as/backend/llvm/sequence.c`.

## Text, bytes and collections

Single and double quotes delimit strings. Escape decoding includes newline, carriage return, tab and NUL. Formatted strings interpolate expressions, as in `f"{value}"`; doubled braces produce literal braces. Parsing and escape decoding are in `c/apps/as/frontend/text.c`.

Strings support concatenation, repetition, comparison, membership and indexing. Methods include `find`, `slice`, `sub`, `split`, `join`, `replace`, `strip`, `upper` and `lower`. Lengths and positions count bytes, and case conversion handles ASCII. String indexing and iteration produce strings containing one byte. Method checking is in `c/apps/as/sema/expression.c`; text operations are in `c/apps/as/runtime/text.c`.

`Bytes(text)` copies a string's bytes. Constructing `Bytes` from a `Buffer` copies its contents into immutable storage. The `decode` method on `Bytes` validates UTF-8 and returns text, raising `ConversionError` for malformed input. These operations are implemented in `c/apps/as/backend/llvm/buffer.c` and `c/apps/as/runtime/bytes.c`. The conversion `str(value)` provides a display representation.

Lists support indexed assignment and `append`. Dictionaries support indexed assignment, `get`, `has`, `remove`, `keys` and `values`. A dictionary's key type satisfies `Hashable`, which accepts integers, booleans, strings and floating point values. With one argument, `get` returns an optional value; a second argument supplies a fallback of the dictionary's value type. These rules are checked in `c/apps/as/sema/expression.c` and `c/apps/as/sema/generic.c`.

Array and slice indices run from zero through length minus one. Lists, strings, byte storage and ranges also accept negative indices relative to their length. Invalid sequence indices raise `IndexError`; a missing key read through dictionary indexing raises `KeyError`. Indexing is implemented across `c/apps/as/backend/llvm/emit.c`, `c/apps/as/backend/llvm/buffer.c`, `c/apps/as/backend/llvm/dict.c` and `c/apps/as/backend/llvm/range.c`.

List comprehensions use an expression binding, as in `[value * 2 for value in values if value > 0]`. The iterable is evaluated before the binding becomes visible. For each item, the filter runs before the element expression. A comprehension produces a `List` with one element type. Its scope and type rules are in `c/apps/as/sema/comprehension.c`.

## Control flow and functions

Statements include `if` with `elif` and `else`, `while`, `for`, `break`, `continue`, `return`, `assert` and `pass`. Conditions and assertions require `bool`. Statement parsing and flow checking are in `c/apps/as/frontend/parser.c` and `c/apps/as/sema/check.c`.

A `for` loop evaluates its iterable once on entry. Arrays, slices and lists yield elements; dictionaries yield a snapshot of their keys; strings yield byte views; byte storage yields `i64` byte values. List iteration checks the current list length at each iteration. Ports support line iteration. Native loop generation is in `c/apps/as/backend/llvm/loop.c` and `c/apps/as/backend/llvm/emit.c`.

`range` accepts a stop value, start and stop, or start, stop and step. Its arguments have type `i64`. The stop is excluded, either step direction is supported, and a zero step raises `ValueError`. Stored ranges support length, indexing and membership using their bounds and step. The implementation is in `c/apps/as/runtime/range.c` and `c/apps/as/backend/llvm/range.c`.

Functions begin with `def`. Public module functions declare parameter and return types. Private module functions, whose names begin with an underscore, can infer one signature from calls and reachable returns. A recursive function supplies a return annotation when inference would depend on its recursive call. These rules are enforced in `c/apps/as/frontend/parser.c`, `c/apps/as/sema/expression.c` and `c/apps/as/sema/check.c`.

Generic functions declare bracketed type parameters. Their constraints describe the operations available while checking the body. Concrete calls infer type arguments and produce native specializations. The built-in constraints are `Number`, `Integer`, `Hashable`, `Equatable`, `Ordered`, `ByteStorage` and `MutableByteStorage`. Constraint checking and specialization are in `c/apps/as/sema/generic.c`.

This complete program combines the generic sum from `tests/fixtures/astyped/generics/algorithms.as` with callers reduced from `tests/fixtures/astyped/generics/main.as`:

```text
# aether: 3

def total[T: Number](values: Slice[T]) -> T:
    result = T(0)
    for value in values:
        result += value
    return result

def main() -> None:
    integers: Array[i64, 3] = [1, 2, 3]
    floats: Array[f64, 2] = [1.25, 2.5]
    print(total(integers), total(floats))
```

The arrays provide borrowed slices for the calls. `T(0)` constructs an accumulator of the inferred numeric type.

Functions are also values with `Callable` signatures. Nested `def` declarations and `lambda` expressions capture enclosing bindings. A callable context, information from another generic argument, or immediate call arguments can supply a lambda's parameter types. Examples appear in `tests/fixtures/astyped/closure/main.as`; capture checking and native environments are implemented in `c/apps/as/sema/closure.c` and `c/apps/as/backend/llvm/closure.c`.

## Modules and imports

`import` binds a module name, and `from` binds selected declarations. Both forms support aliases. A dotted import maps its components to directories and a source filename. Without an alias, the final component becomes the bound name.

Imports search beside the importing source and then in the configured standard library. The `std.` prefix selects the standard library root. Names beginning with an underscore receive privacy checks when imported or accessed through a module. Imported types retain the identity of their original declaration. Resolution is implemented in `c/apps/as/frontend/import.c` and `c/apps/as/sema/scope.c`.

This complete two file program is copied from the module fixtures. `tests/fixtures/astyped/modules/math.as` contains:

```text
# aether: 3
def square(value: i64) -> i64:
    return value * value
```

`tests/fixtures/astyped/modules/main.as` contains:

```text
# aether: 3
import math

def main() -> i64:
    total = 0
    for value in range(1, 5):
        total += math.square(value)
    print(total)
    return 0
```

Loading gathers declarations and dependencies, and the checker validates imported names even when expressions leave them unused. At startup, module initializers execute once in dependency postorder. Assignments within each initializer follow source order.

Direct reads before module variable initialization receive a compile diagnostic. Reads through a called function receive a runtime initialization check. Loading and ordering are implemented in `c/apps/as/frontend/parser.c`; initialization calls and runtime guards are emitted by `c/apps/as/backend/llvm/emit.c`.

## Errors and recovery

`try` protects a block, and `except` selects an error category. An optional `else` suite runs when the protected body reaches its end successfully. Handlers are considered in source order. `except Error` and a bare `except` match every category.

The built-in categories are `OverflowError`, `ZeroDivisionError`, `IndexError`, `ConversionError`, `ValueError`, `AssertionError`, `RuntimeError`, `IOError`, `MemoryError`, `TypeError`, `KeyError` and `PermissionError`. Their shared definition is in `c/apps/as/runtime/exception.h`.

An exception constructor takes a message string. `raise` throws the resulting error value. A bare `raise` inside a handler rethrows its captured error. Error records expose immutable fields named `code`, `message`, `file`, `line` and `column`. Rethrowing preserves the original location. Checking and runtime behavior are implemented in `c/apps/as/sema/check.c` and `c/apps/as/runtime/exception.c`.

This complete program combines reduced portions of `tests/fixtures/astyped/exceptions/operations.as` and `tests/fixtures/astyped/exceptions/main.as`:

```text
# aether: 3.0

def divide(left: i64, right: i64) -> i64:
    return left / right

def recover() -> i64:
    try:
        return divide(1, 0)
    except ZeroDivisionError:
        return 42

def main() -> None:
    print(recover(), divide(12, 3))
```

Generated calls propagate pending errors to the nearest enclosing handler. Catch entry captures the error and clears pending state, allowing subsequent calls to proceed. Failures inside a handler or its `else` suite propagate to an enclosing handler. Native handling is generated in `c/apps/as/backend/llvm/emit.c`.

## Scoped resources and systems operations

`with` binds resources for a lexical scope using assignment syntax. Acquisitions include files through `open`, descriptor views through `port`, both pipe endpoints through `pipe`, processes through command startup, and memory through `region`. Cleanup runs in reverse acquisition order on scope completion, returns, loop exits and exception propagation. Binding checks and cleanup generation are in `c/apps/as/sema/resource.c` and `c/apps/as/backend/llvm/resource.c`.

A `Region` provides ownership transfer through its `move` method and byte views through `borrow` and `borrow_mut`. These methods acquire their results in another `with` scope. Move and borrow analysis tracks ownership through control flow and checks shared access, mutable access and reborrows. See `c/apps/as/sema/region.c`, `c/apps/as/sema/region_flow.c` and `c/apps/as/sema/region_borrow.c`.

`run` constructs a command description. Command descriptions support `|>` pipelines, `->` output redirection and `<-` input redirection. Methods including `wait`, `out` and `start` select execution behavior. A pipeline or redirection used as an expression statement executes synchronously. Command checking is in `c/apps/as/sema/command.c` and `c/apps/as/sema/check.c`.

An `unsafe:` block permits raw memory and syscall operations during checking. Operations including `addr`, `peek8`, `poke8` and `syscall` also check the process's held raw capability at runtime. The checker enforces lexical permission, while generated code and runtime helpers check authority. See `c/apps/as/sema/memory.c`, `c/apps/as/sema/system.c`, `c/apps/as/backend/llvm/memory.c` and `c/apps/as/runtime/system.c`.

## Limitations

- The accepted source version is 3.0, also spelled `3`. Earlier engines and future minor versions are rejected. A module must choose a declared `main` or executable statements that generate it. Import cycles are diagnosed. See `c/apps/as/common/version.c`, `c/apps/as/cli/main.c` and `c/apps/as/frontend/parser.c`.
- User functions have fixed positional arity. Generic classes, generic methods, nested functions declaring their own type parameters, custom protocol constraints, user exception hierarchies, `finally` and formatted string specifications are unsupported. See `c/apps/as/frontend/parser.c` and `c/apps/as/frontend/text.c`.
- Borrowed slices cannot escape through returns, module storage, closures or managed containers. Resource owners cannot be captured, stored in aggregates or passed through function signatures. These checks keep scoped storage tied to its lifetime. See `c/apps/as/sema/check.c`, `c/apps/as/sema/closure.c` and `c/apps/as/sema/resource.c`.
- `Any` supports boxing, checked extraction, formatting and equality; dynamic arithmetic and calls through `Any` are unsupported. Floating point `**=` is rejected. Raw addresses require the caller to maintain valid storage and lifetimes, and byte positions in text can select part of a multibyte character. See `c/apps/as/sema/expression.c`, `c/apps/as/sema/check.c`, `c/apps/as/backend/llvm/memory.c` and `c/apps/as/backend/llvm/emit.c`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/common/version.c` | Version headers and shebang handling. |
| `c/apps/as/frontend/lexer.c` | Tokens, literals, comments, identifiers and indentation. |
| `c/apps/as/frontend/parser.c` | Types, declarations, statements, imports, module initialization and generated entry functions. |
| `c/apps/as/frontend/expression.c` | Expression grammar, precedence, calls and comprehensions. |
| `c/apps/as/frontend/assignment.c` | Multiple assignment and unpacking syntax. |
| `c/apps/as/frontend/text.c` | Escape decoding and formatted strings. |
| `c/apps/as/frontend/import.c` | Import paths and standard library resolution. |
| `c/apps/as/frontend/layout.c` | ABI record declaration validation. |
| `c/apps/as/sema/scope.c` | Names, imports, globals and function scope. |
| `c/apps/as/sema/check.c` | Initialization, control flow, returns and escape restrictions. |
| `c/apps/as/sema/expression.c` | Expression types, constructors, calls and collection methods. |
| `c/apps/as/sema/generic.c` | Composite types, constraints, inference and specialization. |
| `c/apps/as/sema/types.c` | Generic struct instantiation. |
| `c/apps/as/sema/numeric.c` | Numeric promotion and assignment compatibility. |
| `c/apps/as/sema/optional.c` | Optional conversion and presence analysis. |
| `c/apps/as/sema/class.c` | Constructors, field initialization and methods. |
| `c/apps/as/sema/inheritance.c` | Subtyping, overrides and `super`. |
| `c/apps/as/sema/closure.c` | Captured bindings and closure signatures. |
| `c/apps/as/sema/comprehension.c` | Comprehension bindings and element types. |
| `c/apps/as/sema/resource.c` | Resource acquisitions and ownership restrictions. |
| `c/apps/as/sema/region.c` | Region moves, borrows and view arguments. |
| `c/apps/as/sema/region_flow.c` | Region ownership through control flow. |
| `c/apps/as/sema/region_borrow.c` | Borrow access checks. |
| `c/apps/as/sema/command.c` | Command construction and execution method signatures. |
| `c/apps/as/sema/memory.c` | Raw memory operation signatures and lexical checks. |
| `c/apps/as/sema/system.c` | Direct syscall checking. |
| `c/apps/as/backend/llvm/emit.c` | Native entries, expressions, initialization and exception handling. |
| `c/apps/as/backend/llvm/numeric.c` | Arithmetic, conversions, power and numeric errors. |
| `c/apps/as/backend/llvm/assignment.c` | Unpacking checks and store ordering. |
| `c/apps/as/backend/llvm/compound.c` | Compound assignment evaluation and storage resolution. |
| `c/apps/as/backend/llvm/sequence.c` | Equality and sequence membership. |
| `c/apps/as/backend/llvm/loop.c` | Iteration, dictionary snapshots and range cursors. |
| `c/apps/as/backend/llvm/class.c` | Object construction and method dispatch. |
| `c/apps/as/backend/llvm/closure.c` | Captured cells and native closure environments. |
| `c/apps/as/backend/llvm/layout.c` | ABI record field access. |
| `c/apps/as/backend/llvm/buffer.c` | Byte storage construction and access. |
| `c/apps/as/backend/llvm/dict.c` | Dictionary lookup and storage operations. |
| `c/apps/as/backend/llvm/range.c` | Range construction, length and indexing. |
| `c/apps/as/backend/llvm/resource.c` | Resource acquisition and cleanup generation. |
| `c/apps/as/backend/llvm/memory.c` | Raw capability checks and memory access. |
| `c/apps/as/runtime/process.c` | Launch arguments. |
| `c/apps/as/runtime/text.c` | String operations using byte positions. |
| `c/apps/as/runtime/bytes.c` | UTF-8 validation and decoding. |
| `c/apps/as/runtime/range.c` | Range bounds, indexing and membership. |
| `c/apps/as/runtime/exception.h` | Error categories and record fields. |
| `c/apps/as/runtime/exception.c` | Error capture, propagation, locations and reporting. |
| `c/apps/as/runtime/system.c` | Direct syscall runtime authority checks. |
| `tests/fixtures/astyped/numeric/main.as` | Struct and numeric examples. |
| `tests/fixtures/astyped/class/models.as` | Class and constructor examples. |
| `tests/fixtures/astyped/generics/algorithms.as` | Generic sum used in the complete example. |
| `tests/fixtures/astyped/generics/main.as` | Concrete callers for generic functions. |
| `tests/fixtures/astyped/closure/main.as` | Closures and lambda inference examples. |
| `tests/fixtures/astyped/modules/math.as` | Exported function in the module example. |
| `tests/fixtures/astyped/modules/main.as` | Complete import and loop example. |
| `tests/fixtures/astyped/exceptions/operations.as` | Division and recovery functions. |
| `tests/fixtures/astyped/exceptions/main.as` | Catches, rethrowing and recovery examples. |

## See also

- [AetherScript: language and toolchain](overview.md)
- [Type system and semantic analysis](type-system.md)