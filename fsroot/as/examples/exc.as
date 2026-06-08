# exceptions demo (M22.4): raise+catch, and catch a built-in runtime error
try:
    raise "boom"
except e:
    print("caught:", e)
try:
    x = 1 / 0
except e:
    print("runtime caught")
print("exc ok")
