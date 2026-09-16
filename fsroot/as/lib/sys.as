# aether: 3.0
# sys -- typed LogitOS services over the native ABI.
#
# The A2 module assumed every string already ended in NUL. A3 strings may be
# interior views, so abi owns path marshalling and _Arguments owns argv copies.
# These are LogitOS services: the language core remains portable, and neither
# this module nor unsafe grants capabilities that the process does not hold.
import abi
from abi import Time, now_s, sys_yield, wait_dns, wait_ping, get_time
from abi import file_read, file_write, file_delete, file_rename, dir_make, dir_count, dir_name
from abi import getcwd, proc_pid, proc_fork, proc_execve, proc_exit, proc_waitpid, cpu_index

NET_TIMEOUT_S: i64 = 5


# ---- files and directories ----
# Keep the original failure sentinel through Optional. Invalid text/arguments
# still raise typed errors; arbitrary binary contents belong in Bytes, not str.
def read_file(path: str) -> Optional[str]:
    return read_file_max(path, 262144)


def read_file_max(path: str, maximum: i64) -> Optional[str]:
    # This retains SYS_READ_FILE's whole-file size limit. On LogitFS a larger
    # file fails; callers wanting a prefix use a scoped file port's read().
    data = buffer(maximum)
    count = file_read(path, data, maximum)
    if count < 0:
        return None
    unsafe:
        return mem2str(data, count)


def write_file[B: ByteStorage](path: str, data: B) -> i64:
    # Text callers explicitly encode with Bytes(text); native byte storage
    # passes directly without a second whole-file copy in this wrapper.
    return file_write(path, data, len(data))


def remove(path: str) -> i64:
    return file_delete(path)


def rename(old: str, new: str) -> i64:
    return file_rename(old, new)


def mkdir(path: str) -> i64:
    return dir_make(path)


def ls(directory: str) -> Optional[List[str]]:
    count = dir_count(directory)
    if count < 0:
        return None
    names: List[str] = []
    data = buffer(64)
    for index in range(count):
        size = dir_name(directory, index, data)
        # -2 denotes a directory, not an error. Enumeration can change between
        # calls; only the ABI's -1 sentinel means that this entry disappeared.
        if size != -1:
            unsafe:
                names.append(mem2cstr(data))
    return names


def chdir(path: str) -> i64:
    return abi.chdir(path)


def cwd() -> Optional[str]:
    data = buffer(128)
    count = getcwd(data, len(data))
    if count < 0:
        return None
    unsafe:
        return mem2str(data, count)


# ---- process ownership ----
class _Arguments:
    values: List[Bytes]
    pointers: Buffer

    def init(self, arguments: List[str]) -> None:
        self.values = []
        self.pointers = buffer(8 * (len(arguments) + 1))
        for index in range(len(arguments)):
            argument = arguments[index]
            if chr(0) in argument:
                raise ValueError("process argument contains an embedded NUL")
            value = Bytes(argument + chr(0))
            self.values.append(value)
            unsafe:
                poke64(addr(self.pointers) + u64(8 * index), i64(addr(value)))
        # buffer() zeroes the final pointer. values is an ordinary GC-scanned
        # field: the raw pointer vector alone cannot keep its pointees alive.


def pid() -> i64:
    return proc_pid()


def spawn(path: str, arguments: List[str]) -> i64:
    if chr(0) in path:
        raise ValueError("process path contains an embedded NUL")
    # Validate and own every argument before fork. A marshalling exception in
    # the child must not accidentally return into the parent's caller logic.
    owned = _Arguments(arguments)
    child = proc_fork()
    if child == 0:
        try:
            proc_execve(path, owned.pointers, 0)
        except Error:
            # Path marshalling can still allocate after fork. A typed failure
            # has the same child-exit contract as a negative execve result.
            pass
        proc_exit(127)
        raise RuntimeError("process exit unexpectedly returned")
    return child


def wait(child: i64) -> i64:
    # waitpid(-1) means ANY child. A failed spawn must not reap an unrelated
    # process merely because the old run() blindly forwarded its sentinel.
    if child <= 0:
        return -1
    status = buffer(4)
    result = proc_waitpid(child, status, 0)
    if result < 0:
        return -1
    unsafe:
        return peek32(addr(status))


def run(path: str, arguments: List[str]) -> i64:
    return wait(spawn(path, arguments))


def exit(code: i64) -> None:
    proc_exit(code)
    raise RuntimeError("process exit unexpectedly returned")


def cpu() -> i64:
    return cpu_index()


# ---- clocks ----
def time() -> Time:
    clock = Time()
    if get_time(clock) < 0:
        raise IOError("wall clock unavailable")
    return clock


def sleep(seconds: i64) -> None:
    if seconds < 0:
        raise ValueError("sleep duration must be nonnegative")
    duration = seconds * 1000
    # The A2 implementation used RTC seconds with a midnight adjustment.
    # Monotonic time also handles waits spanning a day or a wall-clock change.
    start = abi.monotonic_ms()
    if start < 0:
        raise IOError("monotonic clock unavailable")
    while true:
        now = abi.monotonic_ms()
        if now < start:
            raise IOError("monotonic clock failed or moved backwards")
        if now - start >= duration:
            return
        sys_yield()


# ---- network ----
# Polling policy and typed failure/timeout errors come from the ABI module.
# The explicit *_or forms retain the original opt-in sentinel behavior.
def dns(name: str) -> i64:
    return wait_dns(name, NET_TIMEOUT_S)


def dns_or(name: str, default: i64) -> i64:
    try:
        return dns(name)
    except Error:
        return default


def ip_str(ip: i64) -> str:
    return f"{(ip >> 24) & 255}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}"


def ping(ip: i64) -> i64:
    return wait_ping(ip, NET_TIMEOUT_S)


def ping_or(ip: i64, default: i64) -> i64:
    try:
        return ping(ip)
    except Error:
        return default
