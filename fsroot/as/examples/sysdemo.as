# sysdemo -- the OS as a scripting surface (M23.5 lib/sys.as).
from sys import read_file, write_file, ls, remove, mkdir, run, time, pid, cwd

# file round-trip
write_file("/docs/sysdemo.txt", "written by AetherScript\n")
back = read_file("/docs/sysdemo.txt")
print("roundtrip:", back.strip())

# directory listing (the file we just wrote must be in it)
names = ls("/docs")
print("ls has it:", "sysdemo.txt" in names)
remove("/docs/sysdemo.txt")

# process control: fork+execve+waitpid -- a script driving real programs
code = run("/bin/echo", ["echo", "spawned-from-script"])
print("spawn exit:", code)

# wall clock + identity
t = time()
print("clock sane:", t.year >= 2024 and t.hour >= 0 and t.hour < 24)
print(f"pid={pid() > 0} cwd={cwd()}")
print("sysdemo ok")
