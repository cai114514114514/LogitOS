# aether: 3.0
def main() -> None:
    with owner = region(2):
        with moved = owner.move():
            moved[2] = 1
