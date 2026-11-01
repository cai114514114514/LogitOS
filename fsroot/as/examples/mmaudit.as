# aether: 3.0
# Precompiled audit probe for the swap gate. The old shell-generated AS2
# expression cannot run after removal of that engine. SYS_MEMINFO (94),
# MMCTL_AUDIT (2): include/abi/logit_abi.h and c/kernel/mm/mmsys.c.
# The kernel prints the ownership/reverse-map audit; retain its return value.
def main() -> None:
    unsafe:
        print(syscall(94, 0, 2, 0))
