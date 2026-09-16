# aether: 3.0
def main() -> None:
    errors = 0
    try:
        run([])
    except ValueError:
        errors += 1
    try:
        run("echo", "bad" + chr(0) + "argument")
    except ValueError:
        errors += 1
    command = run("echo")
    try:
        command = command |> command
    except ValueError:
        errors += 1
    linked = run("cat")
    pipeline = command |> linked
    try:
        linked.wait()
    except ValueError:
        errors += 1
    try:
        pipeline = pipeline |> linked
    except ValueError:
        errors += 1
    command = run("echo") -> args()[1] + "/errors-output"
    try:
        command = command -> args()[1] + "/another-output"
    except ValueError:
        errors += 1
    try:
        command.out()
    except ValueError:
        errors += 1
    task = run("cat") <- args()[1] + "/missing-native-input"
    try:
        task.wait()
    except IOError:
        errors += 1
    command = run("echo", "consumed")
    assert command.out() == "consumed\n"
    try:
        command.out()
    except ValueError:
        errors += 1
    try:
        command = command |> run("cat")
    except ValueError:
        errors += 1
    command = run("cat") <- args()[1] + "/command-output.txt"
    with process = command:
        try:
            with duplicate = command:
                pass
        except ValueError:
            errors += 1
    assert errors == 11
    print("native command errors ok")
