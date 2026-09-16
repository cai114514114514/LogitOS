"""Deterministic syscall traces for the native ABI polling helpers.

The private runtime supplies clock readings and poll results. Assertions consume
the entire trace, so a skipped yield, an extra poll, or an incorrect timeout
cannot pass merely by printing the expected success message. No network request
is sent by this host test; real kernel calls have separate guest coverage.
"""

from pathlib import Path


def polling_source(work: Path):
    source = ["# aether: 3.0", "import abi", "def main() -> None:"]
    trace = []
    cases = 0

    def expect(expression, calls, result=None, error=None, message=""):
        nonlocal cases
        cases += 1
        trace.extend(calls)
        if error is None:
            source.append(f"    assert {expression} == {result}")
            return
        source.extend([
            "    caught = false",
            "    try:",
            f"        {expression}",
            f"    except {error} as error:",
            f'        assert "{message}" in error.message',
            "        caught = true",
            "    assert caught",
        ])

    clock = "SYS_MONOTONIC_MS"
    pause = "SYS_YIELD"
    protocols = [
        ('abi.wait_dns("local.test", 1)', "SYS_NET_DNS", "SYS_NET_DNS_RESULT", 0, 42),
        ("abi.wait_ping(42, 1)", "SYS_NET_PING", "SYS_NET_PING_RTT", -1, 17),
        ('abi.wait_http("http://local.test", 1)', "SYS_HTTP_GET", "SYS_HTTP_STATUS", 1, 200),
    ]
    for expression, start, poll, pending, success in protocols:
        expect(expression, [(start, 0), (clock, 100), (poll, pending),
                            (clock, 101), (pause, 0), (poll, success)], result=success)
        expect(expression, [(start, -1)], error="IOError", message="cannot start")
        expect(expression, [(start, 0), (clock, 100), (poll, pending), (clock, 1100)],
               error="RuntimeError", message="timed out after 1s")
        expect(expression, [(start, 0), (clock, -1)],
               error="IOError", message="clock unavailable")
        expect(expression, [(start, 0), (clock, 100), (poll, pending), (clock, 99)],
               error="IOError", message="moved backwards")

    expect('abi.wait_dns("local.test", 1)',
           [("SYS_NET_DNS", 0), (clock, 100), ("SYS_NET_DNS_RESULT", 4294967295)],
           error="IOError", message="dns: failed")
    expect('abi.wait_http("http://local.test", 1)',
           [("SYS_HTTP_GET", 0), (clock, 100), ("SYS_HTTP_STATUS", -7)],
           error="IOError", message="http: failed")
    expect('abi.await_(lambda: 0, false, 0, false, false, 0, -1, "local")', [],
           error="ValueError", message="nonnegative")
    expect('abi.await_(lambda: 0, false, 0, false, false, 0, 9223372036854775807, "local")', [],
           error="OverflowError")
    expect('abi.await_(lambda: 7, false, 0, false, false, 0, 0, "local")',
           [(clock, 100)], result=7)
    expect('abi.await_(lambda: 0, false, 0, false, false, 0, 0, "local")',
           [(clock, 100), (clock, 100)], error="RuntimeError", message="timed out after 0s")
    source.append('    print("native ABI polling ok")')
    path = work / "polling.as"
    path.write_text("\n".join(source) + "\n")

    return path, trace_probe(trace), cases


def trace_probe(trace):
    """Consume a complete, independent syscall/result sequence."""
    entries = "\n".join(f"    {{{number}, {result}}}," for number, result in trace)
    return '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>

struct step { int64_t number; int64_t result; };
static const struct step expected[] = {
''' + entries + '''
};
static unsigned seen;

int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    (void)a;
    (void)b;
    (void)c;
    assert(seen < sizeof expected / sizeof *expected);
    const struct step *step = &expected[seen++];
    assert(number == step->number);
    return step->result;
}

__attribute__((destructor)) static void check_count(void)
{
    assert(seen == sizeof expected / sizeof *expected);
}
'''
