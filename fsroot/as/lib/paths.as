# paths -- tiny POSIX-style path helpers

import strings

def is_abs(path):
    return strings.starts_with(path, "/")

def _trim_right_slashes(path):
    if path == "/":
        return path
    i = len(path) - 1
    while i > 0 and path[i] == "/":
        i = i - 1
    return strings.slice(path, 0, i + 1)

def basename(path):
    path = _trim_right_slashes(path)
    if path == "/":
        return "/"
    i = strings.rfind(path, "/")
    if i < 0:
        return path
    return strings.slice(path, i + 1, len(path))

def dirname(path):
    path = _trim_right_slashes(path)
    if path == "/":
        return "/"
    i = strings.rfind(path, "/")
    if i < 0:
        return "."
    if i == 0:
        return "/"
    return strings.slice(path, 0, i)

def _name_dot(base):                # index of the extension dot, or -1 (leading dots don't count)
    j = 0
    while j < len(base) and base[j] == ".":
        j = j + 1
    i = strings.rfind(base, ".")
    if i < j:                       # no dot, or only leading dots (".bashrc", "..", "..foo")
        return -1
    return i

def extname(path):
    base = basename(path)
    i = _name_dot(base)
    if i < 0:
        return ""
    return strings.slice(base, i, len(base))

def stem(path):
    base = basename(path)
    i = _name_dot(base)
    if i < 0:
        return base
    return strings.slice(base, 0, i)

def join(a, b):
    if a == "" or is_abs(b):
        return normalize(b)
    if b == "":
        return normalize(a)
    if strings.ends_with(a, "/"):
        return normalize(a + b)
    return normalize(a + "/" + b)

def split(path):
    raw = strings.split(path, "/")
    out = []
    for part in raw:
        if part != "":
            out.append(part)
    return out

def _drop_last(xs):
    out = []
    i = 0
    while i + 1 < len(xs):
        out.append(xs[i])
        i = i + 1
    return out

def normalize(path):
    abs = is_abs(path)
    parts = strings.split(path, "/")
    stack = []
    for part in parts:
        if part == "" or part == ".":
            continue
        if part == "..":
            if len(stack) > 0 and stack[len(stack) - 1] != "..":
                stack = _drop_last(stack)
            else:
                if not abs:
                    stack.append(part)
        else:
            stack.append(part)
    out = strings.join(stack, "/")
    if abs:
        out = "/" + out
    if out == "":
        if abs:
            return "/"
        return "."
    return out

def with_ext(path, ext):
    if ext != "" and not strings.starts_with(ext, "."):
        ext = "." + ext
    dir = dirname(path)
    base = stem(path) + ext
    if dir == ".":
        return base
    return join(dir, base)
