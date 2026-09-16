# aether: 3.0
# Catch a declared exception type and an arithmetic exception from native code.
# Error.message keeps the original message; printing Error also includes its kind.

def main() -> None:
    try:
        raise ValueError("boom")
    except ValueError as error:
        print("caught:", error.message)

    try:
        value = 1 / 0
    except ZeroDivisionError as error:
        print("runtime caught")
    print("exc ok")
