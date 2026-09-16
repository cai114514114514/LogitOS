# aether: 3.0
# Code Studio demo - AetherScript
# Runs to completion on the host (`as run`); the guest Run in Studio covers a
# small subset (print/if/while/for-range/simple assignment). This demo also
# uses break, +=, a user call and bit operators, so guest Run refuses it by
# name instead of executing it half-way.
def sq(n: i64) -> i64:
    return n * n


def main() -> None:
    # The explicit entry keeps execution separate from module initialization.
    # Locals still infer their types; the public function above states its ABI.
    total = 0
    for i in range(8):
        if i == 5:
            break
        total += sq(i)

    print("sum of squares:", total)
    print("bits:", 255 & 0x0f, 1 << 4, 2 ** 10)
