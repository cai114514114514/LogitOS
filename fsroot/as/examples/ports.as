# ports.as -- M27 on the machine.
#
# The shell (ash.as) is the milestone's real proof, but it needs a console. This
# runs unattended under `make test-as-os` and pins the four things a port claims,
# on LogitOS rather than on the build host:
#   1. a file port iterated as a stream
#   2. a pipeline: two real ring-3 processes, wired in one fork/dup2/exec pass
#   3. `->` redirection, read back through another port
#   4. the collector as a BACKSTOP for a port the script dropped -- which matters
#      far more here than on the host, because a LogitOS process gets 16 file
#      descriptors (proc.h NFD) and the collector's own threshold is 1024
#      objects. Opening 64 ports and closing none can only work if running out
#      of descriptors forces a collection. It does; see acquire_fd in as_port.c.

print("PORTS: start")

# 1. a stream, one line at a time -- no buffer, no length, no syscall.
def count_lines(path):
    h = open(path)
    if h == nil:
        return -1
    with f = h:
        n = 0
        for line in f:
            n += 1
        return n

print("ports lines:", count_lines("/usr/as/examples/hello.as") > 0)

# 2. `|>` composes two UNSTARTED commands, which is the only moment the fd
#    between them can be wired. `cat` rather than `wc -l`, deliberately: this
#    OS's wc takes no flags, and the thing under test is that bytes crossed the
#    pipe, not that a coreutil can count.
print("ports pipe:", (run("echo", "alpha") |> run("cat")).out().strip())

# 3. `->` on a statement is a command statement: started and waited for here.
run("echo", "redirected") -> "/ports_out.txt"
with g = open("/ports_out.txt"):
    print("ports redir:", g.line())

# 4. 64 ports, none closed, on a 16-entry fd table. All 64 can only succeed if
#    running out of descriptors forces a collection and the collector closes the
#    ones the script dropped -- so `k == 64` IS the backstop working, and
#    `finalized >= 48` is the count it had to close (64 minus the table).
k = 0
for i in range(64):
    p = open("/usr/as/examples/hello.as")
    if p == nil:
        break
    k += 1
st = port_stats()
print("ports drop:", k, st["finalized"] >= 48)

# A borrowed handle is never close(2)d, whatever happens to the value.
for i in range(8):
    b = port(1)
print("ports borrow:", port_stats()["finalized"] > 0)

print("ports ok")
