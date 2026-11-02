# AetherScript type system and semantic analysis

AetherScript’s typed compiler resolves names, infers expression types, checks control flow, and records decisions for execution and LLVM lowering. The analyzer in `c/apps/as/sema/` operates on the project model in `c/apps/as/ir/model.h`. Checked nodes carry binding IDs, function selections, field indices, and conversions, giving diagnostics, editor queries, and code generation a common representation.

## Checking a project

`as_typed_check_with_library()` in `c/apps/as/frontend/parser.c` allocates an `AsTypedProject`, initializes built-in types, and loads the entry module and its imports. After loading and parsing complete without errors, it calls `at_check_functions()`.

The project owns its source buffers, tokens, nodes, and declaration tables. Tokens refer into the retained source buffers, so diagnostic locations remain valid until `as_typed_free()` releases the snapshot. Source overlays are copied during loading, allowing an editor to continue changing its buffers after checking returns. The public interface is in `c/apps/as/include/project.h`.

`at_check_functions()` in `c/apps/as/sema/check.c` coordinates these stages:

1. Validate imports, declaration conflicts, and class overrides.
2. Check module initializers in import postorder.
3. Validate declared field types and recursive value layouts.
4. Check function bodies, resolving inference and creating specializations as needed.
5. Check storage types, captures, and resource signatures.
6. Enter region liveness and borrow analysis when the preceding checks have produced no errors.

Checking module initializers establishes types and initialization facts. Runtime execution later evaluates their statements.

## Type representation

A type is an integer index into the project’s `AtType` table. Expressions, bindings, function signatures, and aggregate fields refer to types through these indices. The fixed table also keeps addresses stable while recursive checking retains pointers to entries. Its representation and capacity are defined in `c/apps/as/ir/model.h`.

| Type family | Representation |
|---|---|
| Scalars | Built-in entries for `bool`, signed and unsigned integers of 8, 16, 32, and 64 bits, `f32`, `f64`, and `str`. |
| Dynamic values | A built-in `Any` entry. |
| Optional values | `AT_OPTIONAL`, with the payload type in `element`. |
| Sequences | Arrays carry an element type and length; lists and shared or mutable slices carry an element type. |
| Dictionaries | `key` holds the key type; `element` holds the value type. |
| Callables | `fields` holds parameter types, `count` their number, and `element` the result type. |
| Declared types | Structs, classes, and layout records retain their module, fields, and declaration locations. Classes also carry a base type. |
| Generic parameters | `AT_PARAMETER` entries carry a name and constraint. |
| System facilities | Built-in entries include `Buffer`, `Bytes`, `Cap`, `Port`, `Command`, `Process`, and `Region`; `AT_POINTER` carries a pointee type. |

Compound types reuse existing entries with matching components. `c/apps/as/sema/generic.c` handles containers and dictionaries; `c/apps/as/sema/callable.c` handles callable signatures. Declared types retain identity through their declarations and modules.

In annotations, the parser maps `int` and `isize` to `i64`, `usize` to `u64`, and `float` to `f64`. It maps `None` and `void` to `AT_VOID`, which represents a function returning no value. The expression `None` has type `AT_NONE`, displayed as `NoneType`. These rules appear in `c/apps/as/frontend/parser.c` and `c/apps/as/sema/expression.c`.

Type index zero is `AT_ERROR`; unresolved inference slots also start at zero. `at_check_mismatch()` in `c/apps/as/sema/scope.c` reports a mismatch when both types are known, allowing an earlier error to propagate through enclosing expressions with fewer repeated diagnostics.

## Name resolution and scopes

`c/apps/as/sema/scope.c` owns binding lookup. Local resolution searches active lexical bindings, the function’s local table, and enclosing functions through closure capture. Expression lookup then searches module variables, including imported variables, followed by function declarations and system constants.

Before checking statements, `at_scope_collect_bindings()` processes `global` declarations and collects assignment, loop, and exception targets. This establishes binding identity across the function before control flow determines initialization. A read preceding a later assignment resolves to that function binding, allowing the checker to report a read before initialization.

Assignments can resolve to an enclosing captured binding. A `global` declaration selects a variable declared in the current module. Call resolution gives user function declarations precedence over predeclared built-ins.

Comprehensions and `with` statements introduce lexical bindings through `AtScopedBinding`, defined in `c/apps/as/sema/internal.h`. Their local slots have hidden names, while lookup associates the source spelling with the active slot:

