# aether: 3.0
# Native report probe for the swap gate, compiled on the host like mempress.
# SYS_MEMINFO (94), MMCTL_REPORT (1): include/abi/logit_abi.h and
# c/kernel/mm/mmsys.c. Keep the syscall result as well as its kernel report
# so a refused operation cannot look like a successful empty report.
def main() -> None:
    unsafe:
        print(syscall(94, 0, 1, 0))
