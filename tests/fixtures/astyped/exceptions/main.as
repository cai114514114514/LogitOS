# aether: 3.0
import operations

def main() -> None:
    try:
        operations.divide(1, 0)
        print("UNREACHABLE")
    except OverflowError:
        print("WRONG HANDLER")
    except ZeroDivisionError as error:
        assert error.line == 4
        assert error.column > 0
        assert len(error.file) > 0
        print(error.message, error.code)

    # A caught failure must be cleared before a normal call can proceed.
    print(operations.recover(), operations.divide(12, 3))
    count = 0
    for i in range(3):
        try:
            if i == 0:
                continue
            narrow: i8 = 127
            operations.twice(narrow)
        except OverflowError:
            count += 1
            if i == 2:
                break
    print("caught", count)

    try:
        try:
            operations.fail()
        except ValueError as original:
            print(original)
            # The inner handler clears another failure, but bare raise must
            # still use the enclosing handler's original stack record.
            try:
                operations.divide(1, 0)
            except ZeroDivisionError:
                pass
            raise
    except Error as reraised:
        assert reraised.line == 10
        print("reraised", reraised.code)

    try:
        pass
    except Error:
        print("WRONG HANDLER")
    else:
        print("else")
    print("done")
