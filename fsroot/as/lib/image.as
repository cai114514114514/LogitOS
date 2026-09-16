# aether: 3.0
# image -- decode image files into native, GC-owned RGBA storage.
#
# The kernel returns only success/failure, so stat and format sniffing still
# provide the useful error context. A failed decode does not prove that a file
# is missing or that its dimensions alone exceeded the budget.
from abi import Imgreq, Stat, img_decode, stat, file_read
from sys import cwd
import paths

# A2 chose these limits against the VM's 24 MiB arena. Preserve the existing
# 5/10/20 MiB retry policy; the native implementation drops each failed attempt
# by returning from its helper before collecting and allocating the next one.
BUDGET0: i64 = 5 * 1024 * 1024
BUDGET_MAX: i64 = 20 * 1024 * 1024

# The original library used whole-file reads, not descriptor pread. Keep that
# cost bounded: successful sniffed decoding reads twice (here and in kernel).
# A file above this limit may still decode; its format is reported as unknown.
SNIFF_MAX: i64 = 4 * 1024 * 1024
UNSNIFFED: str = "?"


def _check_count[B: ByteStorage](data: B, count: i64) -> None:
    if count < 0 or count > len(data):
        raise ValueError("image byte count exceeds backing storage")


def magic[B: ByteStorage](data: B, count: i64) -> str:
    _check_count(data, count)
    if count >= 8 and data[0] == 0x89 and data[1] == 0x50 and data[2] == 0x4E and data[3] == 0x47:
        return "PNG"
    if count >= 6 and data[0] == 0x47 and data[1] == 0x49 and data[2] == 0x46:
        return "GIF"
    if count >= 3 and data[0] == 0xFF and data[1] == 0xD8 and data[2] == 0xFF:
        return "JPEG"
    if count >= 2 and data[0] == 0x42 and data[1] == 0x4D:
        return "BMP"
    if count >= 4 and data[0] == 0 and data[1] == 0 and data[2] == 1 and data[3] == 0:
        return "ICO"
    if count >= 12 and data[0] == 0x52 and data[1] == 0x49 and data[2] == 0x46 and data[3] == 0x46:
        if data[8] == 0x57 and data[9] == 0x45 and data[10] == 0x42 and data[11] == 0x50:
            return "WebP"
    # This is only a hint, not an XML parser. Comments, BOM and leading spaces
    # may conceal SVG; the kernel decoder remains the authority on decoding.
    if count >= 1 and data[0] == 0x3C:
        return "SVG/XML"
    return ""


def hexbytes[B: ByteStorage](data: B, count: i64, limit: i64) -> str:
    _check_count(data, count)
    digits = "0123456789ABCDEF"
    out = ""
    stop = count if limit > count else limit
    for index in range(stop):
        byte = data[index]
        out += digits[(byte >> 4) & 15] + digits[byte & 15] + " "
    return out


class Image:
    path: str
    format: str
    w: i64
    h: i64
    rgba: Buffer

    def init(self, path: str, format: str, w: i64, h: i64, rgba: Buffer) -> None:
        # Division validates capacity without overflowing w*h*4 first. Keep the
        # actual pixel owner as a field, never only its raw address.
        if w <= 0 or h <= 0 or w > len(rgba) / 4 / h:
            raise ValueError("image dimensions do not fit RGBA storage")
        self.path = path
        self.format = format
        self.w = w
        self.h = h
        self.rgba = rgba

    def pixels(self) -> i64:
        return self.w * self.h

    def at(self, x: i64, y: i64) -> i64:
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            raise IndexError(f"image.at({x},{y}): outside a {self.w}x{self.h} image")
        index = (y * self.w + x) * 4
        red = self.rgba[index]
        green = self.rgba[index + 1]
        blue = self.rgba[index + 2]
        alpha = self.rgba[index + 3]
        return (red << 24) | (green << 16) | (blue << 8) | alpha


def stat_of(path: str) -> Optional[Stat]:
    record = Stat()
    if stat(path, record, len(record)) != 0:
        return None
    return record


def is_dir(record: Stat) -> bool:
    return (i64(record.mode) & LST_IFMT) == LST_IFDIR


def _absolute(path: str) -> str:
    if len(path) == 0 or chr(0) in path:
        raise ValueError("image path is empty or contains NUL")
    if paths.is_abs(path):
        return paths.normalize(path)
    directory = cwd()
    if directory is None:
        raise IOError("cannot resolve the current directory")
    return paths.normalize(paths.join(directory, path))


