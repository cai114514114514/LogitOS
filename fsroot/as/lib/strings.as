# strings -- pure AetherScript string helpers
#   from strings import slice, split, join, strip, replace

def contains(s, needle):
    return needle in s

def starts_with(s, prefix):
    if len(prefix) > len(s):
        return false
    i = 0
    while i < len(prefix):
        if s[i] != prefix[i]:
            return false
        i = i + 1
    return true

def ends_with(s, suffix):
    if len(suffix) > len(s):
        return false
    off = len(s) - len(suffix)
    i = 0
    while i < len(suffix):
        if s[off + i] != suffix[i]:
            return false
        i = i + 1
    return true

def slice(s, start, stop):
    if start < 0:
        start = len(s) + start
    if stop < 0:
        stop = len(s) + stop
    if start < 0:
        start = 0
    if stop > len(s):
        stop = len(s)
    out = ""
    i = start
    while i < stop:
        out = out + s[i]
        i = i + 1
    return out

def find(s, needle):
    if len(needle) == 0:
        return 0
    i = 0
    while i + len(needle) <= len(s):
        j = 0
        ok = true
        while j < len(needle) and ok:
            if s[i + j] != needle[j]:
                ok = false
            j = j + 1
        if ok:
            return i
        i = i + 1
    return -1

def rfind(s, needle):
    if len(needle) == 0:
        return len(s)
    i = len(s) - len(needle)
    while i >= 0:
        j = 0
        ok = true
        while j < len(needle) and ok:
            if s[i + j] != needle[j]:
                ok = false
            j = j + 1
        if ok:
            return i
        i = i - 1
    return -1

def repeat(s, n):
    out = ""
    i = 0
    while i < n:
        out = out + s
        i = i + 1
    return out

def count(s, needle):
    if len(needle) == 0:
        raise "count() needs a non-empty needle"
    n = 0
    i = 0
    while i + len(needle) <= len(s):
        if slice(s, i, i + len(needle)) == needle:
            n = n + 1
            i = i + len(needle)
        else:
            i = i + 1
    return n

def join(parts, sep):
    out = ""
    i = 0
    while i < len(parts):
        if i > 0:
            out = out + sep
        out = out + parts[i]
        i = i + 1
    return out

def split(s, sep):
    if len(sep) == 0:
        raise "split() needs a non-empty separator"
    out = []
    start = 0
    i = 0
    while i + len(sep) <= len(s):
        j = 0
        ok = true
        while j < len(sep) and ok:
            if s[i + j] != sep[j]:
                ok = false
            j = j + 1
        if ok:
            out.append(slice(s, start, i))
            i = i + len(sep)
            start = i
        else:
            i = i + 1
    out.append(slice(s, start, len(s)))
    return out

def _space(ch):
    return ch == " " or ch == "\t" or ch == "\n" or ch == "\r"

def lstrip(s):
    i = 0
    while i < len(s) and _space(s[i]):
        i = i + 1
    return slice(s, i, len(s))

def rstrip(s):
    i = len(s) - 1
    while i >= 0 and _space(s[i]):
        i = i - 1
    return slice(s, 0, i + 1)

def strip(s):
    return rstrip(lstrip(s))

def replace(s, old, new):
    if len(old) == 0:
        raise "replace() needs a non-empty old string"
    out = ""
    i = 0
    while i < len(s):
        if i + len(old) <= len(s) and slice(s, i, i + len(old)) == old:
            out = out + new
            i = i + len(old)
        else:
            out = out + s[i]
            i = i + 1
    return out

def lines(s):
    return split(replace(s, "\r\n", "\n"), "\n")

def words(s):
    parts = split(strip(s), " ")
    out = []
    for p in parts:
        if p != "":
            out.append(p)
    return out

def pad_left(s, width, fill):
    if fill == "":
        raise "pad_left() needs a fill string"
    while len(s) < width:
        s = fill + s
    return s

def pad_right(s, width, fill):
    if fill == "":
        raise "pad_right() needs a fill string"
    while len(s) < width:
        s = s + fill
    return s