- A comprehension checks its iterable in the surrounding scope, then exposes the iteration binding to its filter and element expression.
- A `with` statement exposes acquired owners or borrowed views within its body.

The implementations are in `c/apps/as/sema/comprehension.c` and `c/apps/as/sema/resource.c`.

Imports provide member bindings, aliases, and qualified module access. `at_scope_check_imports()` checks missing members, duplicate import bindings, and conflicts with declarations. The parser rejects member imports beginning with `_`; qualified resolution checks private functions, variables, and constructors. See `c/apps/as/frontend/parser.c` and `c/apps/as/sema/scope.c`.

Nested functions resolve captures through their parent’s active checker. Capture records connect a child local slot to a parent slot, and intermediate functions forward captures needed by deeper closures. Both ends are marked for shared cell storage. Capture construction also checks initialization. `c/apps/as/sema/closure.c` establishes these records, and `c/apps/as/backend/llvm/closure.c` uses them to construct closure environments.

## Inference and expression checking

`at_check_expression()` receives an expected type and records the checked type on the node. Expected types flow from annotations, existing bindings, parameters, return signatures, and container elements.

| Context | Checking rule |
|---|---|
| Integer literal | Use an expected concrete integer type when available; otherwise use `i64`. Check the magnitude against its width and signedness. |
| Floating literal | Use `f32` when requested; otherwise use `f64`. |
| Local assignment | Use an annotation or established binding type, or infer the type from the initializer. |
| List or dictionary literal | Use container context, or infer component types from the first entry. Check subsequent entries against those types. |
| Fixed array literal | Check element types and compare the element count with the declared length. |
| Conditions and assertions | Require `bool`. |
| Function call | Check argument count and compatibility with parameter types. |
| Callable context | Supply unresolved parameter types and guide specialization of generic function values. |

These rules are implemented in `c/apps/as/sema/expression.c`, `c/apps/as/sema/check.c`, and `c/apps/as/sema/callable.c`.

Private module functions whose names begin with `_` can infer missing parameter types from calls and a result type from their body. Each function acquires one static signature; later calls are checked against it. Callable context can also supply unresolved parameter types for a function value.

Closures obtain missing parameter types from a matching `Callable` context. That context can supply a concrete result type while the closure body is checked. An immediately invoked closure can learn parameter types from its arguments. These paths are in `c/apps/as/sema/closure.c` and `c/apps/as/sema/callable.c`.

Numeric operations distinguish an expression’s result type from a binding’s storage type. For arithmetic and comparisons involving concrete floating operands, `at_numeric_type()` chooses `f64` when either operand is `f64`, otherwise `f32`. Integer operands can participate in these operations through numeric conversion. Assignment checking then compares the result with the destination type, including compound assignments. For example, an integer destination receiving a floating result produces a type error. See `c/apps/as/sema/numeric.c`.

Equality and ordering check the `Equatable` and `Ordered` constraints. Bitwise operations and shifts require integer operands. Binary power accepts integers and concrete floating types; power compound assignment requires integers. These operator rules appear in `c/apps/as/sema/expression.c` and `c/apps/as/sema/check.c`.

`Any(value)` boxes a value into `Any`. The forms `cast[T]` and `is_type[T]` consume an `Any` value and record a checked cast or type test in `c/apps/as/sema/expression.c`.

## Optional values and control flow

`c/apps/as/sema/optional.c` records optional injection, proven payload access, and class upcasts through `AtNode.value_type` and `AtNode.conversion`. An `Optional[T]` context accepts `None`, a value of `T`, or a compatible class subtype.

Comparisons with `None` establish presence facts for local optional bindings. The checker applies those facts in branches, short-circuit expressions, assertions, and conditional expressions. A conditional expression combining a value with `None` can infer an optional result.

A narrowed read records payload access while the binding retains its optional storage type. LLVM lowering consumes the recorded conversion in `c/apps/as/backend/llvm/emit.c`, preserving the relationship between storage and the value used by an expression.

Statement checking in `c/apps/as/sema/check.c` tracks initialization and optional presence together. At a branch join, facts are intersected across paths that reach the following statement. Returning or throwing branches contribute their exit status; reaching branches determine subsequent initialization facts.