def refusal(path: str) -> str:
    grant = caps()
    bits = grant.bits()
    if (bits & CAP_FS_READ) == 0:
        return f"{path}: refused -- this process does not hold CAP_FS_READ"
    if (bits & CAP_RAW) == 0:
        return f"{path}: refused -- this process does not hold CAP_RAW"
    # A2 documented a VM-only scope check here. This native preflight names a
    # refusal; it neither grants permissions nor replaces kernel authorization.
    # Resolve relative paths first because Cap.scope accepts absolute paths.
    try:
        grant.scope(_absolute(path))
    except Error:
        return f"{path}: refused -- invalid path or outside this process's capability scope ({grant.path()})"
    return ""


def probe(path: str) -> List[Any]:
    # Retain the original [format, size] result shape. Its heterogeneous values
    # explicitly use Any; callers cast each field before using it in A3.
    record = stat_of(path)
    if record is None:
        raise IOError(f"{path}: no such file (stat refused it)")
    if is_dir(record):
        raise IOError(f"{path}: is a directory, not an image")
    size = i64(record.size)
    if size == 0:
        raise ValueError(f"{path}: the file is empty (0 bytes)")
    if size > SNIFF_MAX:
        return [Any(UNSNIFFED), Any(size)]
    data = buffer(size)
    count = file_read(path, data, size)
    if count < 0:
        raise IOError(f"{path}: {size} bytes on disk, but the read was refused")
    format = magic(data, count)
    if format == "":
        raise ValueError(f"{path}: not an image -- the first bytes are "
                         + hexbytes(data, count, 4)
                         + "and match no format this kernel decodes (PNG GIF JPEG BMP ICO WebP)")
    if format == "SVG/XML":
        raise ValueError(f"{path}: looks like SVG/XML, which the KERNEL decoder does not handle")
    return [Any(format), Any(size)]


def _attempt(path: str, c_path: Bytes, format: str, budget: i64) -> Optional[Image]:
    data = buffer(budget)
    request = Imgreq()
    unsafe:
        request.path = addr(c_path)
        request.rgba = addr(data)
    request.max = i32(budget)
    request.w = 0
    request.h = 0
    if img_decode(request) != 0:
        return None
    width = i64(request.w)
    height = i64(request.h)
    if width <= 0 or height <= 0 or width > budget / 4 / height:
        raise ValueError(f"{path}: the kernel reported {width}x{height}, which does "
                         + f"not fit the {budget}-byte buffer it was given")
    return Image(path, format, width, height, data)


def decode(path: str) -> Image:
    reason = refusal(path)
    if reason != "":
        raise PermissionError(reason)
    absolute = _absolute(path)
    information = probe(absolute)
    format = cast[str](information[0])
    size = cast[i64](information[1])
    # A3 paths may be interior views. Keep the terminated copy rooted across
    # every allocation and retry; the request's p field is not a GC root.
    c_path = Bytes(absolute + chr(0))
    budget = BUDGET0
    while budget <= BUDGET_MAX:
        try:
            result = _attempt(path, c_path, format, budget)
        except MemoryError:
            # The guest arena is shared with the caller's live images. A
            # 20 MiB retry cannot fit beside an existing 5 MiB image in its
            # default 24 MiB arena. Preserve the exception type and path;
            # allocation failure is not proof that the decoder rejected data.
            raise MemoryError(f"{path}: cannot allocate {budget} bytes for image decoding")
        if result is not None:
            return result
        gc_collect()
        budget *= 2
    if format == UNSNIFFED:
        raise IOError(f"{path}: {size} bytes -- too large to read back for a format check "
                      + "(whole-file read limit), and SYS_IMG_DECODE refused it")
    raise IOError(f"{path}: {format}, {size} bytes -- it decodes to more than "
                  + f"{BUDGET_MAX} bytes of pixels, or the {format} decoder rejected it "
                  + "(SYS_IMG_DECODE answers -1 with no reason code)")


def fit(iw: i64, ih: i64, bx: i64, by: i64, bw: i64, bh: i64) -> List[i64]:
    if iw <= 0 or ih <= 0:
        raise ValueError(f"image.fit: a {iw}x{ih} image has no aspect ratio")
    if bw <= 0 or bh <= 0:
        return [bx, by, 0, 0]
    # Compare before division to avoid rounding the aspect-ratio decision.
    # A3 checked arithmetic reports dimensions outside the i64 product range.
    width = bw
    height = bh
    if iw * bh <= bw * ih:
        width = iw * bh / ih
    else:
        height = ih * bw / iw
    if width < 1:
        width = 1
    if height < 1:
        height = 1
    return [bx + (bw - width) / 2, by + (bh - height) / 2, width, height]


def centre(iw: i64, ih: i64, bx: i64, by: i64, bw: i64, bh: i64) -> List[i64]:
    # Actual-size display preserves the source size, even when the destination
    # clips it. Shrinking here would erase the viewer's fit/actual distinction.
    return [bx + (bw - iw) / 2, by + (bh - ih) / 2, iw, ih]
