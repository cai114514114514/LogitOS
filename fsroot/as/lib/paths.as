# aether: 3.0
# Lexical POSIX path operations. These do not access the filesystem, resolve
# symlinks, or grant capabilities. The result is still subject to OS checks.
import strings

def is_abs(path: str) -> bool:
    return strings.starts_with(path, "/")

def _trim_right_slashes(path: str) -> str:
    stop = len(path)
    while stop > 1 and path[stop - 1] == "/":
        stop -= 1
    return path.slice(0, stop)

def basename(path: str) -> str:
    path = _trim_right_slashes(path)
    if path == "/":
        return "/"
    separator = strings.rfind(path, "/")
    return path.slice(separator + 1, len(path))

def dirname(path: str) -> str:
    path = _trim_right_slashes(path)
    separator = strings.rfind(path, "/")
    if separator < 0:
        return "."
    if separator == 0:
        return "/"
    return path.slice(0, separator)

def _name_dot(base: str) -> i64:
    # Leading dots belong to the name: .bashrc and ..foo have no extension.
    first = 0
    while first < len(base) and base[first] == ".":
        first += 1
    dot = strings.rfind(base, ".")
    return -1 if dot < first else dot

def extname(path: str) -> str:
    base = basename(path)
    dot = _name_dot(base)
    return "" if dot < 0 else base.slice(dot, len(base))

def stem(path: str) -> str:
    base = basename(path)
    dot = _name_dot(base)
    return base if dot < 0 else base.slice(0, dot)

def join(a: str, b: str) -> str:
    if a == "" or is_abs(b):
        return normalize(b)
    if b == "":
        return normalize(a)
    return normalize(a + "/" + b)

def split(path: str) -> List[str]:
    out: List[str] = []
    for part in strings.split(path, "/"):
        if part != "":
            out.append(part)
    return out

def _drop_last(parts: List[str]) -> List[str]:
    out: List[str] = []
    for i in range(len(parts) - 1):
        out.append(parts[i])
    return out

def normalize(path: str) -> str:
    absolute = is_abs(path)
    stack: List[str] = []
    for part in strings.split(path, "/"):
        if part == "" or part == ".":
            continue
        if part == "..":
            if len(stack) > 0 and stack[-1] != "..":
                stack = _drop_last(stack)
            elif not absolute:
                stack.append(part)
        else:
            stack.append(part)
    result = strings.join(stack, "/")
    if absolute:
        return "/" + result
    return "." if result == "" else result

def with_ext(path: str, ext: str) -> str:
    if ext != "" and not strings.starts_with(ext, "."):
        ext = "." + ext
    directory = dirname(path)
    base = stem(path) + ext
    return base if directory == "." else join(directory, base)