Loops restore incoming initialization facts because their bodies may execute zero times. The checker clears incoming optional presence facts before checking repeated conditions and bodies. Exception handlers start from initialization facts saved before the protected body, with optional presence reconsidered for handler entry.

Constructor initialization uses the same control-flow joins. `c/apps/as/sema/class.c` tracks field stores and checks field reads, constructor exits, and uses that expose `self`. Override checking in `c/apps/as/sema/inheritance.c` compares parameter counts, parameter types after `self`, and result types, keeping inherited method slots compatible.

## Generics and specialization

AetherScript supports generic functions and structs. Function type parameters can carry built-in protocol constraints. For example, `tests/fixtures/astyped/generics/algorithms.as` contains:

```python
def twice[T: Number](value: T) -> T:
    return value + value
```

The supported constraints are `Number`, `Integer`, `Hashable`, `Equatable`, `Ordered`, `ByteStorage`, and `MutableByteStorage`. The parser recognizes their names in `c/apps/as/frontend/parser.c`; `c/apps/as/sema/generic.c` determines which types satisfy them. Template checking validates operations against the constraint attached to each parameter.

At a call, inference matches argument types against parameter patterns. Matching descends through containers, optionals, callables, and matching generic struct instances. An expected result type can fill unresolved bindings through the patterns handled by `at_bind_result_context()`. Bindings learned from arguments take precedence. Callback checking can be deferred until other arguments provide its type context. The call orchestration is in `c/apps/as/sema/expression.c`.

`at_specialize()` in `c/apps/as/sema/generic.c` caches concrete functions by template and type arguments. It clones the body, substitutes types, and resets local checking state. Call and function-value checking then check the concrete function body. Publishing its signature before checking the body lets recursive calls reuse the same instance.

Generic struct annotations use `at_instantiate_struct()` in `c/apps/as/sema/types.c`. Each instance retains its template and arguments, and its fields receive substituted types. The instance is registered before field substitution, allowing recursive references to find an instance under construction. The layout walk in `c/apps/as/sema/check.c` follows inline struct, array, and optional storage when checking recursive value layouts.

## Ownership and storage checks

Semantic analysis also checks resource and view lifetimes. `with` acquisitions create scoped owners for ports, processes, and regions, or scoped views for region borrows. `c/apps/as/sema/resource.c` checks acquisition forms and creates their bindings.

Region analysis uses two passes:

- `c/apps/as/sema/region_flow.c` tracks whether a region owner remains live on every path reaching a use. Its loop analysis iterates ownership facts to a fixed point.
- `c/apps/as/sema/region_borrow.c` tracks active shared and mutable loans. Loans from earlier call arguments remain active while later arguments are checked, exposing conflicts across argument evaluation.

Pointer dereference requires an enclosing `unsafe` block. Each function starts with a fresh checker state, so the unsafe depth follows the function containing the operation. These checks are in `c/apps/as/sema/expression.c` and `c/apps/as/sema/check.c`.

## Diagnostics and source locations

Semantic errors pass through `at_check_error()` or `at_error()`. The latter selects the diagnostic buffer associated with the supplied module before reporting the token span. This keeps imported-file diagnostics attached to their owning source. See `c/apps/as/sema/scope.c` and `c/apps/as/frontend/support.c`.

`AsDiagnostic`, defined in `c/apps/as/common/diagnostic.h`, stores a code, message, help text, byte span, start and end positions, and an optional related location. Byte offsets identify source edits. Lines and columns start at one; column calculation advances over UTF-8 characters by skipping continuation bytes. Span adjustment expands boundaries to whole UTF-8 characters. The implementation is in `c/apps/as/common/diagnostic.c`.

| Code | Representative use |
|---|---|
| `AS3201` | Required signature information or unresolved inference. |
| `AS3202` | Type mismatch or invalid operands. |
| `AS3203` | Integer literal outside its machine type. |
| `AS3204` | Argument-count mismatch. |
| `AS3205` | Unknown variable, function, or member. |
| `AS3206` | Read before initialization. |
| `AS3208` | A function producing a value can reach its end. |
| `AS3402` | Region liveness error. |
| `AS3403` | Borrow conflict. |
| `AS3601` | Constraint failure. |
| `AS3602` | Conflicting or unresolved generic inference. |

