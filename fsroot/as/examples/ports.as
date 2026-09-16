# aether: 3.0
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
#      of descriptors forces a collection. It does; see acquire_fd in legacy/ports.c.


# A3 correction to the historical backstop claim above: file owners cannot be
# dropped outside a with scope. Keep the 64-open workload, but prove each scope
# closes immediately, before GC, rather than inventing GC-finalization counts.
# Optional paths make this same shipped source runnable on the build host.


def count_lines(path: str) -> i64:
    try:
        with file = open(path):
            count = 0
            for line in file:
                count += 1
            return count
    except IOError:
        return -1


def main() -> None:
    arguments = args()
    source = arguments[1] if len(arguments) > 1 else "/usr/as/examples/hello.as"
    destination = arguments[2] if len(arguments) > 2 else "/ports_out.txt"
    print("PORTS: start")
    print("ports lines:", count_lines(source) > 0)

    # cat checks that bytes crossed a real pipe, independent of wc options.
    pipeline = run("echo", "alpha") |> run("cat")
    print("ports pipe:", pipeline.out().strip())

    run("echo", "redirected") -> destination
    with file = open(destination):
        line = file.line()
        assert line is not None
        print("ports redir:", line)

    before = port_stats()
    completed = 0
    for index in range(64):
        with file = open(source):
            assert port_stats()["open"] == before["open"] + 1
            completed += 1
        assert port_stats()["open"] == before["open"]
    after = port_stats()
    print("ports scopes:", completed, after["closed"] - before["closed"] == 64)
    assert after["finalized"] == 0 and after["orphans"] == 0

    # Borrowed wrappers release no OS descriptor and do not count as closes.
    for index in range(8):
        with console = port(1):
            assert console.fd() == 1
    print("ports borrow:", port_stats()["closed"] == after["closed"])
    print("ports ok")
