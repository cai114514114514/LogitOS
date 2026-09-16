"""Render typed A3 wrappers from gen_abi's authoritative call descriptions.

The description owns argument order, buffer direction and byte extents. This
renderer owns native marshalling and the shared polling policy; it never keeps
another table of syscall numbers or per-call calling conventions.
"""

PRELUDE = '''
# ---- native argument ownership ----
# A3 str may be an interior view with no trailing NUL. A named Bytes temporary
# keeps each marshalled argument rooted across later argument allocations.
def _c_string(value: str) -> Bytes:
    if chr(0) in value:
        raise ValueError("C string contains an embedded NUL")
    return Bytes(value + chr(0))

def _check_size[B: ByteStorage](data: B, count: i64) -> None:
    if count < 0 or count > len(data):
        raise ValueError("ABI byte count exceeds backing storage")

def _check_text_size(value: str, count: i64) -> None:
    if count < 0 or count > len(value):
        raise ValueError("ABI byte count exceeds source text")

# argv contains raw addresses. Check its own termination without dereferencing
# those addresses; their pointees must remain live until the kernel has copied
# them. Neither this check nor a p layout field creates hidden GC ownership.
def _check_argv[B: ByteStorage](data: B) -> None:
    if len(data) < 8 or len(data) % 8 != 0:
        raise ValueError("argv requires complete pointer words and a terminator")
    for offset in range(0, len(data), 8):
        zero = true
        for byte in range(8):
            if data[offset + byte] != 0:
                zero = false
        if zero:
            return
    raise ValueError("argv has no NULL terminator")
'''

WAIT_PRELUDE = '''
# ---- waiting ----
# The A2 loop used RTC seconds and a midnight fixup to accommodate a host VM
# stub. Native host syscalls now fail explicitly. Real waits use the monotonic
# clock; a failed/backwards reading raises instead of masquerading as elapsed
# time. Kernel failure and timeout remain distinct typed exceptions.
def now_s() -> i64:
    clock = Time()
    if get_time(clock) < 0:
        raise IOError("wall clock unavailable")
    return (i64(clock.hour) * 60 + i64(clock.minute)) * 60 + i64(clock.second)

def await_(poll: Callable[[], i64], pending_neg: bool, pending_val: i64,
           has_fail: bool, fail_neg: bool, fail_val: i64,
           timeout_s: i64, label: str) -> i64:
    if timeout_s < 0:
        raise ValueError("timeout must be nonnegative")
    duration = timeout_s * 1000
    start = monotonic_ms()
    if start < 0:
        raise IOError("monotonic clock unavailable")
    while true:
        value = poll()
        pending = value < 0 if pending_neg else value == pending_val
        if not pending:
            failed = false
            if has_fail:
                failed = value < 0 if fail_neg else value == fail_val
            if failed:
                raise IOError(label + ": failed")
            return value
        now = monotonic_ms()
        if now < start:
            raise IOError("monotonic clock failed or moved backwards")
        if now - start >= duration:
            raise RuntimeError(label + ": timed out after " + str(timeout_s) + "s")
        sys_yield()
    # The current checker conservatively treats loop exits as reachable.
    raise RuntimeError("polling loop terminated unexpectedly")
'''


def parameter_names(arguments):
    names = []
    for kind, body in arguments:
        if kind == "pack":
            names.extend(name for name, _, _ in body)
        elif kind in ("str", "inbuf", "outbuf", "record", "argv"):
            names.append(body[0])
        elif kind != "lit":
            names.append(body)
    return names


def signature(name, arguments, extra=()):
    generics, parameters = [], []
    for kind, body in arguments:
        if kind == "pack":
            parameters.extend(f"{field}: i64" for field, _, _ in body)
        elif kind in ("inbuf", "outbuf", "argv"):
            parameter = f"B{len(generics)}"
            protocol = "MutableByteStorage" if kind == "outbuf" else "ByteStorage"
            generics.append(f"{parameter}: {protocol}")
            parameters.append(f"{body[0]}: {parameter}")
        elif kind == "record":
            parameters.append(f"{body[0]}: {body[1]}")
        elif kind == "str":
            parameters.append(f"{body[0]}: str")
        elif kind == "int":
            parameters.append(f"{body}: i64")
    parameters.extend(extra)
    suffix = "[" + ", ".join(generics) + "]" if generics else ""
    return f"def {name}{suffix}({', '.join(parameters)}) -> i64:"


def wrapper(name, symbol, arguments, scalar_expression):
    lines, expressions = ["", signature(name, arguments)], [symbol]
    for kind, body in arguments:
        if kind == "str":
            parameter, extent = body
            if extent:
                lines.append(f"    _check_text_size({parameter}, {extent})")
            lines.append(f"    _c_{parameter} = _c_string({parameter})")
            expressions.append(f"addr(_c_{parameter})")
        elif kind in ("inbuf", "outbuf", "record", "argv"):
            parameter = body[0]
            extent = body[2] if kind == "record" else body[1]
            if extent:
                lines.append(f"    _check_size({parameter}, {extent})")
            if kind == "argv":
                lines.append(f"    _check_argv({parameter})")
            expressions.append(f"addr({parameter})")
        else:
            expressions.append(scalar_expression(kind, body))
    lines.extend(["    unsafe:", f"        return syscall({', '.join(expressions)})"])
    return lines


def render_calls(entries, scalar_expression):
    lines = [PRELUDE.rstrip(), "", "# ---- calls (include/abi/logit_calls.abi) ----"]
    for entry in entries:
        if entry[0] == "call":
            _, name, symbol, arguments = entry
            lines.extend(wrapper(name, symbol, arguments, scalar_expression))
    waits = [entry for entry in entries if entry[0] == "wait"]
    if waits:
        lines.append(WAIT_PRELUDE.rstrip())
    for _, name, start, poll, arguments, pending, failure in waits:
        lines.extend(wrapper(name + "_start", start, arguments, scalar_expression))
        lines.extend(wrapper(name + "_poll", poll, [], scalar_expression))
        names = parameter_names(arguments)
        lines.extend(["", signature("wait_" + name, arguments, ["timeout_s: i64"]),
                      f"    if {name}_start({', '.join(names)}) < 0:",
                      f'        raise IOError("{name}: cannot start")'])
        flags = [name + "_poll", str(pending[0] == "lt").lower(), str(pending[1]),
                 str(failure is not None).lower(),
                 str(failure is not None and failure[0] == "lt").lower(),
                 str(failure[1] if failure else 0), "timeout_s", f'"{name}"']
        lines.append(f"    return await_({', '.join(flags)})")
    return "\n".join(lines)