`as_typed_report()` in `c/apps/as/cli/report.c` produces text and JSON reports. Text output includes the path, line, column, code, message, source line, and caret. JSON includes spans, source checksums, related locations, source records, declared symbols, and a truncation flag. Borrow conflicts attach the originating loan location; import-cycle reports include the ordered module chain.

The `check` command in `c/apps/as/cli/commands.c` returns zero when checking succeeds and one when the project has errors.

## Limitations

- Public module functions and generic functions require parameter and result annotations. A recursive call that re-enters a function while its result type is unresolved produces an inference error; a result annotation resolves that case. Empty list and dictionary literals require suitable container context. See `c/apps/as/frontend/parser.c`, `c/apps/as/sema/check.c`, and `c/apps/as/sema/expression.c`.
- Generic classes, generic methods, and nested functions declaring their own type parameters are rejected by `c/apps/as/frontend/parser.c`.
- Optional narrowing uses local snapshots. Fields, module variables, and captured optional bindings require a local copy for narrowing. See `c/apps/as/sema/optional.c` and `c/apps/as/sema/check.c`.
- Borrowed slices are rejected in returned values, module storage, heap container elements, and closure captures. Resource owners are rejected in aggregate storage and function signatures. See `c/apps/as/sema/check.c` and `c/apps/as/sema/resource.c`.
- Compiler capacities include 1,024 type entries and eight type parameters per declaration. Diagnostic storage retains up to 32 errors per module, with truncation exposed in JSON reports. See `c/apps/as/ir/model.h`, `c/apps/as/common/diagnostic.h`, and `c/apps/as/cli/report.c`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/ir/model.h` | Project, type, binding, function, and node representations; capacities. |
| `c/apps/as/include/project.h` | Public project interface and snapshot ownership contract. |
| `c/apps/as/frontend/parser.c` | Annotations, declaration rules, imports, and project checking entry points. |
| `c/apps/as/frontend/support.c` | Diagnostic routing and node allocation. |
| `c/apps/as/sema/internal.h` | Function checker state and lexical binding records. |
| `c/apps/as/sema/scope.c` | Name lookup, binding collection, import checks, and mismatch reporting. |
| `c/apps/as/sema/expression.c` | Expression inference, calls, literals, operators, and conversions. |
| `c/apps/as/sema/check.c` | Statement flow, initialization, returns, layouts, and storage checks. |
| `c/apps/as/sema/numeric.c` | Numeric categories, promotion, and assignment compatibility. |
| `c/apps/as/sema/optional.c` | Optional conversions, presence facts, and conditional values. |
| `c/apps/as/sema/generic.c` | Constraints, type binding, substitution, and function specialization. |
| `c/apps/as/sema/types.c` | Type allocation and generic struct instantiation. |
| `c/apps/as/sema/callable.c` | Callable types, function values, and indirect calls. |
| `c/apps/as/sema/closure.c` | Capture resolution and contextual closure checking. |
| `c/apps/as/sema/comprehension.c` | Comprehension scope and element checking. |
| `c/apps/as/sema/class.c` | Constructor initialization and class member checking. |
| `c/apps/as/sema/inheritance.c` | Class subtyping and override compatibility. |
| `c/apps/as/sema/resource.c` | Scoped resource bindings and ownership restrictions. |
| `c/apps/as/sema/region_flow.c` | Region liveness across branches, loops, and exits. |
| `c/apps/as/sema/region_borrow.c` | Loan conflicts and related diagnostic locations. |
| `c/apps/as/common/diagnostic.h` | Diagnostic records and per-module capacity. |
| `c/apps/as/common/diagnostic.c` | Source spans, positions, and diagnostic formatting. |
| `c/apps/as/cli/report.c` | Project text and JSON reports. |
| `c/apps/as/cli/commands.c` | Check-command reporting and exit status. |
| `c/apps/as/backend/llvm/closure.c` | Shared capture cells and closure environments. |
| `c/apps/as/backend/llvm/emit.c` | Consumption of checked expression conversions. |
| `tests/fixtures/astyped/generics/algorithms.as` | Generic function examples used by the typed fixtures. |

## See also

- [AetherScript language guide](language-guide.md)
- [AetherScript lexer and parser](frontend.md)
- [AetherScript IR and the LLVM backend](ir-and-backend.md)
- [The AetherScript command line and editor support](cli-and-editor.md)
- [Testing AetherScript](testing.md)