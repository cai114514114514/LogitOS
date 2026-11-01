# aether: 3.0
# Native out-of-memory report probe for the OOM gate, compiled on the host like
# mmrep. The old shell-generated AS2 expression cannot run after removal of that
# engine. SYS_MEMINFO (94), MMCTL_OOM (5): include/abi/logit_abi.h and
# c/kernel/mm/mmsys.c. The kernel prints the [oomstat] counters; keep the
# syscall result so a refused operation cannot look like an empty report.
def main() -> None:
    unsafe:
        print(syscall(94, 0, 5, 0))
