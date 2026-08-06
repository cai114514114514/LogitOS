# sys -- direct Logit system access from AetherScript (M23.5).
# Pure AetherScript over syscall()/buffer()/addr(): files, dirs, processes
# (fork+execve+waitpid), time, network. This is the surface a sandboxed
# scripting language doesn't get: the OS, first-class.
#   from sys import read_file, write_file, ls, run, time
# Strings passed to the kernel are NUL-terminated (ObjStr always is); buffers
# come from buffer(), which the collector owns -- there is nothing to free, and
# nothing to leak when a call in between raises. Kernel structs come from `abi`,
# whose field offsets are generated from include/abi/logit_abi.h and checked
# against it by the compiler that builds /bin/as.

from abi import Time, now_s, sys_yield, wait_dns, wait_ping, get_time
from abi import file_read, file_write, file_delete, file_rename, dir_make, dir_count, dir_name
from abi import getcwd, chdir, proc_pid, proc_fork, proc_execve, proc_exit, proc_waitpid, cpu_index

# How long a network operation is given before it is called a timeout. Seconds,
# because it is real elapsed time: the loops these replaced counted 500 polls,
# which is a different amount of time on every machine and under every load.
NET_TIMEOUT_S = 5

# ---- files ----
# The syscall invocations themselves are generated (abi): the convention --
# which argument is the string, which is the buffer -- is stated once in
# include/abi/logit_calls.abi, not at each of these call sites.
def read_file(path):
    return read_file_max(path, 262144)

def read_file_max(path, max):
    buf = buffer(max)
    n = file_read(path, buf, max)
    return mem2str(buf, n) if n >= 0 else nil

def write_file(path, data):
    return file_write(path, data, len(data))

def remove(path):
    return file_delete(path)

def rename(old, new):
    return file_rename(old, new)

def mkdir(path):
    return dir_make(path)

# ---- directories ----
def ls(dir):
    n = dir_count(dir)
    if n < 0:
        return nil
    out = []
    buf = buffer(64)
    for i in range(n):
        sz = dir_name(dir, i, buf)
        if sz != -1:
            out.append(mem2cstr(buf))
    return out

def cwd():
    buf = buffer(128)
    n = getcwd(buf, 128)
    return mem2str(buf, n) if n >= 0 else nil

# chdir is imported from abi unchanged: the generated wrapper IS the convention.

# ---- processes ----
def pid():
    return proc_pid()

# Build a NULL-terminated char*[] from a list of strings, in script memory.
# The list MUST stay live while the kernel reads the array (it does: execve
# copies before returning) -- the addr()s point into the strings' own bytes.
def _argv(args):
    n = len(args)
    pv = buffer(8 * (n + 1))
    for i in range(n):
        poke64(addr(pv) + 8 * i, addr(args[i]))
    poke64(addr(pv) + 8 * n, 0)
    return pv

# spawn(path, args) -> child pid (args = full argv incl. argv[0]).
# fork + execve in pure script: the child VM replaces itself with the program.
def spawn(path, args):
    p = proc_fork()
    if p == 0:
        pv = _argv(args)
        proc_execve(path, pv, 0)
        proc_exit(127)                  # execve failed
    return p

def wait(p):
    st = buffer(4)
    rc = proc_waitpid(p, st, 0)
    return peek32(addr(st)) if rc >= 0 else -1

def run(path, args):
    return wait(spawn(path, args))

def exit(code):
    proc_exit(code)

def cpu():
    return cpu_index()

# ---- time ----
# -> a Time layout: .year .month .day .hour .minute .second .weekday
# (RTC wall clock). The kernel writes struct logit_time straight into it.
def time():
    t = Time()
    get_time(t)
    return t

def sleep(secs):
    start = now_s()
    while true:
        now = now_s()
        if now < start:
            now = now + 86400            # the RTC clock wrapped past midnight
        if now - start >= secs:
            return nil
        sys_yield()

# ---- network ----
# dns() and ping() RAISE on failure and on timeout, and those are two different
# messages. They used to answer 0 and -1 respectively for both -- and for "no
# network interface" as well, so three outcomes shared one value and a caller
# could not tell them apart. The waiting itself is generated from
# include/abi/logit_calls.abi, which is where the protocol (0 means pending,
# 0xFFFFFFFF means failed) is now stated.
def dns(name):
    return wait_dns(name, NET_TIMEOUT_S)

# The old shape, for callers that would rather have a sentinel than a handler.
def dns_or(name, default):
    try:
        return dns(name)
    except e:
        return default

def ip_str(ip):
    return f"{(ip >> 24) & 255}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}"

def ping(ip):
    return wait_ping(ip, NET_TIMEOUT_S)

def ping_or(ip, default):
    try:
        return ping(ip)
    except e:
        return default
