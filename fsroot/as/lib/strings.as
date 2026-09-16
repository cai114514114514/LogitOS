# aether: 3.0
# Text helpers use byte offsets, preserving the A2 API for UTF-8 and embedded
# NULs. slice() borrows immutable text storage; native GC keeps its owner alive.
# Public annotations are the actual compiler contract, not editor-only hints.

def contains(s: str, needle: str) -> bool:
    return needle in s

def starts_with(s: str, prefix: str) -> bool:
    return len(prefix) <= len(s) and s.slice(0, len(prefix)) == prefix

def ends_with(s: str, suffix: str) -> bool:
    return len(suffix) <= len(s) and s.slice(len(s) - len(suffix), len(s)) == suffix

def slice(s: str, start: i64, stop: i64) -> str:
    # Negative bounds are relative to the end; overshooting bounds are clamped.
    # This replaces A2's repeated byte concatenation with an immutable view.
    return s.slice(start, stop)

def find(s: str, needle: str) -> i64:
    return s.find(needle)

def rfind(s: str, needle: str) -> i64:
    # Empty needles match at the end, unlike find(), which matches at zero.
    i = len(s) - len(needle)
    while i >= 0:
        if s.slice(i, i + len(needle)) == needle:
            return i
        i -= 1
    return -1

def repeat(s: str, n: i64) -> str:
    return s * n

def count(s: str, needle: str) -> i64:
    if needle == "":
        raise ValueError("count() needs a non-empty needle")
    matches = 0
    i = 0
    while i <= len(s) - len(needle):
        if s.slice(i, i + len(needle)) == needle:
            matches += 1
            i += len(needle)
        else:
            i += 1
    return matches

def join(parts: List[str], sep: str) -> str:
    # The primitive computes total size first and allocates once. Repeated
    # concatenation copies the growing prefix for every element (quadratic).
    return sep.join(parts)

def split(s: str, sep: str) -> List[str]:
    if sep == "":
        raise ValueError("split() needs a non-empty separator")
    # The primitive retains empty boundary fields and roots the result during
    # each backing-buffer growth, including when it is called inside another call.
    return s.split(sep)

def _space(ch: str) -> bool:
    return ch == " " or ch == "\t" or ch == "\n" or ch == "\r"

def lstrip(s: str) -> str:
    i = 0
    while i < len(s) and _space(s[i]):
        i += 1
    return s.slice(i, len(s))

def rstrip(s: str) -> str:
    i = len(s) - 1
    while i >= 0 and _space(s[i]):
        i -= 1
    return s.slice(0, i + 1)

def strip(s: str) -> str:
    return rstrip(lstrip(s))

def replace(s: str, old: str, new: str) -> str:
    if old == "":
        raise ValueError("replace() needs a non-empty old string")
    return join(split(s, old), new)

def lines(s: str) -> List[str]:
    return split(replace(s, "\r\n", "\n"), "\n")

def words(s: str) -> List[str]:
    # Preserve A2's literal-space splitting. Tabs/newlines are stripped only
    # at the edges; changing this to Unicode whitespace needs a separate API.
    out: List[str] = []
    for part in split(strip(s), " "):
        if part != "":
            out.append(part)
    return out

def _padding(s: str, width: i64, fill: str) -> str:
    if fill == "":
        raise ValueError("padding needs a non-empty fill string")
    if width <= len(s):
        return ""
    missing = width - len(s)
    # A2 repeats the whole fill, even when that overshoots width. Compute the
    # ceiling without missing + len(fill) - 1, which could overflow i64.
    copies = missing / len(fill)
    if missing % len(fill) != 0:
        copies += 1
    return fill * copies

def pad_left(s: str, width: i64, fill: str) -> str:
    return _padding(s, width, fill) + s

def pad_right(s: str, width: i64, fill: str) -> str:
    return s + _padding(s, width, fill)
