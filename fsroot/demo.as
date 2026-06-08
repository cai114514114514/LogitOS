# Code Studio demo - AetherScript
def sq(n):
    return n * n

total = 0
for i in range(8):
    if i == 5:
        break
    total += sq(i)

print("sum of squares:", total)
print("bits:", 255 & 0x0f, 1 << 4, 2 ** 10)
