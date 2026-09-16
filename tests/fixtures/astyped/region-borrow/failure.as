# aether: 3.0
def main() -> None:
    with owner = region(2):
        with view = owner.borrow(0, 2):
            view[2]
